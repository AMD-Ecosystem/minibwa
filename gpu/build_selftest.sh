#!/bin/bash
# SPDX-FileCopyrightText: Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Build the miniBWA GPU-alignment transplant self-test on an AMD Instinct node.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"   # minibwa/
cd "$ROOT"

: "${GPU_ARCH:=gfx942}"
export MIOPEN_USER_DB_PATH="${TMPDIR:-/tmp}/miopen-$USER"
export MIOPEN_CUSTOM_CACHE_DIR="$MIOPEN_USER_DB_PATH"
mkdir -p "$MIOPEN_USER_DB_PATH"

HIPCC="${HIPCC:-hipcc}"
# Resolve the real ROCm root (Alola: /opt/rocm -> /opt/rocm/core-7.x) so hipcc
# finds hip/hip_runtime.h. hipconfig --path reports the true root.
ROCM_ROOT="$(hipconfig --path 2>/dev/null || echo /opt/rocm)"
export ROCM_PATH="$ROCM_ROOT"
export HIP_PATH="$ROCM_ROOT"
INCLUDES="-I. -Igpu/include -I${ROCM_ROOT}/include"
HIPFLAGS="--offload-arch=${GPU_ARCH} -O2 -std=c++17 -x hip"
CXXFLAGS="-O2 -std=c++17"

echo "== compiling transplanted GPU kernel (multialign) =="
$HIPCC $HIPFLAGS $INCLUDES -c gpu/ksw2_extd2_gpu_multialign.cpp -o gpu/ksw2_extd2_gpu_multialign.o

echo "== compiling CIGAR backtrack kernel =="
$HIPCC $HIPFLAGS $INCLUDES -c gpu/ksw2_backtrack_gpu.cpp -o gpu/ksw2_backtrack_gpu.o

echo "== compiling GPU batch interface =="
$HIPCC $HIPFLAGS $INCLUDES -c gpu/ksw2_gpu_batch.cpp -o gpu/ksw2_gpu_batch.o

echo "== compiling dispatch bridge =="
$HIPCC $HIPFLAGS $INCLUDES -c gpu/gpu_dispatch.cpp -o gpu/gpu_dispatch.o

echo "== compiling self-test harness =="
$HIPCC $HIPFLAGS $INCLUDES -c gpu/gpu_selftest.cpp -o gpu/gpu_selftest.o

echo "== compiling miniBWA CPU reference aligner (ksw2_extd2_sse) =="
# Host C compile with SSE4 (x86 host) for the parity reference.
${CC:-cc} -O2 -msse4.2 -mpopcnt $INCLUDES -c ksw2_extd2_sse.c -o gpu/ksw2_extd2_sse.o

echo "== linking =="
# Link WITHOUT -x hip (it would treat the .o inputs as HIP source). hipcc adds
# the HIP runtime libs automatically.
$HIPCC --offload-arch=${GPU_ARCH} gpu/ksw2_extd2_gpu_multialign.o gpu/ksw2_backtrack_gpu.o \
    gpu/ksw2_gpu_batch.o gpu/gpu_dispatch.o gpu/gpu_selftest.o gpu/ksw2_extd2_sse.o \
    -o gpu/gpu_selftest

echo "== BUILD OK: gpu/gpu_selftest =="
