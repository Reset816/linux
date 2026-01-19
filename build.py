import subprocess
import os
import re


KERNEL_CONFIG_NOT_SET = 0


class KernelConfig:
    def __init__(self, kernel: str):
        self.file: str = os.path.join(kernel, '.config')
        self.m: dict[str, str] = {}

        if not os.path.exists(self.file):
            return

        with open(self.file, 'r') as f:
            lines = f.read().splitlines()
            for line in lines:
                if len(line) == 0:
                    continue
                if line.startswith('#'):
                    match = re.match(r'# (\S+) is not set', line)
                    if match:
                        self.m[match.group(1)] = KERNEL_CONFIG_NOT_SET
                else:
                    idx = line.index('=')  # There could be multiple '='
                    option = line[:idx]
                    value = line[idx + 1:]
                    self.m[option] = value

    def write_to_file(self):
        with open(self.file, 'w+') as f:
            for option, value in self.m.items():
                if value == KERNEL_CONFIG_NOT_SET:
                    line = f'# {option} is not set\n'
                else:
                    line = f'{option}={value}\n'
                f.write(line)

    def replace_value_substring(self, old: str, new: str) -> int:
        """
        Replace a substring in any option value (e.g., CONFIG_CMDLINE="... panic_on_warn=1 ...").
        Returns the number of options modified.
        """
        n = 0
        for option, value in list(self.m.items()):
            if value == KERNEL_CONFIG_NOT_SET:
                continue
            if not isinstance(value, str):
                continue
            if old in value:
                self.m[option] = value.replace(old, new)
                n += 1
        return n

    def set_y(self, option: str):
        self.m[option] = 'y'

    def set_m(self, option: str):
        self.m[option] = 'm'

    def not_set(self, option: str):
        self.m[option] = KERNEL_CONFIG_NOT_SET

    def set_y_or_not(self, option: str, b: bool):
        if b:
            self.set_y(option)
        else:
            self.not_set(option)


def download_syzbot_config(kernel: str):
    assert subprocess.run('wget "https://syzkaller.appspot.com/text?tag=KernelConfig&x=691a6769a86ac817" -O .config', cwd=kernel, shell=True).returncode == 0


def compile(kernel):
    subprocess.run('make clean && make distclean', cwd=kernel, shell=True)
    download_syzbot_config(kernel)

    config = KernelConfig(kernel)

    # Ensure panic_on_warn is disabled (commonly appears in CONFIG_CMDLINE).
    config.replace_value_substring('panic_on_warn=1', 'panic_on_warn=0')

    config.set_y('CONFIG_DEBUG_INFO')
    config.set_y('CONFIG_DEBUG_INFO_DWARF4')

    # You must enable KASAN
    config.set_y('CONFIG_KASAN')
    config.set_y('CONFIG_KASAN_OUTLINE')
    config.not_set('CONFIG_KASAN_INLINE')

    # You must disable KASLR
    config.not_set('CONFIG_RANDOMIZE_BASE')

    config.set_y('CONFIG_CONFIGFS_FS')
    config.set_y('CONFIG_SECURITYFS')

    # You must enable tracing
    config.set_y('CONFIG_TRACEPOINTS')
    config.set_y('CONFIG_TRACING')
    config.set_y('CONFIG_EVENT_TRACING')
    config.set_y('CONFIG_TRACE_EVENTS')
    config.set_y('CONFIG_TRACEFS')

    config.write_to_file()

    subprocess.run('LLVM=1 make olddefconfig CC=clang', cwd=kernel, shell=True)
    subprocess.run('LLVM=1 make -j$(nproc) CC=clang', cwd=kernel, shell=True)


if __name__ == '__main__':
    kernel = './'
    compile(kernel)
