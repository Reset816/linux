#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

PATH=/sbin:/bin:/usr/sbin:/usr/bin:/tests
export PATH

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

STATS=/sys/kernel/debug/zicboz_memset_attr/stats
RESET=/sys/kernel/debug/zicboz_memset_attr/reset

reset_attr()
{
	[ -e "$RESET" ] && echo 1 > "$RESET"
}

show_attr()
{
	label="$1"
	if [ -e "$STATS" ]; then
		echo "--- attr: $label ---"
		cat "$STATS"
	fi
}

run_attr()
{
	label="$1"
	shift
	reset_attr
	echo "--- workload: $label ---"
	"$@"
	show_attr "$label"
}

echo "=== zicboz memset K3 guest start ==="
uname -a
cat /proc/cpuinfo | sed -n '1,80p'

if [ -f /tests/zicboz_memset_test.ko ]; then
	echo "--- workload: kernel correctness module ---"
	insmod /tests/zicboz_memset_test.ko || true
	rmmod zicboz_memset_test 2>/dev/null || true
fi

echo "--- workload: eval correctness ---"
insmod /tests/zicboz_memset_eval.ko mode=correctness || true
rmmod zicboz_memset_eval 2>/dev/null || true

echo "--- workload: direct memset bench ---"
insmod /tests/zicboz_memset_eval.ko mode=direct passes="${PASSES:-3}" || true
rmmod zicboz_memset_eval 2>/dev/null || true

echo "--- workload: mixed module memset perf ---"
insmod /tests/zicboz_memset_eval.ko mode=mixed mixed_calls="${MIXED_CALLS:-160000}" || true
rmmod zicboz_memset_eval 2>/dev/null || true

if [ -x /tests/zicboz_memset ]; then
	echo "--- workload: userspace zicboz selftest ---"
	/tests/zicboz_memset || true
fi

if [ -x /tests/zicboz_k3_workloads ]; then
	echo "--- workload: anonymous mmap faults perf ---"
	/tests/zicboz_k3_workloads anon "${ANON_MIB:-128}" "${ANON_PASSES:-3}" || true

	mkdir -p /mnt/tmpfs
	if mount -t tmpfs -o size=384m tmpfs /mnt/tmpfs; then
		echo "--- workload: tmpfs sparse/write/truncate perf ---"
		/tests/zicboz_k3_workloads tmpfs /mnt/tmpfs "${FS_MIB:-128}" || true
		umount /mnt/tmpfs || true
	fi

	mkdir -p /mnt/ext4
	if [ -b /dev/vda ] && mount -t ext4 /dev/vda /mnt/ext4; then
		echo "--- workload: ext4 sparse/write/truncate perf ---"
		/tests/zicboz_k3_workloads ext4 /mnt/ext4 "${FS_MIB:-128}" || true
		umount /mnt/ext4 || true
	else
		echo "ext4 workload skipped: /dev/vda unavailable or mount failed"
	fi
fi

insmod /tests/zicboz_memset_attr.ko || echo "attr insmod failed $?"

run_attr "direct memset attribution" insmod /tests/zicboz_memset_eval.ko mode=direct passes=1 || true
rmmod zicboz_memset_eval 2>/dev/null || true

run_attr "mixed module memset attribution" insmod /tests/zicboz_memset_eval.ko mode=mixed mixed_calls=50000 || true
rmmod zicboz_memset_eval 2>/dev/null || true

if [ -x /tests/zicboz_k3_workloads ]; then
	run_attr "anonymous mmap faults attribution" /tests/zicboz_k3_workloads anon 64 1 || true

	mkdir -p /mnt/tmpfs
	if mount -t tmpfs -o size=192m tmpfs /mnt/tmpfs; then
		run_attr "tmpfs sparse/write/truncate attribution" /tests/zicboz_k3_workloads tmpfs /mnt/tmpfs 64 || true
		umount /mnt/tmpfs || true
	fi

	if [ -b /dev/vda ] && mount -t ext4 /dev/vda /mnt/ext4; then
		run_attr "ext4 sparse/write/truncate attribution" /tests/zicboz_k3_workloads ext4 /mnt/ext4 64 || true
		umount /mnt/ext4 || true
	fi
fi

echo "--- final dmesg summary ---"
dmesg | grep -E 'zicboz_memset|memset_eval|memset_attr|direct kind=|mixed calls=|correctness passed|all checks passed|fail|FAIL|Oops|panic' || true
echo "=== zicboz memset K3 guest done ==="

sync
poweroff -f 2>/dev/null || reboot -f 2>/dev/null || echo o > /proc/sysrq-trigger
