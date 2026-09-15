// SPDX-FileCopyrightText: Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//
// miniBWA GPU-alignment dispatch bridge (trial transplant).
//
// Thin C wrapper over the transplanted AMD-AIOSS/minimap2 GPU batch interface
// (ksw2_gpu_batch.h). It buffers per-read banded dual-affine DP alignment tasks
// and runs them together on an AMD Instinct GPU via ksw_extd2_gpu_multialign,
// writing results back into the caller-owned ksw_extz_t. The struct and the
// KSW_EZ_* flags are ABI-identical between miniBWA and minimap2, so ez is filled
// directly. One accumulator per worker thread.

#ifndef MB_GPU_DISPATCH_H_
#define MB_GPU_DISPATCH_H_

#include <stdint.h>
#include "ksw2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mb_gpu_accum_s mb_gpu_accum_t;

// Create one accumulator. Scoring scalars mirror mb_opt_t fields:
//   a=match, b=mismatch, b_ts=transition, b_ambi=ambiguous,
//   q/e=gap open/extend (short), q2/e2=gap open/extend (long),
//   w=bandwidth, zdrop already scaled by the caller (zdrop*a).
// max_alignments caps the batch before a forced flush.
mb_gpu_accum_t *mb_gpu_accum_init(int8_t a, int8_t b, int8_t b_ts, int8_t b_ambi,
                                  int8_t q, int8_t e, int8_t q2, int8_t e2,
                                  int w, int zdrop, int max_alignments);

// Queue one alignment. Result is written into *ez at the next flush.
// Returns 0 queued, 1 batch auto-flushed, -1 error.
int mb_gpu_accum_add(mb_gpu_accum_t *acc,
                     int qlen, const uint8_t *qseq,
                     int tlen, const uint8_t *tseq,
                     int w, int end_bonus, int zdrop, int flag,
                     ksw_extz_t *ez);

// Run all pending alignments; fills every queued ez. Returns count or -1.
int mb_gpu_accum_flush(mb_gpu_accum_t *acc);

int mb_gpu_accum_pending(mb_gpu_accum_t *acc);
void mb_gpu_accum_destroy(mb_gpu_accum_t *acc);

// Trigger lazy ROCm init once at startup.
void mb_gpu_warmup(void);

// Total alignments dispatched to the GPU across all flushes (diagnostic).
long mb_gpu_dispatched_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MB_GPU_DISPATCH_H_ */
