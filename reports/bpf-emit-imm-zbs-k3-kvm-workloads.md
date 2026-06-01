# RISC-V BPF `emit_imm()` Zbs Evaluation on K3 QEMU/KVM

## Summary

This evaluates the RISC-V BPF JIT change that emits Zbs instructions for profitable 64-bit immediates:

- `1ULL << n` -> `bseti rd, zero, n` when the value is not already a 12-bit signed immediate.
- `~(1ULL << n)` -> `li -1; bclri rd, rd, n` when the value is not already a 32-bit signed immediate.

The evaluation used K3 QEMU/KVM on board CPU 5 only. The guest CPU exposed `zbs`, and the control kernel used the same guest CPU but had `CONFIG_RISCV_ISA_ZBS=n` so `rvzbs_enabled()` stayed false.

## Setup

Board artifact directory:

```text
/home/froster/yuan/bpf-emit-imm-zbs
```

Board CPU/frequency:

```text
/sys/devices/system/cpu/cpu5/cpufreq/scaling_governor: userspace
/sys/devices/system/cpu/cpu5/cpufreq/scaling_cur_freq: 2200000
```

QEMU invocation shape:

```sh
taskset -c 5 qemu-system-riscv64 \
  -accel kvm -machine virt -cpu host -smp 1 -m 1024M \
  -nographic -no-reboot -bios none \
  -kernel /home/froster/yuan/bpf-emit-imm-zbs/artifacts/Image-zbs \
  -initrd /home/froster/yuan/bpf-emit-imm-zbs/artifacts/rootfs-eval.cpio.gz \
  -append 'console=ttyS0 root=/dev/ram rdinit=/init_eval loglevel=4' \
  -serial mon:stdio
```

Artifacts and logs:

```text
artifacts/Image-zbs
artifacts/Image-nozbs
artifacts/config-zbs
artifacts/config-nozbs
artifacts/rootfs-eval.cpio.gz
artifacts/riscv_bpf_emit_imm_zbs_eval
logs/zbs-harden-fixed.log
logs/nozbs.log
```

Guest confirmation:

```text
zbs kernel:   CONFIG_RISCV_ISA_ZBS=y, guest isa includes zbs
control:      # CONFIG_RISCV_ISA_ZBS is not set, guest isa still includes zbs
bpf_jit_enable=1, bpf_jit_harden=0 for main timing runs
```

## Workloads

The self-contained test program is `tools/testing/selftests/bpf/riscv_bpf_emit_imm_zbs_eval.c`. It loads raw socket-filter BPF programs with `bpf(BPF_PROG_LOAD)`, reads the JIT image with `BPF_OBJ_GET_INFO_BY_FD`, counts RISC-V instructions including compressed encodings, counts `bseti`/`bclri`, and times `BPF_PROG_TEST_RUN` repeat loops.

Workload modes:

- `dedicated`: repeated exact candidate constants: bit 63, bit 11, inverse bit 63, inverse bit 40.
- `general`: realistic-ish mixed constants: two eligible single-bit constants and two non-eligible ordinary large constants per repetition.
- `control`: small immediates plus ordinary non-single-bit constants; expected no Zbs use.
- `alu32`: bit 31 and an ALU32 immediate path to check high-bit/sign-extension behavior.

Each main workload used 32 repetitions inside one BPF program and 9 timing rounds of 200,000 `BPF_PROG_TEST_RUN` repeats. Timings below are medians of `ns_per_run`.

## Results

| mode | kernel | JIT insns | JIT bytes | bseti | bclri | median ns/run | retval |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| dedicated | Zbs | 491 | 1310 | 64 | 64 | 119.024 | 128 |
| dedicated | no-Zbs | 619 | 1374 | 0 | 0 | 129.359 | 128 |
| general | Zbs | 651 | 1822 | 64 | 0 | 137.589 | 128 |
| general | no-Zbs | 715 | 1822 | 0 | 0 | 157.260 | 128 |
| control | Zbs | 779 | 2270 | 0 | 0 | 182.112 | 65728 |
| control | no-Zbs | 779 | 2270 | 0 | 0 | 182.228 | 65728 |
| alu32 | Zbs | 24 | 68 | 1 | 1 | 40.377 | 3 |
| alu32 | no-Zbs | 26 | 68 | 0 | 0 | 41.186 | 3 |

Deltas, comparing no-Zbs control to Zbs:

| mode | JIT insn reduction | byte reduction | timing delta | timing improvement |
| --- | ---: | ---: | ---: | ---: |
| dedicated | 128 | 64 | 10.335 ns/run | 7.99% |
| general | 64 | 0 | 19.671 ns/run | 12.51% |
| control | 0 | 0 | 0.116 ns/run | 0.06% |
| alu32 | 2 | 0 | 0.809 ns/run | 1.96% |

Attribution:

- Dedicated mode had 128 eligible constants: 64 single-bit constants became `bseti`, and 64 inverse single-zero masks became `bclri` after `li -1`.
- General mode had 64 eligible single-bit constants; the ordinary large constants did not produce Zbs instructions.
- Control mode had no eligible constants and produced no JIT size change.
- ALU32 mode returned the same value (`3`) with one `bseti` and one `bclri`; the explicit `BPF_ALU | BPF_MOV | BPF_K` path for `0x80000000` was not changed by this optimization.

The general workload has unchanged byte length despite fewer decoded instructions because the fallback sequence uses compressed instructions in places where Zbs uses 32-bit instructions. The decoded instruction count still drops by 64 and the timing improved in this microbenchmark.

## Regression Checks

- 12-bit immediates: control mode uses `7` and `2047`; no Zbs instructions are emitted and the Zbs/no-Zbs JIT size is identical.
- Non-single-bit constants: control mode uses `0x123456789abcdef0` and `0x3333333333333333`; no Zbs instructions are emitted.
- bit 63: dedicated mode uses `0x8000000000000000` and `0x7fffffffffffffff`; retval is unchanged and the expected `bseti`/`bclri` counts appear.
- bit 31 / ALU32: `alu32` mode returns `3` on both kernels. The 64-bit bit-31 constant can use Zbs; the ALU32 immediate instruction path still behaves correctly.
- Runtime gating: with the same `-cpu host` exposing `zbs`, the `CONFIG_RISCV_ISA_ZBS=n` kernel emits zero `bseti`/`bclri` instructions.
- Constant blinding/hardening: a quick dedicated run with `bpf_jit_harden=0` and `bpf_jit_harden=1` on the Zbs kernel both reported 16 `bseti` and 16 `bclri` for 8 repetitions and unchanged JIT size. In this root-run setup, hardening did not hide the returned JIT image or blind these constants.

## Caveats

- These are microbenchmarks designed to force `emit_imm()` candidates and isolate attribution. The general mode is mixed but still synthetic.
- I did not find a natural broad workload in this rootfs that frequently materializes large single-bit or inverse single-zero constants through the socket-filter JIT. The report therefore documents opcode incidence and a mixed synthetic workload instead.
- Timings are `BPF_PROG_TEST_RUN` wall-clock medians inside a KVM guest pinned to board CPU 5, not hardware counter cycles. `perf` exists on the board, but the guest tests were kept self-contained and short to avoid expanding scope.
