# RISC-V BPF JIT Zbs K3/KVM Workload Evaluation

## Summary

This evaluates the RISC-V BPF JIT Zbs optimization on a K3 board using
QEMU/KVM with the guest CPU exposing `zbs`. The kernel candidate emits Zbs
instructions for non-12-bit single-bit immediate operations:

- `OR imm`: `bseti`
- `XOR imm`: `binvi`
- `AND imm` clear-one-bit masks: `bclri`
- `JSET imm`: `bexti`

The no-Zbs control kernel was built from the same tree and config shape with
`CONFIG_RISCV_ISA_ZBS=n`. Both kernels ran the same syscall-only socket-filter
BPF workload in a tiny initramfs.

## Environment

- Board: K3, QEMU/KVM on host CPU 4 only
- Board artifact directory: `/home/froster/yuan/bpf-jit-zbs`
- QEMU command shape:
  `taskset -c 4 qemu-system-riscv64 -accel kvm -machine virt -cpu host -smp 1 -m 512M -nographic -no-reboot -bios none -kernel Image.{zbs,nozbs} -initrd rootfs.bpfzbs.cpio.gz -append "console=ttyS0 earlycon=sbi root=/dev/ram rdinit=/init panic=-1 oops=panic"`
- Guest ISA included `zbs`:
  `rv64imafdcv_..._zba_zbb_zbc_zbs_...`
- JIT dump mode: `/proc/sys/net/core/bpf_jit_enable=2`
- Main timing mode: `/proc/sys/net/core/bpf_jit_harden=0`
- Hardening smoke mode: `/proc/sys/net/core/bpf_jit_harden=2`
- Repeat count: 2,000,000 `BPF_PROG_TEST_RUN` executions per program

## Build and Artifacts

Local build used a temporary wrapper prefix so the requested command form used
`CROSS_COMPILE=riscv64-linux-` while invoking the installed
`riscv64-linux-gnu-` toolchain.

Commands:

```sh
make ARCH=riscv CROSS_COMPILE=riscv64-linux- defconfig
./scripts/config --enable BPF --enable BPF_SYSCALL --enable BPF_JIT \
  --enable BPF_JIT_DEFAULT_ON --enable BPF_EVENTS --enable PERF_EVENTS \
  --enable NET --enable INET --enable PACKET --enable UNIX --enable PROC_FS \
  --enable SYSCTL --enable IKCONFIG --enable IKCONFIG_PROC \
  --enable RISCV_ISA_ZBS --enable RISCV_ISA_ZBA --enable RISCV_ISA_ZBB \
  --enable RISCV_ALTERNATIVE --disable BPF_JIT_ALWAYS_ON --disable DEBUG_INFO
make ARCH=riscv CROSS_COMPILE=riscv64-linux- olddefconfig
make ARCH=riscv CROSS_COMPILE=riscv64-linux- -j64 Image

./scripts/config --disable RISCV_ISA_ZBS
make ARCH=riscv CROSS_COMPILE=riscv64-linux- olddefconfig
make ARCH=riscv CROSS_COMPILE=riscv64-linux- -j64 Image

riscv64-linux-gnu-gcc -static -O2 -Wall -Wextra \
  -Itools/include/uapi -Itools/include \
  tools/testing/selftests/bpf/bpf_jit_zbs_k3.c -o bpf_jit_zbs_k3
```

Uploaded artifacts:

- `/home/froster/yuan/bpf-jit-zbs/Image.zbs`
- `/home/froster/yuan/bpf-jit-zbs/Image.nozbs`
- `/home/froster/yuan/bpf-jit-zbs/rootfs.bpfzbs.cpio.gz`
- `/home/froster/yuan/bpf-jit-zbs/bpf_jit_zbs_k3`
- `/home/froster/yuan/bpf-jit-zbs/config.zbs`
- `/home/froster/yuan/bpf-jit-zbs/config.nozbs`
- `/home/froster/yuan/bpf-jit-zbs/qemu-zbs.log`
- `/home/froster/yuan/bpf-jit-zbs/qemu-nozbs.log`
- `/home/froster/yuan/bpf-jit-zbs/qemu-zbs-harden.log`

## Dedicated Results

| Program | Purpose | Zbs emitted | JIT bytes Zbs/control | Static JIT insns saved | Zbs avg ns | Control avg ns |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `dedicated_alu64_zbs` | ALU64 `OR/XOR/AND imm` | 3 | 44 / 60 | 5 | 29 | 29 |
| `dedicated_jset_zbs` | ALU64 `JSET imm` | 1 | 58 / 64 | 2 | 55 | 53 |
| `dedicated_alu32_bit31` | ALU32 bit31 `OR/XOR/AND/JSET` | 4 | 72 / 98 | 6 | 54 | 55 |

All programs returned the expected value. The timing differences are small at
this syscall/test-run granularity; the result is primarily an instruction-count
and emitted-code-size win.

## General Workload

`broad_packet_flags` models packet/metadata flag manipulation: two single-bit
sets, one single-bit test, one single-bit toggle, and one clear-one-bit mask.

| Program | Zbs emitted | JIT bytes Zbs/control | Static JIT insns saved | Zbs avg ns | Control avg ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| `broad_packet_flags` | 5 | 74 / 98 | 7 | 56 | 55 |

No natural in-tree broad workload was found and run in this tiny rootfs. The
representative flag workload shows the expected code-size reduction but no
stable runtime win at the measured granularity.

## Controls and Regression Checks

| Program | Purpose | Zbs emitted | Zbs avg ns | Control avg ns |
| --- | --- | ---: | ---: | ---: |
| `control_12bit_masks` | 12-bit immediates should keep existing one-insn fallback | 0 | 57 | 55 |
| `control_multi_bit_masks` | non-single-bit masks should keep fallback | 0 | 54 | 54 |
| hardening smoke | `bpf_jit_harden=2`, constant blinding enabled | 0 in decoded immediate forms | all returned expected values | not run |

The hardening smoke pass on the Zbs kernel passed every program with
`bpf_jit_harden=2`. JIT lengths increased substantially from constant blinding
and no direct single-bit immediate Zbs encodings were found in those dumps,
which is expected because blinded immediates are rewritten before JIT emission.

## JIT Dump Attribution

Assembler reference encodings:

```text
28b79793 bseti a5,a5,0xb
28c79793 bseti a5,a5,0xc
68b79793 binvi a5,a5,0xb
68c79793 binvi a5,a5,0xc
48b79793 bclri a5,a5,0xb
48c79793 bclri a5,a5,0xc
48b7d313 bexti t1,a5,0xb
48c7d313 bexti t1,a5,0xc
```

Decoded from `qemu-zbs.log`:

| Block | Program | Matched Zbs encodings |
| ---: | --- | --- |
| 0 | `dedicated_alu64_zbs` | `bseti bit11`, `binvi bit12`, `bclri bit11` |
| 1 | `dedicated_jset_zbs` | `bexti bit11` |
| 2 | `dedicated_alu32_bit31` | `bseti bit31`, `binvi bit31`, `bclri bit31`, `bexti bit31` |
| 3 | `control_12bit_masks` | none |
| 4 | `control_multi_bit_masks` | none |
| 5 | `broad_packet_flags` | `bseti bit11`, `bseti bit12`, `bexti bit13`, `binvi bit11`, `bclri bit12` |

Decoded from `qemu-nozbs.log`: no Zbs encodings matched in any block.

## Caveats

- Runtime results are single-pass QEMU/KVM loop timings. They are useful as a
  smoke/performance sanity check, but instruction-count and JIT-dump evidence is
  the stronger signal here.
- `perf` was not used inside the tiny initramfs; `BPF_PROG_TEST_RUN` average
  nanoseconds and wall-clock loop timing were used instead.
- The broad workload is synthetic but realistic for flag operations. It is not a
  full XDP/tc/cgroup datapath benchmark.
- The no-Zbs guest still reports host `zbs` in `/proc/cpuinfo`; the control is
  kernel-side gating via `CONFIG_RISCV_ISA_ZBS=n`, confirmed by the absence of
  Zbs encodings in JIT dumps.
