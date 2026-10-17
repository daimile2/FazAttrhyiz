# Original CapMix bounded real rtld golden

- `linux-dynamic.elf`: Linux-marked ET_EXEC, PT_INTERP `/lib/ld-capmix.so.1`.
- `ld-capmix.so.1`: original, libc-free PIC user-space rtld, no self relocations.
- `libcmgold.so.1`: original DSO, function/data pointer verification.

No musl/glibc dynamic loader is bundled or claimed. Bounded IE DSO TLS only; no constructors.
This is actual DT_NEEDED loading + symbol resolution + RELA, not L4a handoff.
Sources: `user/linux/ldso/*`; no third-party implementation copied.
ELF ABI declarations come from the build toolchain's `<elf.h>`; no libc linked.

```sh
scripts/build-linux-ldso.sh /tmp/fresh-cm-ldso
for n in linux-dynamic.elf ld-capmix.so.1 libcmgold.so.1 linux-tls.elf libcm_tls_a.so.1 libcm_tls_b.so.1; do
  cmp /tmp/fresh-cm-ldso/$n tests/fixtures/ldso/$n
done
sha256sum -c tests/fixtures/ldso/SHA256SUMS
python3 scripts/check-ldso-fixtures.py tests/fixtures/ldso
```

Fixed with Ubuntu GCC13.3.0/binutils2.42, SOURCE_DATE_EPOCH=0/TZ=UTC.
Two fresh directories produced byte-identical stripped binaries. SHA256SUMS
is checked by normal kernel builds; the audit asserts actual relocations
RELATIVE=1, GLOB_DAT=4, JUMP_SLOT=1, R_X86_64_64=1 and absence of PT_TLS,
interpreter dependencies/self relocation, constructors and SIMD registers.
Other toolchain versions can differ; builds do not auto-replace fixtures.
See `docs/LINUX_ABI_LDSO.md` for exact support and security limitations.

TLS additions: `linux-tls.elf` plus `libcm_tls_a.so.1` and `libcm_tls_b.so.1`.
Both DSOs use initial-exec TLS, initialized and zero-fill variables, alignment64;
main validates FS self-pointer and independent values across a bounded spin.
Run `python3 scripts/check-tls-fixtures.py tests/fixtures/ldso` as well.
The TLS audit is separate: old main/lib remain TLS-free and the interpreter
still has no own TLS or relocations. See `docs/LINUX_ABI_TLS.md`.

The interpreter now requires CPUID NX and carries a CapMixWX v1 PT_NOTE.
It stages DSOs RW+NX and applies PT_LOAD + GNU_RELRO protections before main.
No-RELRO programs remain compatible; stale pre-hardening interpreters are rejected.
See `docs/LINUX_ABI_WX_RELRO.md` and `tests/check_linux_wx.py`.
