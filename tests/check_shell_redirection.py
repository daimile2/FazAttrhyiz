#!/usr/bin/env python3
"""Verify the cold-boot serial outputs from docs/SHELL.md's real VM tests."""
import argparse
from pathlib import Path
import sys


def check(platform, path):
    text = path.read_text(errors='replace')

    def body(command):
        # init can log between the prompt and the typed command, especially
        # on VBox. Match the complete command, not an adjacent prompt prefix.
        marker = command + '\n'
        if text.count(marker) != 1:
            raise ValueError(f'expected exactly one command: {command}')
        tail = text.split(marker, 1)[1]
        prompts=[tail.find(p) for p in ('capmix>', 'fazattrhyiz>') if p in tail]
        if not prompts:
            raise ValueError(f'no returned prompt: {command}')
        return tail[:min(prompts)]

    paths = ('/redirect.txt', '/elf-copy.txt', '/listing.txt') if platform == 'qemu' else (
        '/vbox.txt', '/vbox-elf.txt', '/vbox-list.txt')
    expected = 'redir-first\nredir-append\n\n'  # file + builtin cat's console newline
    for name in paths[:2]:
        actual = body('cat ' + name)
        if actual != expected:
            raise ValueError(f'{name}: expected overwrite+append bytes, got {actual!r}')
    listing = body('cat ' + paths[2])
    names = set(listing.split())
    if not {'shell.elf', 'cat.elf', 'ls.elf', 'nc.elf', 'http-get.elf'} <= names:
        raise ValueError('directory listing incomplete')
    if 'ls entries' in listing or 'user[' in listing:
        raise ValueError('diagnostic metadata leaked into stdout file')
    if body('cat /guard.txt') != 'guarded\n\n':
        raise ValueError('rejected redirection truncated guard file')
    if platform == 'qemu':
        http = body('cat /http.txt')
        if 'HTTP/1.0 200 OK' not in http or 'body verified from python' not in http:
            raise ValueError('persisted HTTP status/body missing')
        if 'http-get body bytes' in http or 'http-get exit' in http:
            raise ValueError('HTTP stderr metadata leaked into stdout')
    if 'root=disk backend=ahci' not in text or 'formatting' in text:
        raise ValueError('expected existing AHCI disk root without reformatting')
    if 'ACPI S5 request' not in text:
        raise ValueError('guest did not reach normal ACPI shutdown')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=('qemu', 'vbox'))
    parser.add_argument('cold_log', type=Path)
    args = parser.parse_args()
    try:
        check(args.platform, args.cold_log)
    except (OSError, ValueError) as error:
        print(f'shell redirection coldboot: FAIL: {error}', file=sys.stderr)
        return 1
    print(f'shell redirection coldboot: PASS ({args.platform}: overwrite/append/stdin/ELF/listing/guard/root/shutdown)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
