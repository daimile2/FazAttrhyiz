# Static interpreter golden fixture

Custom MicroPython 1.24.1 + musl 1.2.5 scalar Linux-user build.
ELF64 x86-64 ET_EXEC, Linux OSABI marker; no PT_INTERP/PT_DYNAMIC.
144496 bytes; SHA-256 in `micropython.sha256`.

Rebuild in two fresh directories:

```sh
scripts/build-micropython.sh /tmp/capmix-mp-repro1
scripts/build-micropython.sh /tmp/capmix-mp-repro2
cmp /tmp/capmix-mp-repro1/micropython.elf /tmp/capmix-mp-repro2/micropython.elf
cmp tests/fixtures/micropython.elf /tmp/capmix-mp-repro1/micropython.elf
make micropython-verify
```

Reference toolchain: Ubuntu GCC 13.3.0, GNU binutils 2.42, Python 3.12;
SOURCE_DATE_EPOCH=0, TZ=UTC, LC_ALL=C, PYTHONHASHSEED=0, stripped ELF, no build-id.
The recipe does not overwrite the fixture. Different toolchains are not promised
byte-identical results; compare and investigate before updating a golden hash.
Sources, notices and custom port are retained in the repository; no build-time network.
See `third_party/micropython/README.md` and `docs/LINUX_MICROPYTHON.md`.
