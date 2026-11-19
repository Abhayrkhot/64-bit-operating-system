# Toolchain for Shale: clang/lld for the UEFI loader (PE/COFF), the kernel
# (ELF, freestanding) and host tools; QEMU + OVMF for boot tests.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang-18 lld-18 llvm-18 libclang-rt-18-dev g++ make python3 \
      qemu-system-x86 ovmf mtools dosfstools gdb git ca-certificates xxd \
 && rm -rf /var/lib/apt/lists/* \
 && for t in clang clang++ ld.lld lld-link llvm-objdump llvm-objcopy llvm-readelf \
             llvm-nm llvm-addr2line llvm-symbolizer llvm-ar llvm-strip; do \
      ln -sf /usr/bin/$t-18 /usr/local/bin/$t; done
