# RVV memcpy wrapper K3 QEMU/KVM workload report

## Setup

- Worktree: `/home/tanyuan-cve/data/repos/linux-repos/linux-riscv-opt-rvv-memcpy-wrapper`
- Optimization commit under test: `554a11cb631d` (`riscv: add vector memcpy wrapper for large copies`)
- This report commit adds the profiling/test instrumentation used for the run. The guest log says `g554a11cb631d-dirty` because measurements were collected after adding that instrumentation and before creating this report commit.
- Build command: `PATH="$PWD/.codex-toolchain:$PATH" make -j$(nproc) ARCH=riscv CROSS_COMPILE=riscv64-linux-`
- Cross compiler: `riscv64-linux-gcc 13.3.0`, GNU ld 2.42, via local symlinks to Ubuntu `riscv64-linux-gnu-*`
- Performance config: `CONFIG_RISCV_ISA_V=y`, `CONFIG_PERF_EVENTS=y`, `CONFIG_TEST_RISCV_V_MEMCPY=m`, `CONFIG_MEMCPY_KUNIT_TEST=m`, `CONFIG_STRING_KUNIT_TEST=m`, `CONFIG_USERCOPY_KUNIT_TEST=m`, `CONFIG_KASAN=n`
- K3 host: `ssh -p 23322 froster@8.134.81.187`, Linux `6.18.3-generic`, QEMU `10.2.1`
- Board artifact directory: `/home/froster/yuan/rvv-memcpy-wrapper`
- QEMU shape: `taskset -c 2 qemu-system-riscv64 -accel kvm -machine virt -cpu host -smp 1 -m 2048M -nographic -no-reboot -bios none ...`
- Guest command lines:
  - Vector: `console=ttyS0 earlycon=sbi rdinit=/root/bin/rvv-run root=/dev/ram0 nokaslr panic=-1 riscv_v_memcpy_profile`
  - Scalar control: same plus `riscv_v_memcpy_disable`

The guest reports `riscv: base ISA extensions acdfimv` and `riscv: ELF capabilities acdfimv`, so RVV is exposed to the guest under `-cpu host`. The SBI PMU extension is also available. Runs use one guest CPU and are pinned to K3 host CPU 2.

## Correctness

KUnit passed in both vector-enabled and scalar-disabled boots:

| suite | result |
| --- | --- |
| `memcpy` | `pass:6 fail:0 skip:0 total:6` |
| `string` | `pass:20 fail:0 skip:0 total:20` |
| `usercopy` | `pass:4 fail:0 skip:0 total:4` |

The workload module also passed:

- Sizes below and above the 2048-byte threshold through 65536 bytes.
- Source and destination offsets `0,1,3,7,8,15`.
- Repeated correctness copies.
- Overlap fallback policy checks: vector-enabled run recorded `reason overlap 16 65536` for the dedicated overlap mode.
- IRQ-disabled fallback check: vector-enabled run recorded `reason disallowed 1 4096` for the dedicated IRQ mode.

## Direct memcpy benchmark

Representative direct `__memcpy()` throughput, 8000 iterations per case. Speedup is scalar-disabled time divided by vector-enabled time, so values above 1.0 favor the vector path.

| size | dst/src off | vector B/ns | scalar B/ns | speedup |
| ---: | --- | ---: | ---: | ---: |
| 2048 | 0/0 | 8.182 | 11.347 | 0.72x |
| 2048 | 0/1 | 8.332 | 1.261 | 6.61x |
| 2048 | 7/3 | 7.253 | 1.256 | 5.77x |
| 4096 | 0/0 | 11.567 | 13.262 | 0.87x |
| 4096 | 0/1 | 11.791 | 1.279 | 9.21x |
| 4096 | 7/3 | 10.491 | 1.284 | 8.17x |
| 8192 | 0/0 | 15.222 | 14.385 | 1.06x |
| 8192 | 0/1 | 15.315 | 1.294 | 11.83x |
| 8192 | 7/3 | 13.395 | 1.293 | 10.35x |
| 16384 | 0/0 | 18.115 | 16.181 | 1.12x |
| 16384 | 0/1 | 18.084 | 1.308 | 13.82x |
| 16384 | 7/3 | 15.729 | 1.303 | 12.07x |
| 32768 | 0/0 | 20.031 | 16.763 | 1.19x |
| 32768 | 0/1 | 19.947 | 1.305 | 15.28x |
| 32768 | 7/3 | 16.776 | 1.308 | 12.82x |
| 65536 | 0/0 | 18.279 | 15.659 | 1.17x |
| 65536 | 0/1 | 16.553 | 1.306 | 12.67x |
| 65536 | 7/3 | 16.672 | 1.307 | 12.75x |

Attribution for direct benchmark after counter reset:

| mode | total calls/bytes | vector calls/bytes | fallback-small calls/bytes | scalar-disabled calls/bytes |
| --- | ---: | ---: | ---: | ---: |
| vector boot | 403962 / 5509198339 | 321331 / 5385823808 | 82638 / 123374549 | 0 / 0 |
| scalar boot | 403995 / 5509199666 | 0 / 0 | 82671 / 123375876 | 321331 / 5385823808 |

Interpretation: aligned scalar remains better at 2-4 KiB, and vector only pulls ahead for aligned copies around 8 KiB and larger in this KVM setup. Misaligned large copies are where the wrapper wins strongly because the existing scalar copy falls back to byte-oriented behavior when source/destination low bits do not match.

## Workloads

The workload module exercised generic kernel `memcpy()` through synthetic mixed distributions and kernel helpers likely to represent large kernel-to-kernel copy patterns.

| workload | vector B/ns | scalar B/ns | speedup | vector calls/bytes | notes |
| --- | ---: | ---: | ---: | ---: | --- |
| mixed distribution | 3.668 | 1.292 | 2.84x | 91779 / 1523190128 | sizes from 64 to 65536 with varied offsets |
| scatterlist copy | 6.867 | 1.372 | 5.00x | 12979 / 53039728 | `sg_copy_from_buffer()` / `sg_copy_to_buffer()` |
| zstd compression buffers | 1.412 | 0.469 | 3.01x | 659 / 17049968 | `zstd_compress_cctx()` on 64 KiB buffers |
| zstd decompression buffers | 7.171 | 7.199 | 1.00x | included in zstd profile | no material change overall |
| xattr-style copies | 9.726 | 1.499 | 6.49x | 5779 / 93788528 | mixed metadata-value sizes through 64 KiB |
| sort records control | 0.308 | 0.311 | 0.99x | 179 / 706928 | broad workload with little useful vector copy share |

Attribution highlights:

- Mixed distribution: 91,779 vector calls for 1.52 GiB; 114,518 small fallback calls for 72.5 MiB.
- Scatterlist: 12,979 vector calls for 53.0 MiB; almost all useful bytes were vector-eligible 2-8 KiB chunks.
- Xattr-style: 5,779 vector calls for 93.8 MiB; small fallbacks were only 3.7 MiB.
- Sort control: only 706 KiB of vector-eligible incidental copies; end-to-end runtime was unchanged.

## Regression and policy findings

- Below-threshold copies are not vectorized. In the direct benchmark, 1024-byte and 2047-byte cases are essentially identical between vector and scalar boots.
- IRQ-disabled copies are not vectorized. Dedicated IRQ test passed and recorded one 4096-byte `disallowed` fallback in vector-enabled boot.
- Overlap is not vectorized. Dedicated overlap test recorded 16 overlap fallbacks for 65536 bytes, and the wrapper does not attempt to provide memmove semantics.
- Broad workloads with little vector-eligible copy volume do not improve. `sort_records` was 0.99x and should be treated as neutral.
- Aligned 2-4 KiB direct copies regress relative to scalar on this K3/KVM setup. A threshold of 8192 would avoid the aligned 2048/4096 regressions while retaining the large-copy and misaligned-copy wins, but it would lose the strong 2-4 KiB misaligned benefit.

## Recommendation

The wrapper is useful on K3/KVM when large copies are often unaligned or when helper workloads naturally produce 2 KiB+ non-overlapping kernel copies with mixed alignment. The current static 2048-byte threshold is too aggressive for aligned-only microbenchmarks on this setup. For a production-ready version, either raise the threshold to at least 8192, or add alignment-sensitive gating that keeps the 2048-byte path for misaligned source/destination pairs while using a higher threshold for 16-byte co-aligned copies.

## Caveats

- These are QEMU+KVM results on K3 with RVV exposed to the guest, not bare-metal kernel measurements.
- Profiling was enabled with `riscv_v_memcpy_profile`, adding atomic counter overhead to every profiled `__memcpy()` call. Scalar and vector runs used the same profiling mode, so comparisons are controlled but absolute throughput is not a no-instrumentation number. The sysfs profile is live rather than latched, so reading it also contributes a small amount of copy traffic to the printed profile totals.
- The workload module calls `__memcpy()` directly for controlled attribution; normal KASAN builds route many C `memcpy()` calls through `__asan_memcpy()` before reaching `__memcpy()`.
- KASAN was disabled for performance runs. Previous TCG smoke testing used KASAN and KUnit, but TCG RVV performance was not representative.
- Logs and generated artifacts were left under `/home/froster/yuan/rvv-memcpy-wrapper` on the board and `.k3-rvv-memcpy/` locally, but they are not committed.
