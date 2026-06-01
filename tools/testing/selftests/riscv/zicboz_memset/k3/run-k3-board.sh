#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

ARTIFACT_DIR="${1:-/home/froster/yuan/zicboz-memset-zero/fast}"
LOG="${2:-$ARTIFACT_DIR/qemu.log}"

exec taskset -c 2 qemu-system-riscv64 \
	-accel kvm \
	-machine virt \
	-cpu host \
	-smp 1 \
	-nographic \
	-no-reboot \
	-bios none \
	-m 2G \
	-kernel "$ARTIFACT_DIR/Image" \
	-initrd "$ARTIFACT_DIR/rootfs.cpio.gz" \
	-drive "file=$ARTIFACT_DIR/ext4.img,format=raw,if=virtio" \
	-append 'console=ttyS0 root=/dev/ram rw rdinit=/test-init' \
	2>&1 | tee "$LOG"
