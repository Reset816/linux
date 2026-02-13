import os
import subprocess


def build_llvm_pass():
    if not os.path.exists('instr/build'):
        assert subprocess.run('cd instr && mkdir build && cd build && cmake .. && make -j$(nproc)', shell=True).returncode == 0


def download_syzbot_config(kernel: str):
    # 6.8.1
    url = 'https://syzkaller.appspot.com/text?tag=KernelConfig&x=691a6769a86ac817'

    assert subprocess.run(f'wget "{url}" -O .config', cwd=kernel, shell=True).returncode == 0


def compile(kernel):
    subprocess.run('make clean && make distclean', cwd=kernel, shell=True)
    download_syzbot_config(kernel)

    subprocess.run('scripts/config -e DEBUG_INFO', cwd=kernel, shell=True)
    subprocess.run('scripts/config -e DEBUG_INFO_DWARF4', cwd=kernel, shell=True)

    # You must enable KASAN
    subprocess.run('scripts/config -e KASAN', cwd=kernel, shell=True)
    subprocess.run('scripts/config -e KASAN_OUTLINE', cwd=kernel, shell=True)
    subprocess.run('scripts/config -d KASAN_INLINE', cwd=kernel, shell=True)

    # You must disable KASLR
    subprocess.run('scripts/config -d RANDOMIZE_BASE', cwd=kernel, shell=True)

    subprocess.run('scripts/config -e CONFIGFS_FS', cwd=kernel, shell=True)
    subprocess.run('scripts/config -e SECURITYFS', cwd=kernel, shell=True)

    # You must enable tracing
    subprocess.run('scripts/config -e TRACEPOINTS', cwd=kernel, shell=True)
    subprocess.run('scripts/config -e TRACING', cwd=kernel, shell=True)
    subprocess.run('scripts/config -e EVENT_TRACING', cwd=kernel, shell=True)

    subprocess.run("sed -i 's/panic_on_warn=1/panic_on_warn=0/g' .config", cwd=kernel, shell=True)

    subprocess.run('LLVM=1 make olddefconfig CC=clang', cwd=kernel, shell=True)
    pass_so = 'instr/build/libICallPass.so'
    kcflags = [
        f'-fpass-plugin={pass_so}',  # our LLVM pass
    ]
    kcflags_str = ' '.join(kcflags)
    subprocess.run(f"LLVM=1 make -j$(nproc) CC=clang KCFLAGS='{kcflags_str}'", cwd=kernel, shell=True)


if __name__ == '__main__':
    build_llvm_pass()
    compile(os.getcwd())

    '''
    boot the kernel with:

    trace_event=myinst:rw,myinst:ic
    trace_buf_size=64M
    '''
