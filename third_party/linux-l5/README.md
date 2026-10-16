# Fixed musl 1.2.5 L5 static probe

`musl-mmap.elf` is built from `user/linux/mmap_probe.c` with the **unmodified**
musl 1.2.5 CRT, libc and mallocng. No L1 BSS allocator override or printf patch.
It tests malloc(2MiB), anonymous PRIVATE mmap(1MiB), page contents and cleanup.
It is not evidence of general musl/glibc or arbitrary BusyBox compatibility.

musl upstream source is already vendored, byte-pinned in
`../linux-l1/musl-1.2.5.tar.gz` and that directory's SHA256SUMS/README.
Musl copyright/license is reproduced in `COPYRIGHT.musl` (MIT and notices).
The complete corresponding archive must accompany redistribution.

Rebuild offline from the repository root:

```sh
scripts/build-musl-l5.sh /tmp/fresh-musl-l5
cmp /tmp/fresh-musl-l5/musl-mmap.elf third_party/linux-l5/musl-mmap.elf
sha256sum -c third_party/linux-l5/SHA256SUMS
```

Pinned output was built with Ubuntu GCC 13.3.0 and GNU binutils 2.42.
The script copies the probe to a stable temporary filename so the unstripped
ELF's STT_FILE record is reproducible. Other toolchain versions may differ.
Only the OSABI byte is marked Linux, as required by CapMix's explicit persona
selection; no executable code or allocator source is patched.
See `docs/LINUX_ABI_L5.md` for the mmap guard-page rejection and remaining limits.
