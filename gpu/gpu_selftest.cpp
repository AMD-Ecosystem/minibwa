// SPDX-FileCopyrightText: Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//
// Standalone parity self-test for the miniBWA GPU-alignment transplant.
//
// Proves the transplanted AMD-AIOSS/minimap2 GPU DP kernel (a) builds against
// miniBWA's ksw2.h, (b) dispatches on a real AMD Instinct GPU, and (c) produces
// scores matching miniBWA's own CPU aligner (ksw_extd2_sse) on the SAME inputs.
// This is the "does the transplanted kernel produce correct aligned reads" gate
// before wiring the bridge into miniBWA's threaded pipeline.
//
// All queued tasks share flag/end_bonus/zdrop so the batch does NOT auto-flush;
// a single explicit flush dispatches the whole batch, making the GPU-dispatch
// counter exact.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
extern "C" {
#include "ksw2.h"
}
#include "gpu/include/gpu_dispatch.h"

static void encode(const char *s, std::vector<uint8_t> &out)
{
	out.clear();
	for (const char *p = s; *p; ++p) {
		switch (*p) {
			case 'A': case 'a': out.push_back(0); break;
			case 'C': case 'c': out.push_back(1); break;
			case 'G': case 'g': out.push_back(2); break;
			case 'T': case 't': out.push_back(3); break;
			default: out.push_back(4); break;
		}
	}
}

static std::string cigar_str(const ksw_extz_t &ez)
{
	std::string s;
	char buf[32];
	for (int k = 0; k < ez.n_cigar; ++k) {
		snprintf(buf, sizeof buf, "%d%c", ez.cigar[k] >> 4, "MID"[ez.cigar[k] & 0xf]);
		s += buf;
	}
	return s;
}

int main(void)
{
	// miniBWA short-read defaults (options.c): a=1,b=4,q=6,e=1,q2=13,e2=1,
	// b_ts=0, b_ambi=1, w=100, zdrop already *a (a=1 so zdrop stays 100).
	const int8_t a = 1, b = 4, b_ts = 0, b_ambi = 1;
	const int8_t q = 6, e = 1, q2 = 13, e2 = 1;
	const int w = 100, zdrop = 100, end_bonus = 5;

	// Build miniBWA's 5x5 matrix the same way mb_align_pair does.
	int8_t mat[25];
	ksw_gen_nt4_mat(mat, a, b, b_ts, b_ambi, 0);

	// Extension mode: KSW_EZ_EXTZ_ONLY is the read-extension flag miniBWA uses in
	// its skeleton (align.c). All cases share this flag so no auto-flush fires.
	const int flag = KSW_EZ_EXTZ_ONLY;

	struct Case { const char *name; const char *q; const char *t; };
	std::vector<Case> cases = {
		{ "exact-40",  "ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT", "ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT" },
		{ "1mm-40",    "ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT", "ACGTACGTACGTACGTACGTATGTACGTACGTACGTACGT" },
		{ "del2-tgt",  "ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT", "ACGTACGTACGTACGTACGTACCGTACGTACGTACGTACGTAC" },
		{ "ins-qry",   "ACGTACGTACGTACGTACGTGGACGTACGTACGTACGTACGT","ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT" },
		{ "extend-30", "ACGTACGTACGTACGTACGTACGTACGTAC",             "ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT" },
	};

	printf("[selftest] warming up ROCm...\n");
	mb_gpu_warmup();

	mb_gpu_accum_t *acc = mb_gpu_accum_init(a, b, b_ts, b_ambi, q, e, q2, e2, w, zdrop, 65536);
	if (!acc) { fprintf(stderr, "[selftest] FAIL: mb_gpu_accum_init NULL\n"); return 2; }

	std::vector<std::vector<uint8_t>> qbuf(cases.size()), tbuf(cases.size());
	std::vector<ksw_extz_t> gpu_ez(cases.size()), cpu_ez(cases.size());
	memset(gpu_ez.data(), 0, gpu_ez.size() * sizeof(ksw_extz_t));
	memset(cpu_ez.data(), 0, cpu_ez.size() * sizeof(ksw_extz_t));

	// ---- CPU reference (miniBWA's own ksw_extd2_sse), one per case ----
	for (size_t i = 0; i < cases.size(); ++i) {
		encode(cases[i].q, qbuf[i]);
		encode(cases[i].t, tbuf[i]);
		ksw_extd2_sse(nullptr, (int)qbuf[i].size(), qbuf[i].data(),
		              (int)tbuf[i].size(), tbuf[i].data(), 5, mat,
		              q, e, q2, e2, w, zdrop, end_bonus, flag, &cpu_ez[i]);
	}

	// ---- GPU: queue the whole batch, flush once ----
	for (size_t i = 0; i < cases.size(); ++i) {
		int r = mb_gpu_accum_add(acc, (int)qbuf[i].size(), qbuf[i].data(),
		                         (int)tbuf[i].size(), tbuf[i].data(),
		                         w, end_bonus, zdrop, flag, &gpu_ez[i]);
		if (r < 0) { fprintf(stderr, "[selftest] FAIL: add %zu\n", i); mb_gpu_accum_destroy(acc); return 3; }
		if (r == 1) fprintf(stderr, "[selftest] WARN: unexpected auto-flush at case %zu\n", i);
	}
	printf("[selftest] pending before flush: %d (expect %zu)\n", mb_gpu_accum_pending(acc), cases.size());
	int n = mb_gpu_accum_flush(acc);
	printf("[selftest] flush dispatched %d alignments\n", n);
	if (n < 0) { fprintf(stderr, "[selftest] FAIL: flush\n"); mb_gpu_accum_destroy(acc); return 4; }

	// ---- Compare ----
	int match = 0;
	for (size_t i = 0; i < cases.size(); ++i) {
		std::string gc = cigar_str(gpu_ez[i]), cc = cigar_str(cpu_ez[i]);
		bool score_ok = (gpu_ez[i].max == cpu_ez[i].max) &&
		                (gpu_ez[i].mqe == cpu_ez[i].mqe) &&
		                (gpu_ez[i].mte == cpu_ez[i].mte) &&
		                (gpu_ez[i].zdropped == cpu_ez[i].zdropped);
		bool cig_ok = (gc == cc);
		bool ok = score_ok && cig_ok;
		if (ok) ++match;
		printf("[selftest] %-9s | CPU max=%d mqe=%d mte=%d zd=%d cig=%s\n",
		       cases[i].name, (int)cpu_ez[i].max, cpu_ez[i].mqe, cpu_ez[i].mte, (int)cpu_ez[i].zdropped, cc.c_str());
		printf("[selftest] %-9s | GPU max=%d mqe=%d mte=%d zd=%d cig=%s  => %s%s\n",
		       "", (int)gpu_ez[i].max, gpu_ez[i].mqe, gpu_ez[i].mte, (int)gpu_ez[i].zdropped, gc.c_str(),
		       score_ok ? "SCORE_OK" : "SCORE_MISMATCH", cig_ok ? " CIGAR_OK" : " CIGAR_DIFF");
	}

	long dispatched = mb_gpu_dispatched_count();
	printf("[selftest] total GPU-dispatched alignments (counter): %ld\n", dispatched);
	mb_gpu_accum_destroy(acc);

	printf("[selftest] GPU==CPU parity: %d/%zu cases\n", match, cases.size());
	if (dispatched == (long)cases.size() && match == (int)cases.size()) {
		printf("[selftest] PASS: GPU dispatch confirmed AND matches miniBWA CPU aligner on all cases\n");
		return 0;
	}
	// Dispatch alone is the primary POC gate; parity is the stretch goal.
	if (dispatched == (long)cases.size()) {
		printf("[selftest] PARTIAL: GPU dispatch confirmed (%ld) but %d/%zu parity (see mismatches above)\n",
		       dispatched, match, cases.size());
		return 5;
	}
	fprintf(stderr, "[selftest] FAIL: dispatched=%ld match=%d/%zu\n", dispatched, match, cases.size());
	return 1;
}
