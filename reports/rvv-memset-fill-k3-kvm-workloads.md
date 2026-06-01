# RVV memset fill K3 KVM workload report

Date: 2026-06-01

Worktree: `/home/tanyuan-cve/data/repos/linux-repos/linux-riscv-opt-rvv-memset-fill`

Branch: `riscv-opt/rvv-memset-fill`

Base: `39f90c1967215375f7d87b81d14b0f3ed6b40c29`

## Implementation

This change adds a conservative RVV path for large generic kernel `memset()`:

- `arch/riscv/lib/memset.S` keeps the existing scalar body as `__memset_scalar()` and adds a public `__memset()` wrapper when `CONFIG_RISCV_ISA_V=y`.
- The wrapper enters the vector path only for `count >= 16384` and only after the RISC-V alternatives framework confirms runtime `ZVE32X`.
- `arch/riscv/lib/memset_rvv.c` rejects vector use before `SYSTEM_RUNNING`, when `may_use_simd()` is false, and for zero fills when runtime Zicboz is present.
- `arch/riscv/lib/memset_vector.S` uses an `e8,m8` RVV fill loop with `vmv.v.x` and `vse8.v`.
- `__pi_memset` and `__pi___memset` stay scalar. Purgatory builds `memset.o` with `RISCV_MEMSET_STANDALONE`, so purgatory does not reference vector code.
- `lib/tests/memcpy_kunit.c` now has a large `memset()` KUnit case that checks return value, byte fill semantics, all destination alignments 0..63, nonzero/zero values, threshold-adjacent sizes, and page-boundary offsets.

The threshold was deliberately raised to 16 KiB. Earlier exploratory K3 KVM runs with lower thresholds found the 4 KiB edge was alignment-sensitive and not always faster. The final implementation keeps 4 KiB through 16383 bytes on the scalar path and uses RVV only where the measured margin is clearer.

## Build

Final local build commands:

```sh
PATH=/tmp/riscv64-linux-prefix/bin:$PATH \
make ARCH=riscv CROSS_COMPILE=riscv64-linux- -j64 vmlinux Image modules

PATH=/tmp/riscv64-linux-prefix/bin:$PATH \
make ARCH=riscv CROSS_COMPILE=riscv64-linux- \
  M=$PWD/tools/testing/riscv/rvv-memset-fill modules
```

Relevant final perf-image config:

- `CONFIG_RISCV_ISA_V=y`
- `CONFIG_RISCV_ISA_ZICBOZ=y`
- `CONFIG_KPROBES=y`
- `CONFIG_KPROBE_EVENTS=y`
- `CONFIG_PERF_EVENTS=y`
- `CONFIG_KUNIT=y`
- `CONFIG_MEMCPY_KUNIT_TEST=y`
- `# CONFIG_KASAN is not set`

The K3 performance image used KASAN off to avoid measuring sanitizer overhead. Earlier local correctness testing used KUnit/KASAN before the K3 performance pass; the final K3 guest booted with `kunit.enable=0 kunit.autorun=0` and logged `kunit: disabled`.

Object check after the final build:

```text
<__memset>:
  lui   a3,0x4
  bltu  a2,a3,__memset_scalar
  ALTERNATIVE fallback to __memset_scalar unless ZVE32X is present
  tail  enter_vector_memset
```

This confirms the final vector-entry boundary is exactly 16384 bytes.

## Board and QEMU

Board artifact directory:

```text
/home/froster/yuan/rvv-memset-fill
```

Final board artifact hashes:

```text
a35937d8d94c57876bc712cc108b3992cdaf45626a061af542d7be6bb2517b5d  Image
c197531a18a2d769aae7402932ff5c40ff80a571208c2c3535655154d0ca5e73  rootfs-rvv-memset-fill.cpio.gz
faf29c6383b742d3d0411ded352c52f33e46c762c9a1e53565c9979f44526723  rvv_memset_eval.ko
7e3753fc8975836cd3175c600e4108207edcd4aa513e7d0a86d225e54ea40267  guest-run.sh
fa2cdedce0bf4b20a64c19735385253abb46ccea478099be6456c89235bdfcef  run-k3.sh
```

Final log:

```text
/home/froster/yuan/rvv-memset-fill/run-k3-rvv-memset-fill.log
local parsed copy: /tmp/rvv-memset-fill-k3-final-th16384-full.log
sha256: 1b8282836a36d06866cc5e0f3e287b75151fb3086201fa94a94e52a37fa34293
```

The final eval was bound only to the assigned Spacemit K3 X100 CPU3:

```sh
taskset -c 3 qemu-system-riscv64 \
  -accel kvm \
  -machine virt \
  -cpu host \
  -smp 1 \
  -m 1024M \
  -nographic \
  -no-reboot \
  -bios none \
  -kernel Image \
  -initrd rootfs-rvv-memset-fill.cpio.gz \
  -append "console=ttyS0 root=/dev/ram rdinit=/init loglevel=7 nokaslr kunit.enable=0 kunit.autorun=0"
```

The host CPU check showed CPU3 online:

```text
CPU CORE SOCKET NODE ONLINE
  3    3      0    -    yes
```

CPU3 cpufreq was `userspace` at `2200000`; cpufreq was not changed.

Important constraint: no A100 harts were used for evaluation. The final run was KVM on X100 CPU3 only. CPU8-15 were not used for the eval.

## Dedicated correctness

The dedicated correctness and benchmark pass completed successfully:

```text
status: PASS
failures: 0
correct_checks: 15732
irqoff_calls: 69882
irqoff_bytes: 601661952
```

Coverage included:

- Values: `0x00`, `0x01`, `0x5a`, `0x80`, `0xff`, `0x1234`.
- Destination alignments: offsets `0..63`.
- Page-boundary offsets: `4048`, `4063`, `4064`, `4065`, `4080`.
- Sizes around scalar/vector boundaries: `4095`, `4096`, `4097`, `8191`, `8192`, `8193`, `16383`, `16384`, `16385`, plus `65536`.
- Normal context and IRQ-disabled context.
- Return value and byte guards before and after the destination range.

The guest logged `rvv_memset_eval: unloaded` and `reboot: Power down`; no crash, warning, KASAN report, or failed status appeared in the final log.

## Dedicated throughput

Numbers below are unprobed dedicated benchmark medians across offsets `0`, `1`, `31`, and `63`. `normal` is the normal direct `__memset()` call. `irqoff` is the same call under local IRQ disable, which forces `may_use_simd()` false and is used as a scalar fallback/control.

Nonzero fill, value `0x5a`:

| size | expected normal path | normal MiB/s | irqoff MiB/s | delta |
|---:|---|---:|---:|---:|
| 4096 | scalar | 14694.5 | 13424.0 | +9.5% |
| 8192 | scalar | 15078.4 | 14234.7 | +5.9% |
| 16383 | scalar | 14836.6 | 14596.2 | +1.6% |
| 16384 | RVV | 19071.8 | 14376.9 | +32.7% |
| 16385 | RVV | 19594.5 | 14568.3 | +34.5% |
| 65536 | RVV | 22265.6 | 14948.5 | +48.9% |

Per-alignment nonzero deltas were positive at the final vector boundary:

| size | offset 0 | offset 1 | offset 31 | offset 63 |
|---:|---:|---:|---:|---:|
| 16384 | +28.2% | +41.9% | +33.6% | +31.6% |
| 16385 | +41.1% | +33.2% | +31.6% | +29.8% |
| 65536 | +44.5% | +51.9% | +52.9% | +45.0% |

Zero fill:

| size | expected normal path | normal MiB/s | irqoff MiB/s | delta |
|---:|---|---:|---:|---:|
| 16384 | scalar fallback | 14730.5 | 14442.5 | +2.0% |
| 16385 | scalar fallback | 14508.0 | 14075.1 | +3.1% |
| 65536 | scalar fallback | 14972.5 | 14989.4 | -0.1% |

Zero fill does not use the RVV asm on this Zicboz-capable guest. That keeps generic zero memset from competing with the zeroing policy and with the existing Zicboz-oriented zero-page machinery.

## Attribution

The attribution pass used kprobes on `__memset`, `enter_vector_memset`, `__asm_rvv_memset`, and `__memset_scalar`. Timing from this section includes kprobe overhead and is not used as the primary throughput result.

Direct attribution summary:

```text
status: PASS
failures: 0
correct_checks: 192
summary large_candidates=3108 vector_calls=777 vector_bytes=25460995 fallback_calls=2331 small_scalar_calls=26 zero_zicboz_scalar_calls=7790 irqoff_calls=7776
probe __memset calls=15578 bytes=165523956 zero_calls=7802 zero_bytes=82766830 nonzero_calls=7776 nonzero_bytes=82757126
probe enter_vector_memset calls=3108 bytes=101843980 zero_calls=1554 zero_bytes=50921990 nonzero_calls=1554 nonzero_bytes=50921990
probe __asm_rvv_memset calls=777 bytes=25460995 zero_calls=0 zero_bytes=0 nonzero_calls=777 nonzero_bytes=25460995
probe __memset_scalar calls=14789 bytes=140056817 zero_calls=7790 zero_bytes=82760686 nonzero_calls=6999 nonzero_bytes=57296131
```

Boundary buckets:

```text
probe_bucket __memset 4096_16383 calls=6216 bytes=50921472
probe_bucket enter_vector_memset 4096_16383 calls=0 bytes=0
probe_bucket __asm_rvv_memset 4096_16383 calls=0 bytes=0
probe_bucket __memset_scalar 4096_16383 calls=6216 bytes=50921472

probe_bucket __memset ge16384 calls=3108 bytes=101843980
probe_bucket enter_vector_memset ge16384 calls=3108 bytes=101843980
probe_bucket __asm_rvv_memset ge16384 calls=777 bytes=25460995
probe_bucket __memset_scalar ge16384 calls=2331 bytes=76382985
```

Interpretation:

- Calls below 16 KiB did not enter the vector wrapper.
- Calls at or above 16 KiB entered `enter_vector_memset()`.
- Only nonzero, normal-context, at-or-above-threshold calls reached `__asm_rvv_memset()`.
- Zero fills at or above threshold fell back to `__memset_scalar()` because Zicboz was present.
- IRQ-disabled nonzero fills at or above threshold fell back to `__memset_scalar()` because `may_use_simd()` was false.

## Workloads

### Allocation/init-heavy control

The allocation workload is a synthetic control that repeatedly allocates a 16 KiB object and directly calls generic `__memset()` through the module. It is useful for attribution, but it should not be described as a naturally occurring broad kernel workload.

```text
status: PASS
summary large_candidates=12000 vector_calls=6000 vector_bytes=98304000 fallback_calls=6000 small_scalar_calls=18 zero_zicboz_scalar_calls=6006 irqoff_calls=0
row kind=workload mode=alloc value=0x5a size=16384 iterations=6000 bytes=98304000 ns=14615500 mib_per_sec_x100=641442
row kind=workload mode=alloc value=0x00 size=16384 iterations=6000 bytes=98304000 ns=15402542 mib_per_sec_x100=608665
probe __asm_rvv_memset calls=6000 bytes=98304000 zero_calls=0 nonzero_calls=6000
probe __memset_scalar calls=6006 bytes=98305392 zero_calls=6006 nonzero_calls=0
```

This confirms the intended split under a larger direct workload: nonzero 16 KiB fills use RVV; zero 16 KiB fills fall back scalar on the Zicboz-capable guest.

### tmpfs xattr workload

The tmpfs xattr workload exercises a more realistic filesystem metadata/xattr path with 2500 operations and a 2048-byte xattr value. It mostly did not hit generic large `__memset()`:

```text
status: PASS
summary large_candidates=0 vector_calls=0 vector_bytes=0 fallback_calls=0 small_scalar_calls=18 zero_zicboz_scalar_calls=6 irqoff_calls=0
row kind=workload mode=xattr value=0x5a size=2048 iterations=2500 bytes=5120000 ns=3627417 mib_per_sec_x100=134608
probe __memset calls=18 bytes=7184 zero_calls=18 zero_bytes=7184 nonzero_calls=0 nonzero_bytes=0
probe enter_vector_memset calls=0 bytes=0
probe __asm_rvv_memset calls=0 bytes=0
```

This is an important negative result. Several broad kernel paths use `clear_page`, specialized zeroing, copies from userspace, compiler inlining, or subsystem-specific initialization instead of large generic `__memset()`. In this xattr run, the generic memset activity was small zeroing only, so the RVV memset candidate did not affect it.

## Regression and fallback analysis

- Small and medium fills are protected by the 16 KiB threshold. Attribution showed `4096_16383` calls went to scalar only.
- Zero fills on the K3 guest did not reach RVV asm. The direct pass recorded `__asm_rvv_memset zero_calls=0`.
- IRQ-disabled calls fell back cleanly through `may_use_simd()`. The direct pass recorded `irqoff_calls=7776`; nonzero IRQ-off large fills entered the wrapper but did not reach `__asm_rvv_memset()`.
- The final 16 KiB boundary was chosen because earlier 4 KiB-threshold exploration showed mixed results around 4097 bytes. Keeping the threshold at 16 KiB avoids that alignment-sensitive edge.
- Nonzero 16 KiB and 64 KiB direct fills improved strongly versus the IRQ-off scalar control in the unprobed dedicated pass.
- Zero 64 KiB median throughput was effectively unchanged versus the scalar control, with no RVV use.

## Residual risks

- The final performance data is from one Spacemit K3 X100 core under QEMU KVM, not bare metal.
- No A100 data was collected or used. CPU8-15 were not used.
- Kprobe attribution changes timing, so only the dedicated unprobed benchmark should be used for throughput claims.
- The broader workload evidence is sparse because realistic kernel paths did not naturally hit large generic `__memset()` in the xattr workload.
- A no-vector runtime boot was not executed on the board because the evaluation was constrained to `-cpu host` on the assigned RVV-capable X100 CPU3. The runtime alternative fallback was source and object verified.
- The threshold may need retuning on other RVV implementations, VLENs, or bare-metal configurations.
