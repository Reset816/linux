# RISC-V memcpy-fast-unaligned subsystem workload evaluation on Spacemit K3 KVM

Date: 2026-06-01

Worktree: `/home/tanyuan-cve/data/repos/linux-repos/linux-riscv-opt-memcpy-fast-unaligned`

Branch: `riscv-opt/memcpy-fast-unaligned`

Board: Spacemit K3 Pico ITX, QEMU+KVM, QEMU pinned to host CPU 2.

## Summary

The patch optimizes a narrow cliff: generic kernel `__memcpy()` for large copies whose source and destination low alignment bits differ, when the RISC-V unaligned-access probe/static key says scalar unaligned access is fast.

The targeted kernel module benchmark still shows that cliff clearly. For `dst+1/src+2` copies, forced fast reached 8.3-10.0 GiB/s for 1 KiB to 64 KiB copies, while forced slow stayed near 1.0 GiB/s. The mixed direct-`__memcpy()` benchmark improved from 2326 MiB/s to 10195 MiB/s with about 39% of copied bytes mismatched.

The subsystem workloads are more nuanced:

- `ext4_xattr` naturally hit generic `__memcpy()` with mismatched 1K-4K copies: 748800 mismatched bytes out of 1214346 traced bytes in the auto run. End-to-end time did not improve; fast/slow/auto means were 1.117 s, 1.112 s, and 1.112 s. The xattr path does expose relevant copies, but this workload is dominated by filesystem/xattr/fsync overhead.
- `ext4_meta`, `squashfs_lz4_read`, `f2fs_compress`, and `control_user_memcpy` did hit `__memcpy()`, but their mismatched bytes were small-copy buckets only. They did not exercise the optimized large mismatched-copy cliff, and no meaningful end-to-end improvement should be claimed from them.
- No clear regression was observed. Fast/slow/auto differences in subsystem timings were within the small run-to-run spread for this KVM setup. The largest apparent change, f2fs fast vs slow, favored fast by about 2.2%, but the min/max ranges overlap enough that I would not treat it as a demonstrated workload speedup.

## Local build and artifacts

The local cross compiler is installed as `riscv64-linux-gnu-*`, so the build used the existing `/tmp/riscv64-linux-shim` symlink prefix to satisfy `CROSS_COMPILE=riscv64-linux-`.

Kernel config/build:

```sh
PATH=/tmp/riscv64-linux-shim:$PATH make ARCH=riscv CROSS_COMPILE=riscv64-linux- \
  O=/tmp/riscv-memcpy-kvm-build olddefconfig

PATH=/tmp/riscv64-linux-shim:$PATH make ARCH=riscv CROSS_COMPILE=riscv64-linux- \
  O=/tmp/riscv-memcpy-kvm-build -j64
```

Relevant config was enabled for timing and attribution:

```text
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
CONFIG_PERF_EVENTS=y
CONFIG_RISCV_PROBE_UNALIGNED_ACCESS=y
CONFIG_KPROBES=y
CONFIG_KPROBE_EVENTS=y
CONFIG_DEBUG_FS=y
CONFIG_EXT4_FS=y
CONFIG_EXT4_FS_XATTR=y
CONFIG_BLK_DEV_LOOP=y
CONFIG_SQUASHFS=y
CONFIG_SQUASHFS_LZ4=y
CONFIG_SQUASHFS_LZO=y
CONFIG_F2FS_FS=y
CONFIG_F2FS_FS_COMPRESSION=y
CONFIG_F2FS_FS_LZ4=y
# CONFIG_KASAN is not set
# CONFIG_KUNIT is not set
```

Selftest/module build:

```sh
PATH=/tmp/riscv64-linux-shim:$PATH make -C tools/testing/selftests/riscv/memcpy \
  ARCH=riscv CROSS_COMPILE=riscv64-linux- \
  OUTPUT=/tmp/riscv-memcpy-subsystem-selftest \
  KDIR=/tmp/riscv-memcpy-kvm-build -j64
```

Filesystem images were prepared locally:

```sh
truncate -s 160M /tmp/riscv-memcpy-subsystem-assets/ext4.img
/usr/sbin/mkfs.ext4 -F -q -O ^metadata_csum_seed /tmp/riscv-memcpy-subsystem-assets/ext4.img

truncate -s 160M /tmp/riscv-memcpy-subsystem-assets/f2fs.img
/usr/sbin/mkfs.f2fs -f -O extra_attr,compression /tmp/riscv-memcpy-subsystem-assets/f2fs.img

mksquashfs /tmp/riscv-memcpy-subsystem-assets/squash-tree \
  /tmp/riscv-memcpy-subsystem-assets/squashfs-lz4.img \
  -comp lz4 -b 131072 -noappend
```

The initramfs used the Buildroot RISC-V rootfs under `/home/tanyuan-cve/data/repos/linux-repos/riscv-rootfs/buildroot-2026.02.2/output/riscv64-rootfs/images/rootfs.cpio`, plus `/root/memcpy_user_test`, `/root/memcpy_general_workload`, `/root/memcpy_subsystem_workload`, `/root/test_riscv_memcpy_fast_unaligned.ko`, `/root/trace_riscv_memcpy_args.ko`, and `/root/run_subsystem_workloads.sh`.

## Board artifacts

Uploaded under:

```text
/home/froster/yuan/memcpy-fast-unaligned/subsystem-workloads
```

Useful retained files:

```text
Image
rootfs.cpio.gz
anchor-rootfs.cpio.gz
ext4-base.img
f2fs-base.img
squashfs-lz4.img
run_qemu_subsystem.sh
eval-fast-clean.log
eval-slow-clean.log
eval-auto-clean.log
eval-anchor-clean.log
```

Transient per-mode ext4/f2fs working images were removed after the run.

## QEMU commands

Subsystem runner command, with mode-specific image copies and mode-specific command-line override:

```sh
taskset -c 2 qemu-system-riscv64 \
  -accel kvm -machine virt -cpu host -smp 1 -m 1536M \
  -nographic -no-reboot -bios none \
  -kernel Image -initrd rootfs.cpio.gz \
  -drive file=ext4-fast.img,if=virtio,format=raw \
  -drive file=squashfs-lz4.img,if=virtio,format=raw,readonly=on \
  -drive file=f2fs-fast.img,if=virtio,format=raw \
  -append "console=ttyS0 earlycon=sbi rdinit=/root/run_subsystem_workloads.sh panic=-1 unaligned_scalar_speed=fast"
```

The same command was run with `unaligned_scalar_speed=slow`, and with no override for auto-probe.

Anchor-only command:

```sh
taskset -c 2 qemu-system-riscv64 \
  -accel kvm -machine virt -cpu host -smp 1 -m 1024M \
  -nographic -no-reboot -bios none \
  -kernel Image -initrd anchor-rootfs.cpio.gz \
  -append "console=ttyS0 earlycon=sbi rdinit=/root/run_anchor.sh panic=-1 unaligned_scalar_speed=fast"
```

`-cpu host` worked. `-bios none` was required for this KVM setup.

Host pinning/frequency observations from the runner:

```text
affinity=pid ... current affinity list: 0-7
cpu2_scaling_cur_freq=2200000
cpu2_cpuinfo_max_freq=2400000
cpu2_scaling_governor=userspace
```

The runner was launched with `taskset -c 2`; CPU 2 is an X100 core.

## Guest observations

Guest ISA under `-cpu host`:

```text
rv64imafdcv_zicbom_zicboz_zicntr_zicond_zicsr_zifencei_zihintntl_zihintpause_zihpm_zimop_zaamo_zalrsc_zawrs_zfa_zfh_zfhmin_zca_zcb_zcd_zcmop_zba_zbb_zbc_zbs_zkt_zvbb_zvbc_zve32f_zve32x_zve64d_zve64f_zve64x_zvfh_zvfhmin_zvkb_zvkg_zvkned_zvknha_zvknhb_zvksed_zvksh_zvkt_smstateen_ssaia_sscofpmf_sstc_svinval_svnapot_svpbmt
```

Unaligned state:

- Forced fast: `scalar unaligned access speed set to 'fast' (3) by command line`; userspace hwprobe reported `3`.
- Forced slow: `scalar unaligned access speed set to 'slow' (2) by command line`; userspace hwprobe reported `2`.
- Auto: scalar probe reported `Ratio of byte access time to unaligned word access is 4.68, unaligned accesses are fast`; userspace hwprobe reported `3`.

The guest does not print a `riscv_hwprobe` line in `/proc/cpuinfo`; the userspace selftest's hwprobe syscall output was used instead.

## Attribution method

The temporary module `trace_riscv_memcpy_args.ko` installs a kprobe on `__memcpy`. On RISC-V it reads:

```text
a0 = dst
a1 = src
a2 = len
```

It exposes `/proc/riscv_memcpy_trace`:

- writing `1` resets counters and enables tracing;
- writing `0` disables tracing;
- reading reports call count, total bytes, length buckets, same-alignment bytes, mismatched-alignment bytes, and mismatched bytes by length bucket.

Timing and tracing were deliberately separated. Each workload first ran five uninstrumented timing samples. Then it ran one attribution sample with the kprobe enabled. Kprobe attribution is therefore not used as timing data.

Alignment mismatch means:

```c
((dst ^ src) & 7) != 0
```

Buckets are `<128`, `128-1023`, `1024-4095`, and `>=4096`.

## Correctness

Correctness passed in fast, slow, and auto modes:

```text
ok 1 all size/alignment memcpy cases
correctness passed for all src/dst low-bit combinations
```

## Anchor: direct kernel `__memcpy()` benchmark

Throughput in MiB/s. These are direct kernel module calls to `__memcpy()`.

| Case | Forced fast | Forced slow | Auto |
|---|---:|---:|---:|
| 1 KiB dst+1/src+2 | 8293 | 1019 | 8613 |
| 4 KiB dst+1/src+2 | 8800 | 1034 | 8752 |
| 16 KiB dst+1/src+2 | 9166 | 1038 | 9630 |
| 64 KiB dst+1/src+2 | 10029 | 1033 | 9935 |
| 64 KiB dst+0/src+0 | 16070 | 15844 | 15913 |
| 64 KiB dst+1/src+1 | 15229 | 14553 | 14483 |

The forced-fast mismatched cases are about 8.1x to 9.7x faster than forced slow in this run. Same-alignment cases are similar across modes, which is the intended non-regression behavior.

## Anchor: mixed direct-`__memcpy()` benchmark

This benchmark uses deterministic random-ish sizes and alignments:

```text
calls=9864
bytes=67112488
same_calls=6165
mismatch_calls=3699
same_bytes=40966448
mismatch_bytes=26146040
mismatch byte share=38.96%
```

| Mode | Time ns | MiB/s |
|---|---:|---:|
| Forced fast | 6277833 | 10195 |
| Forced slow | 27515458 | 2326 |
| Auto | 6286292 | 10181 |

This is still a direct `__memcpy()` benchmark, but it is a less pure worst case than the fixed-offset anchor. It improved 4.4x from forced slow to forced fast.

## Subsystem workloads

Each workload used five uninstrumented timing samples per mode.

- `ext4_meta`: create/write/fsync/rename/unlink/fsync-dir for 320 small files on an ext4 virtio-block image mounted `rw,sync,user_xattr,acl`.
- `ext4_xattr`: for 96 files, set/get/remove `user.big` xattrs with 3900-byte values, near the filesystem block-size scale.
- `squashfs_lz4_read`: mount a prebuilt LZ4 squashfs image and recursively read/hash 1056 regular files, 6206176 uncompressed bytes.
- `f2fs_compress`: f2fs image created with `extra_attr,compression`, mounted with `compress_algorithm=lz4,compress_extension=*`; write/fsync/read 48 compressible 64 KiB files.
- `control_user_memcpy`: tmpfs userspace file copy from the previous broader workload runner; included as a case expected not to benefit from generic kernel `__memcpy()`.

## End-to-end timing

Times are seconds, five samples per mode.

| Workload | Fast mean | Fast min/max | Slow mean | Slow min/max | Auto mean | Auto min/max |
|---|---:|---:|---:|---:|---:|---:|
| ext4_meta | 2.548 | 2.523 / 2.577 | 2.541 | 2.525 / 2.558 | 2.523 | 2.503 / 2.545 |
| ext4_xattr | 1.117 | 1.102 / 1.142 | 1.112 | 1.097 / 1.125 | 1.112 | 1.102 / 1.139 |
| squashfs_lz4_read, all samples | 0.04285 | 0.01230 / 0.16109 | 0.04329 | 0.01223 / 0.16309 | 0.04284 | 0.01226 / 0.16101 |
| squashfs_lz4_read, warm samples 1-4 | 0.01329 | 0.01230 / 0.01618 | 0.01335 | 0.01223 / 0.01633 | 0.01330 | 0.01226 / 0.01620 |
| f2fs_compress | 0.230 | 0.228 / 0.233 | 0.235 | 0.227 / 0.247 | 0.234 | 0.229 / 0.237 |
| control_user_memcpy, all samples | 0.03463 | 0.02363 / 0.07837 | 0.03481 | 0.02381 / 0.07827 | 0.03466 | 0.02364 / 0.07808 |
| control_user_memcpy, warm samples 1-4 | 0.02369 | 0.02363 / 0.02373 | 0.02394 | 0.02381 / 0.02417 | 0.02380 | 0.02364 / 0.02404 |

Interpretation:

- `ext4_xattr` is the best natural subsystem hit for this patch's shape, but end-to-end time did not move because the optimized copies are a small part of the total operation.
- The squashfs first sample is a cold-cache outlier; warm samples are much faster. Neither cold nor warm samples show a mode-dependent effect.
- The f2fs mean favors fast slightly, but slow has a broad sample range and overlaps fast/auto. Treat this as no demonstrated speedup.
- `control_user_memcpy` is a negative/control workload. Warm samples differ by about 1%, with no meaningful regression.

## `__memcpy` attribution, auto mode

One kprobe-instrumented sample per workload, separate from timing.

| Workload | Calls | Bytes | Mismatch bytes | Mismatch share | Mismatch <128 | Mismatch 128-1023 | Mismatch 1024-4095 | Mismatch >=4096 | Total >=4096 bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ext4_meta | 3401 | 111424 | 39518 | 35.47% | 39518 | 0 | 0 | 0 | 49152 |
| ext4_xattr | 1295 | 1214346 | 759424 | 62.54% | 10624 | 0 | 748800 | 0 | 442368 |
| squashfs_lz4_read | 4457 | 100727 | 34967 | 34.71% | 34967 | 0 | 0 | 0 | 49152 |
| f2fs_compress | 2657 | 1549686 | 140723 | 9.08% | 56594 | 84129 | 0 | 0 | 1277952 |
| control_user_memcpy | 111 | 60236 | 121 | 0.20% | 121 | 0 | 0 | 0 | 49152 |

The key line is `ext4_xattr`: it generated 192 mismatched 1024-4095-byte `__memcpy()` calls, accounting for 748800 bytes. This is relevant to the optimization's alignment cliff, though still below the `>=4096` bucket. The larger `>=4096` copies in all measured subsystem workloads were same-aligned.

## Regression analysis

No clear slowdown was measured for workloads expected not to benefit:

- Same-aligned 64 KiB direct `__memcpy()` stayed close: 16070 MiB/s fast, 15844 MiB/s slow, 15913 MiB/s auto.
- `ext4_meta` mismatched bytes were all `<128`, and fast/slow/auto timing means stayed within 1.0%.
- `squashfs_lz4_read` mismatched bytes were all `<128`; warm means were 13.29 ms, 13.35 ms, and 13.30 ms.
- `control_user_memcpy` traced only 121 mismatched `__memcpy()` bytes; warm means were 23.69 ms, 23.94 ms, and 23.80 ms.

The direct benchmark does show that same low alignment but unaligned addresses, e.g. `dst+1/src+1`, can remain slower than fully aligned `dst+0/src+0`; that is pre-existing behavior in the generic memcpy shape and is not the target of this patch.

## Caveats and failed/partial attempts

- The subsystem timing pass and kprobe attribution pass are separate. This avoids kprobe overhead in timing, but attribution is not collected at exactly the same instant as the timed samples.
- The squashfs attribution run happens after the timed read pass, so it is warm-cache attribution. In this setup that is acceptable for copy-shape attribution, but it is not a cold-read profile.
- The squashfs image is very compressible: 6206176 uncompressed bytes in a 68 KiB image. This makes it useful for metadata/decompression/read-path coverage, not for storage throughput claims.
- f2fs compression mounted successfully with `compress_algorithm=lz4,compress_extension=*`, and the workload completed, but compressed physical block savings were not independently verified inside the minimal guest.
- The initial refreshed initramfs attempts failed twice before the final run: first with a host-architecture BusyBox (`ENOEXEC`), then with a non-root setuid BusyBox after repacking without root ownership. Final cpio repack used the RISC-V Buildroot rootfs and `cpio --owner=0:0`; the clean logs in this report are from the fixed run.
- Generic `__memcpy` attribution does not include libc memcpy, copy_to/from_user helpers, page remapping, checksum paths, or other architecture-specific copy routines.
- This is single-vCPU QEMU+KVM pinned to one K3 X100 host core. It validates guest behavior and relative fast/slow/auto mode effects under KVM; it is not a multi-core scalability test.

## Conclusion

The optimization remains well justified for the intended cliff: large generic kernel `__memcpy()` with mismatched low alignment bits on hardware where scalar unaligned access is fast. The subsystem runs found one plausible natural source of mismatched generic `__memcpy()` traffic, ext4 xattr values near block size, but not enough end-to-end weight to show a workload-level speedup. The other subsystem/control workloads mostly did not generate large mismatched generic `__memcpy()` calls, and they showed no meaningful regression.
