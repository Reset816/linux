#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

HOST=/mnt/host
MODULE=/rvv_memset_eval.ko
PROC=/proc/rvv_memset_eval

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

if [ ! -e "$MODULE" ]; then
	MODULE=$HOST/rvv_memset_eval.ko
	mkdir -p "$HOST"
	mountpoint -q "$HOST" || \
		mount -t 9p -o trans=virtio,version=9p2000.L host0 "$HOST"
fi

echo "== guest =="
uname -a
grep -E '^(processor|hart|isa)' /proc/cpuinfo || true

echo "== load eval module =="
insmod "$MODULE"

run_case()
{
	name="$1"
	shift
	echo "== $name =="
	printf '%s\n' "$*" > "$PROC"
	cat "$PROC"
}

run_case dedicated 'reset
correct iterations=400 bytes=67108864 sizes=512,768,1023,1024,1025,1536,2047,2048,2049,4095,4096,4097,8191,8192,8193,16383,16384,16385,65536 values=0x00,0x01,0x5a,0x80,0xff,0x1234 offsets=all
correct sizes=512,768,1023,1024,1025,1536,2047,2048,2049,4095,4096,4097,8191,8192,8193,16383,16384,16385,65536 values=0x00,0x01,0x5a,0x80,0xff,0x1234 offsets=4048,4063,4064,4065,4080
bench iterations=400 bytes=134217728 sizes=512,768,1023,1024,1025,1536,2047,2048,2049,4095,4096,4097,8191,8192,8193,16383,16384,16385,65536 values=0x00,0x5a offsets=0,1,31,63 modes=normal,irqoff'

run_case attribution_direct 'reset
probe=1
correct iterations=120 bytes=33554432 sizes=1024,1025,1536,2047,2048,2049,4095,4096,4097,8191,8192,8193,16383,16384,16385,65536 values=0x00,0x5a offsets=0,1,63
bench iterations=120 bytes=67108864 sizes=1024,1025,2047,2048,2049,4095,4096,4097,8191,8192,8193,16383,16384,16385,65536 values=0x00,0x5a offsets=0,1 modes=normal,irqoff
probe=0'

run_case workload_alloc 'reset
probe=1
workload alloc iterations=6000 size=16384 value=0x5a
workload alloc iterations=6000 size=16384 value=0x00
probe=0'

mkdir -p /tmp/rvv-memset-xattr
mount -t tmpfs -o size=64M tmpfs /tmp/rvv-memset-xattr
run_case workload_xattr 'reset
probe=1
workload xattr iterations=2500 size=2048 value=0x5a path=/tmp/rvv-memset-xattr
probe=0'
umount /tmp/rvv-memset-xattr

echo "== dmesg markers =="
dmesg | grep -Ei 'rvv_memset_eval|memset_large|KASAN|BUG:|Oops|WARNING|WARN|not ok|fail' || true

rmmod rvv_memset_eval
sync
if [ -e /proc/sysrq-trigger ]; then
	echo o > /proc/sysrq-trigger
	sleep 5
else
	poweroff -f
fi
