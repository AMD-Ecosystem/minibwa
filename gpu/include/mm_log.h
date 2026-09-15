// SPDX-FileCopyrightText: Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//
// Lightweight logging shim for the miniBWA GPU-alignment transplant trial.
//
// The transplanted GPU files (ksw2_gpu_batch.cpp, hip_raii.h) come from
// AMD-AIOSS/minimap2, where mm_log.h pulls in spdlog + minimap.h. miniBWA has
// neither. The transplanted call sites use spdlog "{}"-style format strings, so
// this shim's macros DISCARD their varargs (a printf shim would misinterpret the
// "{}" tokens). mm_log_debug is compiled out unless MB_GPU_VERBOSE is set; the
// warn/error macros emit a fixed marker line so a fault is still visible without
// re-implementing spdlog's formatter. hip_raii.h's mm_log_error and the batch's
// own HIP error helper are the only non-debug users.

#ifndef MM_LOG_SHIM_H_
#define MM_LOG_SHIM_H_

#include <cstdio>

// Discard the spdlog-style variadic args; emit a marker so faults stay visible.
#define mm_log(...)        do { fprintf(stderr, "[gpu-align] (log)\n"); } while (0)
#define mm_log_error(...)  do { fprintf(stderr, "[gpu-align][error] (see HIP error above)\n"); } while (0)
#define mm_log_warn(...)   do { fprintf(stderr, "[gpu-align][warn]\n"); } while (0)

#ifdef MB_GPU_VERBOSE
#define mm_log_debug(...)  do { fprintf(stderr, "[gpu-align][debug]\n"); } while (0)
#else
#define mm_log_debug(...)  do { } while (0)
#endif

#endif /* MM_LOG_SHIM_H_ */
