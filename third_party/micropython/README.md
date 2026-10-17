# Pinned MicroPython source bundle

- MicroPython **v1.24.1**, upstream MIT license (see LICENSE and archive notices).
- Unmodified upstream source archive:
  https://codeload.github.com/micropython/micropython/tar.gz/refs/tags/v1.24.1
- Archive SHA-256:
  `a291173de1d680786a66dd2381805b6d134defaecea670a560dc7e7d8de540ad`.
  Verify from the repository root with `sha256sum -c third_party/micropython/SHA256SUMS`.
- Static fixture: `tests/fixtures/micropython.elf`; its hash manifest is adjacent.
- Custom Linux-user port: `user/linux/micropython/`; no changes to upstream VM,
  parser or GC sources. Includes the upstream x86-64 generic GC register/stack helper.
- Reproduction: `scripts/build-micropython.sh /path/to/fresh-build-directory`.
  Uses the already pinned `third_party/linux-l1/musl-1.2.5.tar.gz`; reuses the
  integer-only printf patch and scalar object list in a **separate** musl build.
  The libc copyright notice is also copied to `COPYRIGHT.musl`.
- Fixed 256KiB BSS GC heap, 8KiB source limit, 8KiB stack-check budget; scalar
  integer bytecode only; sys/builtins supported, external imports/float/threads/
  JIT/native emitters/REPL disabled. This is not general MicroPython/CPython/Linux.
- Kernel builds use the checked-in fixture and never download or rebuild upstream.
- Full scope, toolchain, tests and evidence: `docs/LINUX_MICROPYTHON.md`.
