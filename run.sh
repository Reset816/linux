ARCH=riscv CROSS_COMPILE=riscv64-linux- make defconfig
scripts/config -e RUNTIME_TESTING_MENU -e KPROBES -e TEST_MEMSET_RISCV
cat .config | grep RUNTIME_TESTING_M

# A: 开启Zicboz优化（默认）
scripts/config -d RISCV_MEMSET_DISABLE_ZICBOZ
ARCH=riscv CROSS_COMPILE=riscv64-linux- make olddefconfig

# B: 关闭Zicboz优化
scripts/config -e RISCV_MEMSET_DISABLE_ZICBOZ
ARCH=riscv CROSS_COMPILE=riscv64-linux- make olddefconfig

cat .config | grep RISCV_MEMSET_DISABLE_ZICBOZ
ARCH=riscv CROSS_COMPILE=riscv64-linux- make -j50
# qemu-system-riscv64 \
#   -machine virt \
#   -nographic \
#   -cpu max,zicboz=on \
#   -m 4096 \
#   -smp 4 \
#   -bios default \
#   -kernel arch/riscv/boot/Image \
#   -drive file=/workspaces/cpm/linux-repos/ubuntu-24.04.3-preinstalled-server-riscv64.img,format=raw,if=none,id=hd0 \
#   -device virtio-blk-device,drive=hd0 \
#   -append "console=ttyS0 root=/dev/vda1 rw rootwait test_memset_riscv.bytes_per_case=16777216 test_memset_riscv.min_loops=1000 test_memset_riscv.warmup_loops=200  test_memset_riscv.require_probe=1 cloud-init=disabled"

qemu-system-riscv64 \
    -machine virt \
    -nographic \
    -cpu max,zicboz=on \
    -m 4096 \
    -smp 4 \
    -bios default \
    -kernel arch/riscv/boot/Image \
    -initrd /workspaces/cpm/linux-repos/tmp/output/images/rootfs.cpio \
    -append "console=ttyS0 rdinit=/sbin/init rw rootwait test_memset_riscv.bytes_per_case=16777216 test_memset_riscv.min_loops=1000 test_memset_riscv.warmup_loops=200 test_memset_riscv.require_probe=1"

dmesg | grep test_memset_riscv

## maybe we should use perf instead of our own script to measure the performance