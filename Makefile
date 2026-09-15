# Modifications Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
CC=			gcc
CFLAGS=		-std=c99 -g -Wall -O3
CPPFLAGS=
LDFLAGS=
INCLUDES=
LOBJS=		kommon.o kalloc.o bwt.o l2bit.o options.o seed.o map-algo.o lchain.o align.o pe.o cs.o format.o \
			ksw2_extz2_sse.o ksw2_extd2_sse.o ksw2_ll_sse.o
AOBJS=		kthread.o libsais.o libsais64.o index.o bseq.o map-main.o fastmap.o
MALLOC_O=	mimalloc.o
PROG=		minibwa
LIBS=		-lpthread -lz -lm
ARCH=		$(shell uname -m)
omp=		$(shell printf '\043include <omp.h>\nint main(){return 0;}' | $(CC) -x c -fopenmp -o /dev/null - 2>/dev/null && echo "1" || echo "0")

# ---- GPU (ROCm/HIP) alignment offload ----------------------------------------
# `make GPU=1` builds the AMD Instinct GPU-accelerated aligner. It compiles the
# transplanted ksw_extd2_gpu batch (from AMD-AIOSS/minimap2) with hipcc, defines
# HAVE_GPU_ALIGNMENT so the C sources activate the GPU dispatch seam in
# mb_align_pair, and links against the HIP runtime. The default `make` still
# produces a pure-CPU binary with no ROCm dependency.
#
#   make GPU=1 ARCH=gfx942      # MI300X / MI300A
#   make GPU=1 ARCH=gfx950      # MI350X / MI355X
#
# HIPCC / ROCM_PATH are auto-resolved; override on the command line if needed.
ROCM_PATH?=	$(shell hipconfig --path 2>/dev/null || echo /opt/rocm)
HIPCC?=		$(ROCM_PATH)/bin/hipcc
GPU_ARCH?=	gfx942
GPU_OBJS=	gpu/gpu_dispatch.o gpu/ksw2_extd2_gpu_multialign.o gpu/ksw2_backtrack_gpu.o gpu/ksw2_gpu_batch.o
GPU_INCLUDES=	-I. -Igpu/include
# GPU_ARCH may be a single arch (gfx942) or a ";"-separated list (gfx942;gfx950);
# expand to one --offload-arch flag per arch so hipcc builds a multi-arch binary.
OFFLOAD_ARCH=	$(foreach a,$(subst ;, ,$(GPU_ARCH)),--offload-arch=$(a))
HIPFLAGS=	-std=c++17 -O3 $(OFFLOAD_ARCH) -DHAVE_GPU_ALIGNMENT $(GPU_INCLUDES)

# ROCm manylinux (RHEL8) toolchain seam: hipcc bundles its own clang++ whose
# default gcc-install-dir is the RHEL8 system gcc-8, which ships no libstdc++.so
# dev symlink (only the .so.6 runtime), so the HIP link fails with
# "unable to find library -lstdc++". Point clang at gcc-toolset-14 when present.
# Also, hipcc --hip-link defaults to a PIE executable while the C objects are
# compiled non-PIC (default make), so add -no-pie to avoid R_X86_64_32
# relocation-against-local-symbol link errors. Both are no-ops on toolchains
# where they are unnecessary (older full-ROCm Ubuntu images).
GCC_INSTALL_DIR?=	$(firstword $(wildcard /opt/rh/gcc-toolset-14/root/usr/lib/gcc/x86_64-redhat-linux/14))
ifneq ($(GCC_INSTALL_DIR),)
	HIPFLAGS+=--gcc-install-dir=$(GCC_INSTALL_DIR)
	HIP_LDFLAGS+=--gcc-install-dir=$(GCC_INSTALL_DIR)
endif
HIP_LDFLAGS+=-no-pie

ifneq ($(asan),)
	CFLAGS+=-fsanitize=address
	LDFLAGS+=-fsanitize=address
	LIBS+=-ldl
endif

ifeq ($(omp),1)
	CPPFLAGS+=-DLIBSAIS_OPENMP
	CFLAGS+=-fopenmp
	LIBS+=-fopenmp
endif

ifneq ($(gpl),0)
	AOBJS+=QSufSort.o bwtgen.o
	CPPFLAGS+=-DUSE_GPL
endif

ifeq ($(mimalloc),0)
	MALLOC_O=
	CPPFLAGS+=-DHAVE_KALLOC
endif

ifeq ($(ARCH), x86_64)
	CFLAGS+=-msse4.2 -mpopcnt
endif

# GPU=1: activate the GPU seam in the C code, link via hipcc + HIP runtime.
ifeq ($(GPU),1)
	CPPFLAGS+=-DHAVE_GPU_ALIGNMENT -I. -Igpu/include
	AOBJS+=$(GPU_OBJS)
	LINKER=$(HIPCC)
	LDFLAGS+=$(OFFLOAD_ARCH) $(HIP_LDFLAGS)
	LIBS+=-L$(ROCM_PATH)/lib -lamdhip64
else
	LINKER=$(CC)
endif

.SUFFIXES:.c .o
.PHONY:all clean depend

.c.o:
		$(CC) -c $(CFLAGS) $(CPPFLAGS) $(INCLUDES) $< -o $@

# HIP objects (only built for GPU=1). hipcc compiles the device kernels and the
# host-side batch/dispatch glue in one pass.
gpu/%.o: gpu/%.cpp
		$(HIPCC) -c $(HIPFLAGS) $< -o $@

all:$(PROG)

mimalloc.o:
		$(CC) -c -std=gnu11 -O3 -Wall -Wextra -DNDEBUG -DMI_MALLOC_OVERRIDE -DMI_OSX_INTERPOSE=1 -DMI_OSX_ZONE=1 -Imimalloc mimalloc/static.c -o $@

libminibwa.a:$(LOBJS)
		$(AR) -csru $@ $(LOBJS)

minibwa:libminibwa.a $(MALLOC_O) $(AOBJS) main.o
		$(LINKER) $(CFLAGS) $(LDFLAGS) $(MALLOC_O) $(AOBJS) main.o -o $@ -L. -lminibwa $(LIBS)

clean:
		rm -fr *.o gpu/*.o a.out $(PROG) *~ *.a *.dSYM

depend:
		(LC_ALL=C; export LC_ALL; makedepend -Y -- $(CFLAGS) $(DFLAGS) -- *.c *.cpp)

# DO NOT DELETE

QSufSort.o: QSufSort.h
align.o: mbpriv.h minibwa.h l2bit.h bwt.h kommon.h bseq.h kalloc.h ksw2.h
bseq.o: bseq.h kommon.h kseq.h
bwt.o: kommon.h kalloc.h bwt.h
bwtgen.o: QSufSort.h
cs.o: mbpriv.h minibwa.h l2bit.h bwt.h kommon.h bseq.h kalloc.h
fastmap.o: mbpriv.h minibwa.h l2bit.h bwt.h kommon.h bseq.h ketopt.h kseq.h
fastmap.o: kalloc.h
format.o: mbpriv.h minibwa.h l2bit.h bwt.h kommon.h bseq.h
index.o: libsais.h libsais64.h kommon.h ketopt.h mbpriv.h minibwa.h l2bit.h
index.o: bwt.h bseq.h
kalloc.o: kalloc.h
kommon.o: kommon.h
ksw2_extd2_sse.o: ksw2.h
ksw2_extz2_sse.o: ksw2.h
ksw2_ll_sse.o: ksw2.h
kthread.o: kthread.h
l2bit.o: kommon.h l2bit.h kseq.h
lchain.o: mbpriv.h minibwa.h l2bit.h bwt.h kommon.h bseq.h kalloc.h ksort.h
libsais.o: libsais.h
libsais64.o: libsais.h libsais64.h
main.o: kommon.h mbpriv.h minibwa.h l2bit.h bwt.h bseq.h ketopt.h
map-algo.o: mbpriv.h minibwa.h l2bit.h bwt.h kommon.h bseq.h kalloc.h ksort.h
map-main.o: kommon.h mbpriv.h minibwa.h l2bit.h bwt.h bseq.h kalloc.h
map-main.o: kthread.h ketopt.h kseq.h
options.o: minibwa.h
pe.o: mbpriv.h minibwa.h l2bit.h bwt.h kommon.h bseq.h kalloc.h ksw2.h
seed.o: mbpriv.h minibwa.h l2bit.h bwt.h kommon.h bseq.h kalloc.h ksort.h
