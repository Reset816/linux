# RISC-V RVV copy_page() K3/KVM evaluation

Date: 2026-06-01

Branch: `riscv-opt/rvv-copy-page`

Board: Spacemit K3, accessed with `ssh -p 23322 froster@8.134.81.187`

Board artifact directory: `/home/froster/yuan/rvv-copy-page`

## Summary

The K3 QEMU+KVM guest exposes RVV to the kernel (`rv64imafdcv...`, VLEN 256). The RVV `copy_page()` implementation is correct, and the forced-vector mode engages the vector path for all measured production `copy_page()` calls.

However, the boot-time profitability gate keeps the scalar path on this K3/KVM setup:

```
riscv: copy_page: keeping scalar copy, vlen=256, vector=136458ns, scalar=124208ns per 512 pages
```

That is the right production decision for this machine: forced vector is slower in the direct benchmark and slightly slower in the page-copy-heavy workloads. The gate prevents those regressions while still leaving a vector path available on systems where the boot probe finds it profitable.

## Build And Artifacts

Local build command:

```
PATH="$PWD/.tmp-toolchain:$PATH" make -j64 ARCH=riscv CROSS_COMPILE=riscv64-linux- Image modules
PATH="$PWD/.tmp-toolchain:$PATH" make ARCH=riscv CROSS_COMPILE=riscv64-linux- \
  -C tools/testing/selftests/riscv/copy_page \
  OUTPUT="$PWD/tools/testing/selftests/riscv/copy_page"
```

The local toolchain shim maps `riscv64-linux-*` to the installed `riscv64-linux-gnu-*` tools, while preserving the requested `CROSS_COMPILE=riscv64-linux-` make interface.

Relevant config included `CONFIG_RISCV_ISA_V=y`, `CONFIG_PERF_EVENTS=y`, `CONFIG_KPROBES=y`, `CONFIG_KPROBE_EVENTS=y`, `CONFIG_FTRACE=y`, `CONFIG_DEBUG_FS=y`, `CONFIG_TMPFS=y`, `CONFIG_COMPACTION=y`, `CONFIG_MIGRATION=y`, and `CONFIG_RISCV_COPY_PAGE_TEST=m`.

Uploaded artifacts:

| File | SHA-256 |
| --- | --- |
| `Image` | `d8e395916dbcf06063f61656e96272c5e317424a424862c7133d3a1546d5494b` |
| `rvv-copy-page-initramfs.cpio.gz` | `d89f332a1d6d7328f6f424382faac29a7d155ce06f35a944f1b49b72fd879984` |
| `test_copy_page.ko` | `21d467e07f63f90f36c81cf7668aa5be0d585ef1208e49eacdbabf2f38f7025e` |
| `copy_page_debugfs` | `9956852f048b9e1241a6b29a54c09be84fc8ae95adddf80ec3379a103b116c79` |
| `copy_page_workloads` | `82751741249121e9b5831bcf7f662d8bf315bbcf76ea06f42f071c455905f23e` |

## QEMU Command

Each run used the same manual KVM command shape, pinned to host CPU 2 and with no firmware:

```
taskset -c 2 timeout 240 qemu-system-riscv64 \
  -accel kvm -machine virt -cpu host -smp 1 -m 2048M \
  -nographic -no-reboot -bios none \
  -kernel Image \
  -initrd rvv-copy-page-initramfs.cpio.gz \
  -append "console=ttyS0 earlycon rdinit=/test-init nokaslr riscv.copy_page=<auto|scalar|vector>"
```

Console logs are preserved on the board as `k3-auto.log`, `k3-scalar.log`, and `k3-vector.log`.

## Dedicated Correctness And Direct Benchmark

The debugfs test module validates:

- `copy_page()` data integrity and guard bytes.
- `copy_user_page()` data integrity through the RISC-V `copy_page()` wrapper.
- Direct RVV helper correctness when vector use is legal.
- Fallback when SIMD is disallowed by disabling IRQs. With `CONFIG_RISCV_ISA_V_PREEMPTIVE=n`, `irq_disabled_vector_eligible` stayed `0`.

All runs reported `status: ok`, `copy_user_page_ok: 1`, `direct_vector_legal: 1`, and `irq_disabled_vector_eligible: 0`.

200,000-iteration direct benchmark:

| Policy | Boot probe | `copy_page` ns/page | `memcpy` ns/page | direct RVV ns/page | `copy_page` calls | vector calls |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `auto` | scalar kept, vector 136458 ns vs scalar 124208 ns per 512 pages | 272 | 243 | 274 | 200000 | 0 |
| `scalar` | forced scalar, vector 136458 ns vs scalar 124416 ns per 512 pages | 274 | 238 | 274 | 200000 | 0 |
| `vector` | forced vector, vector 136417 ns vs scalar 123709 ns per 512 pages | 334 | 243 | 278 | 200000 | 200000 |

The direct benchmark shows the cost of forcing the production vector path on this setup: `copy_page()` rises from 274 ns/page in forced-scalar mode to 334 ns/page in forced-vector mode, about a 21.9% slowdown. The direct RVV helper itself is around 278 ns/page, but production vector `copy_page()` also pays the kernel vector begin/end cost.

## Broader Workloads And Attribution

The workload helper resets the test-module counters before each workload and reads `copy_page` totals afterward.

Workload definitions:

- `anon-cow`: allocate and fault anonymous memory, fork, and dirty all pages in the child.
- `tmpfs-private`: populate a tmpfs file, map it `MAP_PRIVATE`, and dirty all pages.
- `fork-exec`: fork children that immediately exec `/bin/true`, expected to have much lower page-copy density.
- `cpu-control`: CPU-only arithmetic loop, expected not to call `copy_page()`.

Results for 4096 pages and 16 loops:

| Workload | Auto elapsed ms | Scalar elapsed ms | Vector elapsed ms | Auto calls/vector | Scalar calls/vector | Vector calls/vector |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `anon-cow` | 246.312 | 245.512 | 248.157 | 65616 / 0 | 65616 / 0 | 65632 / 65632 |
| `tmpfs-private` | 213.612 | 213.509 | 217.391 | 65536 / 0 | 65536 / 0 | 65536 / 65536 |
| `fork-exec` | 28.151 | 28.185 | 28.208 | 304 / 0 | 304 / 0 | 320 / 320 |
| `cpu-control` | 168.743 | 168.806 | 168.682 | 0 / 0 | 0 / 0 | 0 / 0 |

Attribution is clear:

- The anonymous COW and tmpfs private-fault workloads naturally call `copy_page()` about 65k times each.
- Forced-vector mode engages vector for those calls.
- Auto and forced-scalar mode use zero vector calls.
- The CPU-only control calls `copy_page()` zero times, so it is not expected to benefit.

Relative to forced scalar, forced vector regressed:

- `anon-cow`: +1.1% elapsed time.
- `tmpfs-private`: +1.8% elapsed time.
- `fork-exec`: +0.1%, effectively noise.
- `cpu-control`: -0.1%, noise and no page-copy attribution.

## Regression Analysis

On K3/KVM with VLEN 256, enabling RVV page copy unconditionally is not profitable. The direct benchmark and realistic copy-heavy workloads both agree with the boot probe.

The production `auto` policy is safe on this system because it leaves the static key disabled. It behaves like forced scalar for broad workloads and avoids the forced-vector slowdown.

## Caveats

Only one K3/KVM run per policy was captured. Runs were pinned to host CPU 2 with a single guest CPU (`-smp 1`) to reduce scheduling noise, but these are still virtualized results rather than bare-metal kernel timings.

The instrumentation counters are built only under `CONFIG_RISCV_COPY_PAGE_TEST=m`; they are for attribution and are not part of a normal production config.

The compaction/migration path was not separately isolated in this initramfs run. The evaluated broad workloads cover anonymous COW, tmpfs/page-cache private dirty faults, fork/exec, and a no-copy CPU control.
