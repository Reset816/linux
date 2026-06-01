#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

ROOTFS_IMAGES="${ROOTFS_IMAGES:-/home/tanyuan-cve/data/repos/linux-repos/riscv-rootfs/buildroot-2026.02.2/output/riscv64-rootfs/images}"
IMAGE="${IMAGE:-arch/riscv/boot/Image}"
SELFTEST_BIN="${SELFTEST_BIN:-tools/testing/selftests/riscv/out/zicboz_memset/zicboz_memset}"
HOST_CPU="${HOST_CPU:-10}"
QEMU_CPU="${QEMU_CPU:-max,zkr=false,zk=false,zkn=false,zks=false}"
TIMEOUT="${TIMEOUT:-300}"
LOG="${LOG:-/tmp/zicboz_qemu.log}"

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/zicboz-qemu.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

overlay="$tmpdir/overlay"
mkdir -p "$overlay/tests"

cp arch/riscv/kernel/tests/zicboz_memset_test.ko "$overlay/tests/"
cp "$SELFTEST_BIN" "$overlay/tests/"

if [ -f lib/tests/memcpy_kunit.ko ]; then
	cp lib/tests/memcpy_kunit.ko "$overlay/tests/"
fi

if [ -f lib/tests/test_kprobes.ko ]; then
	cp lib/tests/test_kprobes.ko "$overlay/tests/"
fi

cat > "$overlay/test-init" <<'INIT_EOF'
#!/bin/sh
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/tests

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true

echo "=== zicboz-memset-zero qemu test start ==="
uname -a
cat /proc/cpuinfo | sed -n '1,80p'

echo "--- modules ---"
insmod /tests/zicboz_memset_test.ko || echo "zicboz_memset_test insmod failed $?"

if [ -f /tests/memcpy_kunit.ko ]; then
	insmod /tests/memcpy_kunit.ko || echo "memcpy_kunit insmod failed $?"
fi

if [ -f /tests/test_kprobes.ko ]; then
	insmod /tests/test_kprobes.ko || echo "test_kprobes insmod failed $?"
fi

echo "--- selftest ---"
/tests/zicboz_memset
selftest_rc=$?
echo "zicboz selftest rc=$selftest_rc"

echo "--- filtered dmesg ---"
dmesg | grep -E 'zicboz_memset_test|memcpy|kunit|KTAP|kprobes|RISCV|riscv|cboz|Zicboz|fail|FAIL|ok |not ok' || true
echo "=== zicboz-memset-zero qemu test done ==="

sync
poweroff -f 2>/dev/null || reboot -f 2>/dev/null || echo o > /proc/sysrq-trigger
INIT_EOF
chmod +x "$overlay/test-init"

(cd "$overlay" && find . -print0 | cpio --null -ov --format=newc --quiet > "$tmpdir/overlay.cpio")
cat "$ROOTFS_IMAGES/rootfs.cpio" "$tmpdir/overlay.cpio" | gzip -9 > "$tmpdir/initramfs.cpio.gz"

exec taskset -c "$HOST_CPU" timeout "$TIMEOUT" \
	qemu-system-riscv64 \
	-machine virt \
	-cpu "$QEMU_CPU" \
	-nographic \
	-m 2G \
	-smp 1 \
	-kernel "$IMAGE" \
	-initrd "$tmpdir/initramfs.cpio.gz" \
	-append 'console=ttyS0 root=/dev/ram rw earlycon rdinit=/test-init' \
	-no-reboot 2>&1 | tee "$LOG"
