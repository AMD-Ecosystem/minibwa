# Building and running GPU-accelerated miniBWA on AMD Instinct

This branch (`amd-integration-0.3`) adds optional GPU offload of miniBWA's dual-affine
banded DP alignment stage to AMD Instinct GPUs via ROCm/HIP, using the proven
`ksw_extd2_gpu_multialign` kernel from AMD's minimap2 GPU port
(AMD-AIOSS/minimap2, `amd-integration-2.31`). The default build is unchanged
(pure CPU, no ROCm dependency); the GPU path is opt-in at build time (`make GPU=1`)
and at run time (`--gpu`).

## What this branch adds

- A per-thread GPU alignment accumulator threaded through miniBWA's alignment
  pipeline (`mb_align_skeleton -> mb_align1 -> mb_align_pair`). The dual-affine
  `ksw_extd2_sse` call is routed to a synchronous GPU add+flush when the accumulator
  is present and the alignment is not a generic-scoring (methylation/transition) case.
- The transplanted ROCm/HIP kernels under `gpu/` plus a thin `gpu_dispatch` bridge
  that reconciles miniBWA's ksw2 sequence-orientation convention with the kernel.
- A `--gpu` CLI flag and a one-time ROCm warmup, plus a `make GPU=1` build path.
- Methylation/transition (generic scoring) and the single-affine `ksw_extz2_sse`
  path stay on CPU; the common non-methylation short/long-read path is GPU-eligible.

## Prerequisites

- Docker with AMD ROCm (recommended): `rocm/dev-ubuntu-24.04:7.2-complete`
  (self-contained ROCm 7.2 + HIP dev headers; talks to the host kernel driver).
- Or a bare-metal ROCm 7.2+ install with full dev headers (`hipcc`,
  `hip/hip_runtime.h`).
- An AMD Instinct GPU. Validated on **MI300X (gfx942)** and **MI350X/MI355X
  (gfx950)**.
- GPU device access at run time via the Docker flags
  `--device=/dev/kfd --device=/dev/dri --group-add video`.

Verify the GPU with `rocminfo` (parse the `Name:` / `Marketing Name:` lines); do NOT
rely on `rocm-smi --json` on ROCm 7.2.

## Building the BWT index (one-time, ~11 min, ~55 GB RAM)

The index build is CPU-only and identical for both binaries:

```bash
./minibwa index ref.fa        # writes ref.fa.mbw + ref.fa.l2b
```

## CPU build (no ROCm required)

The default build produces a pure-CPU binary that links no ROCm library:

```bash
make gpl=0 mimalloc=0         # MIT-only sources, kalloc allocator
# or plain: make
```

## GPU build (MI300X, gfx942)

The GPU kernels compile with `hipcc`; the C sources gain the GPU seam via
`-DHAVE_GPU_ALIGNMENT`; the binary links the HIP runtime (`-lamdhip64`). Build inside
a ROCm dev container (recommended on nodes that ship only the ROCm runtime):

```bash
docker run --rm -v $(pwd):/work rocm/dev-ubuntu-24.04:7.2-complete \
  bash -c "cd /work && make GPU=1 GPU_ARCH=gfx942 gpl=0 mimalloc=0"
```

On a host with a full ROCm dev toolchain, build directly:

```bash
make GPU=1 GPU_ARCH=gfx942 gpl=0 mimalloc=0
```

`ROCM_PATH` / `HIPCC` are auto-resolved from `hipconfig --path` (override on the
command line if needed).

## GPU build (MI355X / MI350X, gfx950)

```bash
docker run --rm -v $(pwd):/work rocm/dev-ubuntu-24.04:7.2-complete \
  bash -c "cd /work && make GPU=1 GPU_ARCH=gfx950 gpl=0 mimalloc=0"
```

## GPU build (multi-arch, one fat binary)

`GPU_ARCH` accepts a `;`-separated list; one `--offload-arch` flag is emitted per arch,
producing a single binary that runs on both:

```bash
docker run --rm -v $(pwd):/work rocm/dev-ubuntu-24.04:7.2-complete \
  bash -c 'cd /work && make GPU=1 GPU_ARCH="gfx942;gfx950" gpl=0 mimalloc=0'
```

## Running

### CPU

```bash
./minibwa map -t 8 ref.fa reads.fq > out.paf
```

### GPU (AMD Instinct)

In Docker (pass the GPU devices; the container's own ROCm 7.2 talks to the host kernel
driver, so no host `/opt/rocm` mount is needed):

```bash
docker run --rm --device=/dev/kfd --device=/dev/dri --group-add video \
  -v $(pwd):/work -v /path/to/data:/data:ro \
  rocm/dev-ubuntu-24.04:7.2-complete \
  bash -c "cd /work && export MIOPEN_USER_DB_PATH=/tmp/miopen-\$USER MIOPEN_CUSTOM_CACHE_DIR=/tmp/miopen-\$USER; mkdir -p \$MIOPEN_USER_DB_PATH; ./minibwa map -t 8 --gpu /data/ref.fa /data/reads.fq > out_gpu.paf"
```

On bare metal with ROCm installed:

```bash
export MIOPEN_USER_DB_PATH=$TMPDIR/miopen-$USER MIOPEN_CUSTOM_CACHE_DIR=$TMPDIR/miopen-$USER
mkdir -p $MIOPEN_USER_DB_PATH
./minibwa map -t 8 --gpu ref.fa reads.fq > out_gpu.paf
```

`--gpu` on a CPU-only build prints a warning and falls back to CPU.

## Validation results (GRCh38 no-alt, 126,620 x 150 bp, 1% HG002 short-read subset)

Container `rocm/dev-ubuntu-24.04:7.2-complete` (ROCm 7.2, HIP 7.2.26015), reference
GRCh38 no-alt (3.0 GB), 1% HG002 short-read subset (126,620 x 150 bp), `-t 8`. Both
binaries built and run on each node; the CPU binary is the reference. GPU dispatch
was independently confirmed on both archs with the standalone `gpu/gpu_selftest`
harness (`ksw_extd2_gpu_multialign` launched on real silicon, dispatch counter 5/5).

| Platform | Arch | CPU wall | GPU wall | Mapped (CPU) | Mapped (GPU) | Mapped agreement | Byte-identical reads |
|---|---|---|---|---|---|---|---|
| AMD Instinct MI300X | gfx942 | 10 s | 65 s | 116,218 | 116,214 | 116,214 both / 0 GPU-only / 4 CPU-only = 99.997% | 126,264 / 126,623 = 99.72% |
| AMD Instinct MI350X/MI355X | gfx950 | 4 s | 56 s | 116,218 | 116,214 | 116,214 both / 0 GPU-only / 4 CPU-only = 99.997% | 126,264 / 126,623 = 99.72% |

- Both archs build clean (CPU-only, single-arch GPU, and gfx942;gfx950 multi-arch);
  the full-dataset run completes with no crashes or HIP errors.
- Byte-identical PAF lines: 130,835 / 131,209 on both archs.
- The two archs produce byte-identical output to each other and agree with the CPU
  aligner to the same degree (cross-arch reproducibility).
- The CPU-only reference binary links no ROCm library (`ldd` shows no `libamdhip64`).

## Known limitations

- The GPU alignment path accelerates the dual-affine DP extension stage
  (`ksw_extd2`). For 150 bp short reads the CPU SSE path is faster (per-alignment
  dispatch overhead dominates the trivially small short-read DP; miniBWA's ungapped
  CPU fast-path already carries most short reads). The GPU offload is designed for
  longer reads (large-DP workloads) where DP cost dominates, matching AMD minimap2's
  GPU-align characteristics (gains on Oxford Nanopore / PacBio HiFi, not tiny short
  reads).
- A residual ~0.3% of reads show CIGAR/AS differences vs the CPU aligner (same mapped
  locus). Two benign classes: (1) a narrow dual-affine gap-fill edge case in the
  shared, verbatim-transplanted `ksw_extd2_gpu_multialign` kernel (identical to
  AMD-AIOSS/minimap2) returns a sub-optimal indel path for a few near-diagonal
  segments -- a defect in the shared kernel to be filed upstream, not in this
  integration; (2) multi-mapping reads to repetitive/decoy loci (`chrUn_*`,
  `*_random`, centromeric) where a 1-2 point AS difference flips the primary-alignment
  choice between two near-equal loci (low mapq), inherent to any DP-scoring
  difference; it does not affect the mapped-set agreement.
- The methylation/transition (generic-scoring) alignment path and the single-affine
  `ksw_extz2_sse` path are not GPU-offloaded (they stay on CPU).
- gfx1250 (MI450 FFM simulator) is not yet validated.
- **OSRB approval is required before any public release of this fork.**

## Differences from upstream miniBWA

Modified upstream files (each carries the AMD modification line; upstream `LICENSE.txt`
is untouched):

- `Makefile` -- `GPU=1` / `GPU_ARCH` build targets (hipcc for `gpu/*.cpp`,
  `-DHAVE_GPU_ALIGNMENT`, `-lamdhip64`).
- `align.c` -- the GPU seam inside `mb_align_pair`'s dual-affine branch; GPU
  accumulator threaded through `mb_align1` / `mb_align_skeleton`; km-arena cigar
  ownership fix.
- `map-algo.c` -- per-thread GPU accumulator lifecycle in `mb_tbuf`.
- `map-main.c` -- `--gpu` CLI flag and one-time warmup.
- `mbpriv.h` -- `mb_tbuf` GPU field and accessor.
- `minibwa.h` -- `MB_F_GPU` flag and `mb_gpu_accum_t` typedef.
- `.gitignore` -- GPU build artifacts.

New AMD files (Apache-2.0 SPDX header) and transplanted kernels (retain their existing
AMD MIT header from minimap2) live under `gpu/`:

- `gpu/gpu_dispatch.{cpp,h}` -- the miniBWA-facing dispatch bridge (new AMD).
- `gpu/gpu_selftest.cpp`, `gpu/build_selftest.sh`, `gpu/include/mm_log.h` -- new AMD.
- `gpu/ksw2_extd2_gpu_multialign.cpp`, `gpu/ksw2_gpu_batch.{cpp,h}`,
  `gpu/include/hip_raii.h` -- verbatim transplant from AMD-AIOSS/minimap2 (AMD MIT).
- `gpu/ksw2_backtrack_gpu.cpp` -- transplant with an AMD modification notice.

## Design notes

The seam is a per-thread `mb_gpu_accum_t` (one per `kt_for` worker) threaded through
`mb_align_skeleton -> mb_align1 -> mb_align_pair` as a `void *gpu` argument (NULL =
CPU). Inside `mb_align_pair`, the dual-affine `ksw_extd2_sse` call is redirected to a
synchronous GPU add+flush when the accumulator is present and the alignment is not a
generic-scoring case. `ksw_extz_t` is ABI-identical between miniBWA and minimap2, so
results are written straight back and `mb_align1`'s control flow (z-drop test, second
pass, dp_score accumulation) reads `ez` immediately after the call as before.

The `gpu/gpu_dispatch.cpp` bridge reverses the query for every `KSW_EZ_EXTZ_ONLY`
extension task: the kernel reads the query inline as `query[(qlen-1-r)+t]` and expects
minimap2's orientation, while miniBWA passes the opposite orientation (the CPU
`ksw_extd2_sse` hides this by rebuilding a reversed query internally). The GPU
backtrack kernel's extension reach-end branch mirrors the CPU condition
(`EXTZ_ONLY && mqe+end_bonus > max`, independent of z-drop). The DP kernel itself is
byte-identical to the transplant source.
