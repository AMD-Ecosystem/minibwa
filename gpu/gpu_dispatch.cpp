// SPDX-FileCopyrightText: Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//
// miniBWA GPU-alignment dispatch bridge implementation (trial transplant).
//
// Wraps ksw_extd2_gpu_batch_* (transplanted from AMD-AIOSS/minimap2). The batch
// builds its own 5x5 scoring matrix from the scalars via ksw_gen_ts_mat, which
// for the non-methylation case is byte-identical to miniBWA's ksw_gen_nt4_mat,
// so no matrix marshalling is needed. Diagnostic counter proves GPU dispatch.

#include <atomic>
#include <vector>
#include <algorithm>
#include <hip/hip_runtime.h>
#include "gpu/include/gpu_dispatch.h"
#include "gpu/include/ksw2_gpu_batch.h"

static std::atomic<long> g_dispatched{0};

struct mb_gpu_accum_s {
	gpu_batch_t *batch;
};

// ksw_gen_ts_mat is defined in ksw2_gpu_batch.cpp's expectations as an extern
// "C" symbol. miniBWA has no such standalone function (it inlines
// ksw_gen_nt4_mat in ksw2.h), so provide the minimap2-equivalent here; for the
// non-methylation short-read path it produces the identical matrix.
extern "C" void ksw_gen_simple_mat(int m, int8_t *mat, int8_t a, int8_t b, int8_t sc_ambi)
{
	int i, j;
	a = a < 0 ? -a : a;
	b = b > 0 ? -b : b;
	sc_ambi = sc_ambi > 0 ? -sc_ambi : sc_ambi;
	for (i = 0; i < m - 1; ++i) {
		for (j = 0; j < m - 1; ++j)
			mat[i * m + j] = (i == j) ? a : b;
		mat[i * m + m - 1] = sc_ambi;
	}
	for (j = 0; j < m; ++j)
		mat[(m - 1) * m + j] = sc_ambi;
}

extern "C" mb_gpu_accum_t *mb_gpu_accum_init(int8_t a, int8_t b, int8_t b_ts, int8_t b_ambi,
                                             int8_t q, int8_t e, int8_t q2, int8_t e2,
                                             int w, int zdrop, int max_alignments)
{
	(void)b_ts; // amd-integration-2.31 batch_init has no transition arg (uses
	            // ksw_gen_simple_mat); miniBWA short-read default b_ts==0 anyway.
	if (max_alignments <= 0) max_alignments = 4096;
	mb_gpu_accum_t *acc = new (std::nothrow) mb_gpu_accum_s();
	if (!acc) return nullptr;
	// avail_mem is the per-batch VRAM budget that gates the auto-flush; 0 means
	// "flush after every add" (current_mem > 0 is always true), so derive a real
	// budget from device free memory like minimap2's gpu_accum does.
	size_t free_mem = 0, total_mem = 0;
	if (hipMemGetInfo(&free_mem, &total_mem) != hipSuccess || free_mem == 0)
		free_mem = (size_t)8 << 30; // 8 GiB fallback
	// Conservative fraction so a single-thread trial never over-commits VRAM.
	size_t avail_mem = (size_t)(free_mem * 0.25);
	// sc_ambi = b_ambi (miniBWA's ambiguous-base score).
	acc->batch = ksw_extd2_gpu_batch_init(avail_mem, max_alignments,
	                                      q, e, q2, e2, w, zdrop,
	                                      a, b, b_ambi,
	                                      nullptr, nullptr);
	if (!acc->batch) { delete acc; return nullptr; }
	return acc;
}

extern "C" int mb_gpu_accum_add(mb_gpu_accum_t *acc,
                                int qlen, const uint8_t *qseq,
                                int tlen, const uint8_t *tseq,
                                int w, int end_bonus, int zdrop, int flag,
                                ksw_extz_t *ez)
{
	if (!acc || !acc->batch) return -1;

	// Query-orientation contract of the transplanted multialign kernel.
	//
	// The kernel is minimap2's ksw_extd2_gpu_multialign, so it expects exactly
	// the sequence orientation minimap2's gpu_accum feeds it. That orientation
	// differs from miniBWA's ksw_extd2_sse convention ONLY in the two
	// extension-mode calls (KSW_EZ_EXTZ_ONLY); gap-fill feeds a forward query in
	// both and needs no change.
	//
	//                minimap2 (kernel-validated)      miniBWA align.c
	//   gap-fill     fwd query, fwd target            fwd query, fwd target
	//   left  ext    fwd query, rev target  (REV_CIGAR) rev query, rev target
	//   right ext    rev query, fwd target            fwd query, fwd target
	//
	// In BOTH extension cases the kernel wants the query in the OPPOSITE
	// orientation to the one miniBWA passes (left-ext: miniBWA reversed it,
	// minimap2 keeps it forward; right-ext: minimap2 reverses it, miniBWA keeps
	// it forward). The CPU ksw_extd2_sse hides this because it always rebuilds a
	// reversed qr[] internally; the multialign kernel instead reads the query
	// inline as query[(qlen-1-r)+t] and relies on the caller's orientation.
	// Feeding miniBWA's orientation straight through therefore double-reverses
	// (left-ext) or fails to reverse (right-ext) the query, and extension DP
	// terminates almost immediately (observed left-ext: 3M/max=3 vs CPU
	// 26M4I/max=30).
	//
	// So reverse the query for every EXTZ_ONLY task and leave the target as
	// miniBWA passed it (already reversed for left-ext, forward for right-ext) --
	// which is precisely minimap2's per-case orientation. The returned
	// max_q/max_t and (REV_CIGAR-reversed) CIGAR then land in the same frame
	// align.c already interprets, matching minimap2's process_*_ext_result.
	int r;
	if (flag & KSW_EZ_EXTZ_ONLY) {
		std::vector<uint8_t> qfwd(qseq, qseq + qlen);
		std::reverse(qfwd.begin(), qfwd.end());
		r = ksw_extd2_gpu_batch_add(acc->batch, qlen, qfwd.data(), tlen, tseq,
		                            w, end_bonus, zdrop, flag, ez);
	} else {
		r = ksw_extd2_gpu_batch_add(acc->batch, qlen, qseq, tlen, tseq,
		                            w, end_bonus, zdrop, flag, ez);
	}
	if (r == 1) {
		// A forced flush happened inside add; the flushed tasks were dispatched.
		// (count is tracked in flush via pending snapshot; see flush wrapper.)
	}
	return r;
}

extern "C" int mb_gpu_accum_flush(mb_gpu_accum_t *acc)
{
	if (!acc || !acc->batch) return -1;
	int pending = ksw_extd2_gpu_batch_pending(acc->batch);
	int n = ksw_extd2_gpu_batch_flush(acc->batch);
	if (n > 0) g_dispatched.fetch_add(n, std::memory_order_relaxed);
	else if (n < 0 && pending > 0) { /* error path; nothing dispatched */ }
	return n;
}

extern "C" int mb_gpu_accum_pending(mb_gpu_accum_t *acc)
{
	if (!acc || !acc->batch) return 0;
	return ksw_extd2_gpu_batch_pending(acc->batch);
}

extern "C" void mb_gpu_accum_destroy(mb_gpu_accum_t *acc)
{
	if (!acc) return;
	if (acc->batch) ksw_extd2_gpu_batch_destroy(acc->batch);
	delete acc;
}

extern "C" void mb_gpu_warmup(void)
{
	ksw_gpu_warmup();
}

extern "C" long mb_gpu_dispatched_count(void)
{
	return g_dispatched.load(std::memory_order_relaxed);
}
