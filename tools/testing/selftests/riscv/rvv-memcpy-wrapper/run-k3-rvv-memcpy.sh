#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Run the RVV memcpy workload initramfs under K3 QEMU+KVM.

set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARTIFACT_DIR=${ARTIFACT_DIR:-/home/froster/yuan/rvv-memcpy-wrapper}
MODE=${1:-vector}
APPEND="console=ttyS0 earlycon=sbi rdinit=/root/bin/rvv-run root=/dev/ram0 nokaslr panic=-1 riscv_v_memcpy_profile"

case "$MODE" in
vector)
	;;
scalar|disabled)
	APPEND="$APPEND riscv_v_memcpy_disable"
	MODE=scalar
	;;
*)
	echo "usage: $0 [vector|scalar]" >&2
	exit 2
	;;
esac

LOG="$ARTIFACT_DIR/k3-${MODE}.log"
echo "QEMU_LOG=$LOG"
echo "QEMU_MODE=$MODE"
echo "QEMU_APPEND=$APPEND"
exec taskset -c 2 qemu-system-riscv64 \
	-accel kvm \
	-machine virt \
	-cpu host \
	-smp 1 \
	-m 2048M \
	-nographic \
	-no-reboot \
	-bios none \
	-kernel "$ARTIFACT_DIR/Image" \
	-initrd "$ARTIFACT_DIR/rvv-memcpy-k3-initramfs.cpio.gz" \
	-append "$APPEND" 2>&1 | tee "$LOG"
