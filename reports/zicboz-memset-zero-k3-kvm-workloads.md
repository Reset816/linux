# RISC-V Zicboz memset zero K3 KVM evaluation

Base: `39f90c1967215375f7d87b81d14b0f3ed6b40c29`

Patch under test: `00959d1ae840 ("riscv: use zicboz for large zero memset")`

Board: Spacemit K3 Pico ITX, `qemu-system-riscv64 10.2.1`, KVM guest pinned to host CPU2 with `taskset -c 2`. CPU2 is an X100 core. The host governor was reported as userspace around 2.2GHz.

## Methodology

Two kernels were built from the same tree:

| Kernel | Config difference | Purpose |
| --- | --- | --- |
| fast | `CONFIG_RISCV_ISA_ZICBOZ=y`, `CONFIG_RISCV_ZICBOZ_MEMSET_TEST=m` | exercises the new `__memset(dst, 0, n)` `cbo.zero` path |
| control | `CONFIG_RISCV_ISA_ZICBOZ=n` | forces scalar `__memset` fallback and makes `clear_page()` fall back to `__memset` |

Local build command:

```sh
PATH="$PWD/.tmp-riscv64-linux-prefix:$PATH" \
make ARCH=riscv CROSS_COMPILE=riscv64-linux- -j64 Image modules
```

The prefix directory was a local shim from `riscv64-linux-*` to the installed `riscv64-linux-gnu-*` tools. Helper modules were built with:

```sh
PATH="$PWD/.tmp-riscv64-linux-prefix:$PATH" \
make ARCH=riscv CROSS_COMPILE=riscv64-linux- \
     -C tools/testing/selftests/riscv/zicboz_memset/k3 KDIR="$PWD"
```

Board artifacts were uploaded to:

```text
/home/froster/yuan/zicboz-memset-zero/fast
/home/froster/yuan/zicboz-memset-zero/control
```

Each directory contains `Image`, `rootfs.cpio.gz`, `ext4.img`, `run-k3-board.sh`, and a `tests/` directory. No board packages were installed. The initramfs is self-contained except for board QEMU and KVM.

QEMU command used by `run-k3-board.sh`:

```sh
taskset -c 2 qemu-system-riscv64 \
  -accel kvm -machine virt -cpu host -smp 1 -nographic -no-reboot \
  -bios none -m 2G \
  -kernel "$ARTIFACT_DIR/Image" \
  -initrd "$ARTIFACT_DIR/rootfs.cpio.gz" \
  -drive "file=$ARTIFACT_DIR/ext4.img,format=raw,if=virtio" \
  -append 'console=ttyS0 root=/dev/ram rw rdinit=/test-init'
```

Guest `/proc/cpuinfo` exposed `zicboz`; `riscv_cboz_block_size` was 64 in the fast kernel. The fast kernel also used the existing RISC-V Zicboz `clear_page()` implementation. The control kernel intentionally did not, so anonymous page-fault attribution is not a pure test of the new `memset` path.

## Helper Artifacts

K3-specific helper sources live under:

```text
tools/testing/selftests/riscv/zicboz_memset/k3/
```

Files:

| File | Purpose |
| --- | --- |
| `zicboz_memset_eval.c` | out-of-tree kernel module for correctness, direct memset timing, and mixed zero/nonzero memset workload |
| `zicboz_memset_attr.c` | kprobe/debugfs attribution module for `__memset` arguments on RISC-V (`a0`, `a1`, `a2`) |
| `zicboz_k3_workloads.c` | static userspace anonymous mmap, tmpfs, and ext4 workload driver |
| `run-k3-guest.sh` | initramfs guest runner |
| `run-k3-board.sh` | board-side QEMU+KVM launcher |

The attribution module reports total calls/bytes, zero/nonzero calls/bytes, length buckets `<128`, `128-1K`, `1K-4K`, `>=4K`, and an estimate of full aligned CBO-eligible bytes for zero calls in valid linear addresses.

## Correctness

Fast kernel checks passed:

| Check | Result |
| --- | --- |
| `zicboz_memset_test.ko` linear aligned/unaligned boundary checks | pass |
| `zicboz_memset_test.ko` nonzero fallback | pass |
| `zicboz_memset_test.ko` vmalloc fallback | pass |
| `zicboz_memset_eval.ko mode=correctness` | pass |
| page boundary zeroing | pass |
| userspace RISC-V Zicboz selftest | pass |
| ioremap/MMIO-looking fallback | skipped intentionally; the test does not issue speculative MMIO zeroing |

Control kernel ran the eval correctness module and userspace selftest successfully. `zicboz_memset_test.ko` was not present in the no-Zicboz artifact because that module depends on `CONFIG_RISCV_ZICBOZ_MEMSET_TEST`.

## Dedicated Direct Benchmark

Direct benchmark is a kernel module repeatedly calling generic `memset()` on a `kmalloc()` buffer. Values are median `ns/call` over three passes before kprobe instrumentation.

Aligned `memset(dst, 0, n)`:

| Size | fast | control | fast/control | Result |
| ---: | ---: | ---: | ---: | --- |
| 16 | 15 | 14 | 1.07x | small fallback, noise/slower |
| 32 | 15 | 14 | 1.07x | small fallback, noise/slower |
| 64 | 16 | 15 | 1.07x | small fallback, noise/slower |
| 128 | 18 | 17 | 1.06x | small fallback, noise/slower |
| 255 | 27 | 26 | 1.04x | below threshold, noise/slower |
| 256 | 27 | 21 | 1.29x | slower |
| 512 | 35 | 38 | 0.92x | 8% faster |
| 1024 | 55 | 66 | 0.83x | 17% faster |
| 2048 | 195 | 125 | 1.56x | slower |
| 4096 | 379 | 252 | 1.50x | slower |
| 8192 | 1012 | 483 | 2.10x | slower |
| 16384 | 2387 | 967 | 2.47x | slower |
| 32768 | 5262 | 1905 | 2.76x | slower |
| 65536 | 11463 | 3883 | 2.95x | slower |

Unaligned `memset(dst + 13, 0, n)`:

| Size | fast | control | fast/control | Result |
| ---: | ---: | ---: | ---: | --- |
| 256 | 56 | 28 | 2.00x | slower |
| 512 | 58 | 44 | 1.32x | slower |
| 1024 | 82 | 78 | 1.05x | about even/slower |
| 2048 | 188 | 143 | 1.31x | slower |
| 4096 | 402 | 272 | 1.48x | slower |
| 8192 | 1023 | 533 | 1.92x | slower |
| 16384 | 2390 | 1065 | 2.24x | slower |
| 32768 | 5293 | 2113 | 2.50x | slower |
| 65536 | 11488 | 4201 | 2.73x | slower |

Nonzero aligned control (`memset(dst, 0x5a, n)`) was effectively unchanged. Median fast/control ratios stayed within about -2.3% to +7.1%, with large sizes within roughly +/-2.3%.

Interpretation: on this K3 KVM path, `cbo.zero` is only faster in a narrow aligned 512-1024 byte window. The large-size behavior is a clear regression versus the scalar store loop, despite the operation being CBO-eligible. This makes a general large-zero fast path unattractive for this CPU/KVM combination without CPU/vendor-specific thresholding or a runtime policy.

## Mixed Kernel Workload

The mixed kernel module uses an 82% zero / 18% nonzero distribution over sizes 32 through 32768 bytes and varied offsets in a `kmalloc()` buffer.

| Kernel | calls | zero bytes | nonzero bytes | ns | ns/call | MiB/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| fast | 160000 | 1190776576 | 227463424 | 98777083 | 617 | 13692 |
| control | 160000 | 1190776576 | 227463424 | 104875250 | 655 | 12896 |

Fast/control was 0.942 by elapsed time, a 5.8% improvement. This distribution contains many mid-sized zero calls, where the direct benchmark showed some wins, and enough nonzero calls to model fallback traffic.

Attribution with kprobe overhead:

| Kernel | total bytes | zero bytes | nonzero bytes | estimated eligible zero bytes |
| --- | ---: | ---: | ---: | ---: |
| fast | 443872653 | 372266509 | 71606144 | 368784192 |
| control | 444110221 | 372504077 | 71606144 | 369021760 |

The mixed workload strongly hits generic `__memset` and is mostly CBO-eligible. The direct-size results explain why the aggregate improves only modestly: mid-sized wins are diluted by large-size regressions.

## Broader Workloads

Wall-clock timings without kprobe instrumentation:

| Workload | fast ns | control ns | fast/control | Interpretation |
| --- | ---: | ---: | ---: | --- |
| anonymous mmap fault, 128 MiB x3 | 361955792 | 367906625 | 0.984 | about even; mostly page zeroing, not a pure new-`memset` test |
| tmpfs sparse/write/truncate, 128 MiB | 89305416 | 89015666 | 1.003 | no meaningful change |
| ext4 sparse/write/truncate, 128 MiB | 937679376 | 914316583 | 1.026 | 2.6% slower/noisy |

Attribution runs used smaller 64 MiB jobs and kprobe instrumentation, so the timing numbers are not performance numbers. They are only call-source evidence.

| Workload | Kernel | total `__memset` bytes | zero bytes | eligible zero bytes | Takeaway |
| --- | --- | ---: | ---: | ---: | --- |
| anonymous mmap faults | fast | 103058 | 103058 | 18496 | fast kernel uses Zicboz `clear_page()`, so generic `__memset` is mostly not on the page-fault zeroing path |
| anonymous mmap faults | control | 67555986 | 67555986 | 67471424 | no-Zicboz control makes `clear_page()` tail-call `__memset`, so attribution is not comparable to the new memset optimization alone |
| tmpfs sparse/write/truncate | fast | 239054 | 239054 | 147968 | very little generic `__memset` traffic |
| tmpfs sparse/write/truncate | control | 483958 | 483958 | 397696 | very little generic `__memset` traffic |
| ext4 sparse/write/truncate | fast | 2166825 | 2162729 | 346112 | many tiny calls; little eligible traffic |
| ext4 sparse/write/truncate | control | 2454733 | 2450637 | 636352 | many tiny calls; little eligible traffic |

The broad filesystem workloads do not materially exercise the new generic `memset(dst, 0, n)` fast path. Anonymous fault zeroing is dominated by `clear_page()` behavior, which already had its own Zicboz implementation before this patch.

## Regression Assessment

Observed regressions:

| Area | Result |
| --- | --- |
| nonzero `memset` | no meaningful regression in direct benchmark; code falls back to scalar immediately |
| zero `memset` below 256 bytes | should fall back; observed small differences are measurement noise/build difference, mostly within one or two ns/call |
| aligned zero 256 bytes | slower, 27 ns vs 21 ns |
| aligned zero 2 KiB and larger | significantly slower, growing to 2.95x slower at 64 KiB |
| unaligned zero at and above 256 bytes | consistently slower except near-even 1 KiB |
| tmpfs/ext4 broad workloads | no useful speedup; ext4 run was 2.6% slower in this single run |

The large-size regression is the main blocker for a generic upstream policy. It likely reflects this implementation/hardware/KVM combination: the scalar loop writes cache-resident memory very quickly, while repeated `cbo.zero` has higher per-block cost on the K3 KVM path.

## Upstreamability Interpretation

The implementation is conservative for correctness:

* `cbo.zero` is only attempted for zero fills.
* small sizes fall back to scalar.
* nonzero fills fall back to scalar.
* no-Zicboz, invalid block size, unsupported builds, and purgatory fall back to scalar.
* only the aligned full cache-block interval is cleared with `cbo.zero`; unaligned head/tail bytes use scalar stores.
* destinations outside the direct linear map fall back, avoiding vmalloc and MMIO-looking addresses.
* CBO block-size heterogeneity mismatches disable the extension through the existing alternative guard path.

The performance data does not support enabling this as a broad "large memset zero" optimization for K3 under QEMU+KVM. A future upstreamable version probably needs one of:

* a per-CPU/vendor threshold table and a much higher threshold on K3, possibly disabling the memset path entirely there;
* a static key or alternative policy derived from measured `cbo.zero` throughput;
* narrowing the optimization to CPUs where direct large-zero benchmarks show clear wins.

The existing RISC-V Zicboz `clear_page()` path remains separate and was already present before this optimization. Broad page-fault wins/losses should not be attributed to the new generic `memset` path without isolating `clear_page()`.

## Raw Logs

Complete QEMU logs are on the board:

```text
/home/froster/yuan/zicboz-memset-zero/fast/qemu-fast.log
/home/froster/yuan/zicboz-memset-zero/control/qemu-control.log
```

Local parsed copies during this run were stored under `/tmp/zicboz-k3-logs/`.
