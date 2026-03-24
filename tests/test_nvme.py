#!/usr/bin/env python3
"""test_nvme.py — verify NVMe driver initializes on q35 with NVMe disk."""

import subprocess, sys, os, tempfile, re

BOOT_TIMEOUT = int(os.environ.get('BOOT_TIMEOUT', '900'))
BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build')
ISO   = os.path.join(BUILD, 'aegis.iso')


def strip_ansi(s):
    return re.sub(r'\x1b\[[?0-9;]*[A-Za-z]|\x1bc', '', s)


def create_disk(path):
    # Create a sparse 64MB raw disk image.  No filesystem is needed — the NVMe
    # driver only needs a block device to enumerate; it does not mount or read
    # the filesystem.  truncate creates a sparse file instantly without writing
    # any data, which avoids a dependency on mke2fs / mkfs.ext2.
    subprocess.run(
        ['truncate', '-s', '64M', path],
        check=True, capture_output=True)


def main():
    if not os.path.exists(ISO):
        print('FAIL: %s not found — run make iso first' % ISO)
        sys.exit(1)

    fd, disk_path = tempfile.mkstemp(suffix='.img')
    os.close(fd)
    try:
        create_disk(disk_path)

        qemu_cmd = [
            'qemu-system-x86_64',
            '-machine', 'q35', '-cpu', 'Broadwell',
            '-cdrom', ISO, '-boot', 'order=d',
            '-drive', 'file=%s,if=none,id=nvme0' % disk_path,
            '-device', 'nvme,drive=nvme0,serial=aegis0',
            '-display', 'none', '-vga', 'std',
            '-nodefaults', '-serial', 'stdio',
            '-no-reboot', '-m', '128M',
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
        ]

        proc = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL)
        try:
            stdout, _ = proc.communicate(timeout=BOOT_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, _ = proc.communicate()

        output = stdout.decode('utf-8', errors='replace')
        lines  = [l for l in strip_ansi(output).splitlines()
                  if l.startswith('[')]

        nvme_ok = any('[NVME] OK:' in l for l in lines)
        if nvme_ok:
            print('PASS: [NVME] OK: found in serial output')
            sys.exit(0)
        else:
            print('FAIL: [NVME] OK: not found in serial output')
            print('--- kernel lines ---')
            for l in lines:
                print(l)
            sys.exit(1)
    finally:
        os.unlink(disk_path)


if __name__ == '__main__':
    main()
