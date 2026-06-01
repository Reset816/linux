# RISC-V memcpy-fast-unaligned general workload evaluation on Spacemit K3 KVM

Date: 2026-06-01

Worktree: `/home/tanyuan-cve/data/repos/linux-repos/linux-riscv-opt-memcpy-fast-unaligned`

Branch: `riscv-opt/memcpy-fast-unaligned`

Board: Spacemit K3 Pico ITX, QEMU+KVM, QEMU pinned to host CPU 2.

## Summary

The targeted kernel `__memcpy()` mismatched-alignment benchmark still shows the expected large benefit from the fast unaligned path. For 1 KiB to 64 KiB `dst+1/src+2` copies, forced fast was about 6.9x to 8.0x faster than forced slow in this run. A deterministic mixed-alignment kernel benchmark, with about 39% of copied bytes using mismatched low alignment bits, improved from 2731 MiB/s to 10305 MiB/s, or about 3.8x.

The broader tmpfs, pipe, TCP loopback, tar, gzip, and compute workloads did not show a clear end-to-end benefit. Their fast-vs-slow differences were small enough to treat as noise for this setup. Kprobe instrumentation successfully attached to `__memcpy`, but counted zero `__memcpy` events for those broader workloads, which means these particular paths mostly avoid the generic kernel `__memcpy()` symbol that this patch optimizes. They may use user-space libc copies, arch copy-to/from-user routines, page-cache/page-table operations, networking skb helpers, checksumming, or other copy paths instead.

So the optimization is valuable when the kernel actually performs large generic `__memcpy()` calls with differing source/destination low alignment bits. This evaluation does not support claiming broad speedups for common tmpfs, pipe, loopback TCP, tar, gzip, or compute workloads.

## Implementation additions for this evaluation

I extended the local selftest artifacts used for evaluation:

- `tools/testing/selftests/riscv/memcpy/test_modules/test_riscv_memcpy_fast_unaligned.c`
  - Added `mixed_bench`, a deterministic kernel-space `__memcpy()` benchmark with random-ish sizes and alignments.
  - It reports call/byte split between same low alignment and mismatched low alignment.
- `tools/testing/selftests/riscv/memcpy/memcpy_general_workload.c`
  - Added a static guest workload runner for tmpfs file copy, pipe copy, TCP loopback, command-wrapped tar/gzip cases, and a compute control.
- `tools/testing/selftests/riscv/memcpy/Makefile`
  - Builds `memcpy_general_workload` alongside the existing userspace correctness test.

## Local build

The local cross compiler exists as `riscv64-linux-gnu-*`, so the build used the existing `/tmp/riscv64-linux-shim` symlink prefix to satisfy the requested `CROSS_COMPILE=riscv64-linux-`.

Kernel config/build:

```sh
PATH=/tmp/riscv64-linux-shim:$PATH make ARCH=riscv CROSS_COMPILE=riscv64-linux- O=/tmp/riscv-memcpy-kvm-build defconfig

./scripts/config --file /tmp/riscv-memcpy-kvm-build/.config \
  -e MODULES -e MODULE_UNLOAD -e PERF_EVENTS \
  -e RISCV_PROBE_UNALIGNED_ACCESS -e RISCV_MISALIGNED -e RISCV_SCALAR_MISALIGNED \
  -e PROC_FS -e SYSFS -e DEBUG_FS -e DEVTMPFS -e DEVTMPFS_MOUNT -e TMPFS -e BLK_DEV_INITRD \
  -e IKCONFIG -e IKCONFIG_PROC -e KALLSYMS -e KALLSYMS_ALL \
  -e KPROBES -e KPROBE_EVENTS -e DYNAMIC_EVENTS -e FPROBE_EVENTS \
  -e FUNCTION_TRACER -e DYNAMIC_FTRACE -e FUNCTION_GRAPH_TRACER \
  -d KASAN -d KUNIT -d MEMCPY_KUNIT_TEST -d STRING_KUNIT_TEST -d FORTIFY_KUNIT_TEST

PATH=/tmp/riscv64-linux-shim:$PATH make ARCH=riscv CROSS_COMPILE=riscv64-linux- O=/tmp/riscv-memcpy-kvm-build olddefconfig
PATH=/tmp/riscv64-linux-shim:$PATH make ARCH=riscv CROSS_COMPILE=riscv64-linux- O=/tmp/riscv-memcpy-kvm-build -j64
```

Selftest/module build:

```sh
PATH=/tmp/riscv64-linux-shim:$PATH make -C tools/testing/selftests/riscv/memcpy \
  ARCH=riscv CROSS_COMPILE=riscv64-linux- \
  OUTPUT=/tmp/riscv-memcpy-general-selftest \
  KDIR=/tmp/riscv-memcpy-kvm-build -j64
```

The initramfs was built by concatenating the Buildroot `rootfs.cpio.gz` with a small gzip-compressed overlay containing:

- `/root/memcpy_user_test`
- `/root/memcpy_general_workload`
- `/root/test_riscv_memcpy_fast_unaligned.ko`
- `/root/run_general_workloads.sh`

Artifacts:

- `/tmp/riscv-memcpy-kvm-build/arch/riscv/boot/Image`
- `/tmp/riscv-memcpy-general-rootfs.cpio.gz`

## Board artifacts

Uploaded under:

```text
/home/froster/yuan/memcpy-fast-unaligned/general-workloads
```

Final board contents:

```text
Image
rootfs.cpio.gz
run_qemu_general.sh
eval-fast.log
eval-slow.log
eval-auto.log
SHA256SUMS
```

Final checksums on the board:

```text
84f39a6e6e3bb8d7bb2e6889f4b040e48d19e35a4c592849070175b474544f3d  Image
2f0b2c428192e804b49c5225fa179d9808f7ce9b52e58dbd9b48ae4907f21e05  rootfs.cpio.gz
874f3edaf88d8dec80e0eb4045189ce9cbfa3e60263513c970fab41161cb25f6  run_qemu_general.sh
ef1dbd151df47ed8c0b675f61c29582dcb2def62b937496aafacc86ce291e9fc  eval-fast.log
f1778093c99b7ecbaf8aedcb0dc799aa0d091c81dfe921bb8c279a583d28c270  eval-slow.log
fabc8da84b3c49714ec6c8803c8800edc096c7a33c037587a4d488ad0d1a9b64  eval-auto.log
```

No extra software was installed on the board. The guest workload was self-contained in the initramfs overlay.

## QEMU command

The board runner used:

```sh
taskset -c 2 qemu-system-riscv64 \
  -accel kvm -machine virt -cpu host -smp 1 -m 1024M \
  -nographic -no-reboot -bios none \
  -kernel Image -initrd rootfs.cpio.gz \
  -append "console=ttyS0 earlycon=sbi rdinit=/root/run_general_workloads.sh panic=-1 unaligned_scalar_speed=fast"
```

The same command was run with `unaligned_scalar_speed=slow`, and with no override for auto-probe.

`-cpu host` worked. `-bios none` was required because machine-mode firmware is not supported with this KVM setup.

Host CPU pinning/frequency observations:

- QEMU process pinned with `taskset -c 2`.
- CPU 2 is an X100 core.
- `cpu2_scaling_cur_freq=2200000`
- `cpu2_cpuinfo_max_freq=2400000`
- `cpu2_scaling_governor=userspace`

## Guest observations

Guest ISA under `-cpu host`:

```text
rv64imafdcv_zicbom_zicboz_zicntr_zicond_zicsr_zifencei_zihintntl_zihintpause_zihpm_zimop_zaamo_zalrsc_zawrs_zfa_zfh_zfhmin_zca_zcb_zcd_zcmop_zba_zbb_zbc_zbs_zkt_zvbb_zvbc_zve32f_zve32x_zve64d_zve64f_zve64x_zvfh_zvfhmin_zvkb_zvkg_zvkned_zvknha_zvknhb_zvksed_zvksh_zvkt_smstateen_ssaia_sscofpmf_sstc_svinval_svnapot_svpbmt
```

Other guest CPU fields:

- `mmu: sv39`
- `mvendorid: 0x710`
- `marchid: 0x8000000058000002`
- `mimpid: 0x33d8a600`

Scalar unaligned state:

- Forced fast: dmesg reported `scalar unaligned access speed set to 'fast' (3) by command line`; userspace hwprobe reported `3`.
- Forced slow: dmesg reported `scalar unaligned access speed set to 'slow' (2) by command line`; userspace hwprobe reported `2`.
- Auto: scalar probe reported `Ratio of byte access time to unaligned word access is 5.23, unaligned accesses are fast`; userspace hwprobe reported `3`.

Tracing config in the guest:

```text
CONFIG_PERF_EVENTS=y
CONFIG_RISCV_PROBE_UNALIGNED_ACCESS=y
CONFIG_KPROBES=y
CONFIG_FTRACE=y
CONFIG_KPROBE_EVENTS=y
```

The kprobe event `p:memcpy_count __memcpy` attached successfully. Function-tracer filtering on `__memcpy` failed, likely because this assembly symbol is not available as an ftrace callsite. Kprobe counts were therefore used for broad workload attribution.

## Correctness

Correctness passed in fast, slow, and auto modes:

- Userspace: `ok 1 all size/alignment memcpy cases`
- Kernel module: `correctness passed for all src/dst low-bit combinations`
- Kernel module return: `module_ret=0`

## Targeted kernel `__memcpy()` anchor

Throughput in MiB/s. These are direct kernel module calls to `__memcpy()`.

| Case | Forced fast | Forced slow | Auto |
|---|---:|---:|---:|
| 1 KiB dst+1/src+2 | 8488 | 1228 | 8473 |
| 4 KiB dst+1/src+2 | 9355 | 1245 | 9028 |
| 16 KiB dst+1/src+2 | 9990 | 1251 | 9127 |
| 64 KiB dst+1/src+2 | 9799 | 1249 | 9743 |
| 64 KiB dst+3/src+5 | 9836 | 1249 | 9842 |
| 64 KiB dst+7/src+1 | 9785 | 1248 | 9724 |

Interpretation: the targeted mismatched-alignment case remains a large win. Forced fast is about 6.9x to 8.0x faster than forced slow in this run.

## Mixed-alignment kernel benchmark

The mixed benchmark used deterministic random-ish sizes and alignments:

- 9864 calls
- 67,112,488 copied bytes
- 6,165 same-alignment calls
- 3,699 mismatched-alignment calls
- 40,966,448 same-alignment bytes
- 26,146,040 mismatched-alignment bytes

| Mode | Time ns | MiB/s |
|---|---:|---:|
| Forced fast | 6,210,584 | 10,305 |
| Forced slow | 23,427,625 | 2,731 |
| Auto | 6,241,584 | 10,254 |

Interpretation: this is still a direct `__memcpy()` workload, but it is less artificial than the pure worst-case anchor. With about 39% of bytes using mismatched low alignment bits, forced fast was about 3.8x faster than forced slow.

## Broad workload results

Each broad workload ran 7 samples per mode. The first sample is excluded from the summary below because tmpfs/page-cache setup has a visible warmup effect, especially for `filecopy_tmpfs`. Values are warm-sample mean/min/max.

| Workload | Fast mean | Fast min/max | Slow mean | Slow min/max | Auto mean | Auto min/max |
|---|---:|---:|---:|---:|---:|---:|
| filecopy_tmpfs MiB/s | 1341.1 | 1326.1 / 1357.1 | 1335.0 | 1284.6 / 1355.6 | 1333.3 | 1280.2 / 1370.9 |
| pipe_copy MiB/s | 2767.0 | 2734.8 / 2787.9 | 2757.8 | 2740.9 / 2774.8 | 2747.2 | 2729.6 / 2757.9 |
| tcp_loopback MiB/s | 1845.7 | 1799.1 / 1925.1 | 1888.9 | 1759.6 / 2147.8 | 2078.0 | 2016.0 / 2103.3 |
| tar_tmpfs MiB/s | 332.7 | 320.2 / 343.2 | 333.4 | 323.9 / 342.9 | 333.6 | 323.9 / 343.6 |
| gzip_tmpfs MiB/s | 8.3 | 8.3 / 8.3 | 8.3 | 8.3 / 8.3 | 8.3 | 8.3 / 8.3 |
| compute_xorshift pseudo-MiB/s | 61.5 | 61.0 / 62.4 | 61.2 | 60.6 / 61.9 | 61.6 | 61.3 / 62.0 |

Warm-sample mean time in milliseconds:

| Workload | Fast | Slow | Auto |
|---|---:|---:|---:|
| filecopy_tmpfs | 47.725 | 47.956 | 48.030 |
| pipe_copy | 46.262 | 46.414 | 46.594 |
| tcp_loopback | 34.695 | 34.049 | 30.806 |
| tar_tmpfs | 54.146 | 54.025 | 53.994 |
| gzip_tmpfs | 2167.405 | 2166.991 | 2167.509 |
| compute_xorshift | 1861.359 | 1869.006 | 1857.163 |

Interpretation:

- `filecopy_tmpfs`, `pipe_copy`, `tar_tmpfs`, `gzip_tmpfs`, and `compute_xorshift` show no meaningful fast-vs-slow effect.
- `tcp_loopback` varied more than the others. Auto was faster in this run, but kprobe still saw zero `__memcpy` hits, so this should not be attributed to the memcpy optimization.
- `gzip_tmpfs` and `compute_xorshift` are mostly controls. They behaved as expected: no meaningful effect from the kernel memcpy static key.

## `__memcpy` attribution for broad workloads

The guest attached:

```text
kprobe_memcpy=ok
```

For every broad workload and mode, the kprobe count was zero:

| Workload | Fast | Slow | Auto |
|---|---:|---:|---:|
| filecopy_tmpfs | 0 | 0 | 0 |
| pipe_copy | 0 | 0 | 0 |
| tcp_loopback | 0 | 0 | 0 |
| tar_tmpfs | 0 | 0 | 0 |
| gzip_tmpfs | 0 | 0 | 0 |
| compute_xorshift | 0 | 0 | 0 |

This explains why the broad workloads do not track the targeted kernel module improvement. They do not appear to exercise the generic `__memcpy()` symbol optimized by this patch in this guest/configuration.

## Failed or adjusted attempts

- Initial trace setup used `/sys/kernel/debug/tracing` with direct shell redirections. In the minimal initramfs, a failed redirection could terminate PID 1. The harness was fixed to mount/use `/sys/kernel/tracing` and perform trace writes in guarded subshells.
- The first tar-tree setup attempted to read `/dev/zero`, but the minimal initramfs initially did not have the expected device node. The final harness mounts devtmpfs and creates the tar input tree by copying the static workload binary instead.
- The first TCP loopback attempt failed because loopback had not been brought up. The final harness runs `ip link set lo up` before TCP tests.
- Function ftrace filtering for `__memcpy` failed. Kprobe events did attach and were used for event counts.

## Caveats

- This is a single-vCPU QEMU+KVM guest pinned to one K3 X100 host core. Results may differ on bare metal or with multiple guest CPUs.
- CPU 2 was running at 2.2 GHz with a 2.4 GHz max and `userspace` governor. Some run-to-run noise remains.
- The performance kernel disables KASAN and KUnit to avoid benchmark distortion.
- `-cpu host` does not expose every host extension. In particular, earlier host context mentioned `zicbop`, but the guest ISA did not include it. This does not affect the scalar unaligned memcpy path measured here.
- The broad workloads are intentionally simple and constrained by the Buildroot initramfs. They are useful probes, not a replacement for application-level benchmarking.
- The broad workload kprobe evidence is symbol-level. It shows that generic `__memcpy()` was not hit, but it does not quantify copies performed through other arch helpers or in userspace libc.

