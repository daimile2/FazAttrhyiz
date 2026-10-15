# Phase 6: SHA-256 content verification

> 历史阶段记录：保留当时实现与验收边界；后续功能可能已扩展。当前状态见
> [内核总览](../KERNEL_OVERVIEW.zh-CN.md)，操作测试见 [VirtualBox手册](../VIRTUALBOX_TESTING.zh-CN.md)。

`lib/sha256.c` is an original, allocation-free implementation of SHA-256 from
[FIPS 180-4](https://doi.org/10.6028/NIST.FIPS.180-4), not copied third-party code.
`cm_sha256_init/update/final` supports streaming; `cm_sha256` is the one-shot API.
Initialize a context before use; finalization ends it until reinitialization.
NULL input is allowed only for zero length. Lengths exceeding the standard's
64-bit bit-count limit are rejected. Buffers must not overlap the context.

`cm_sign_t` supplies immutable content, its byte length, a trusted expected
32-byte digest, flags and an optional id. `cm_sign_verify` retains signed/module/
notarized policy checks, computes the actual content SHA-256, and examines all
32 digest bytes. Missing/malformed digests and mismatches fail closed.
`cm_sip_allows_kext` invokes the same verifier. The caller must obtain the
expected digest from trusted policy and prevent concurrent content modification
through verification and subsequent use. This is content integrity checking;
it provides no public-key signer authentication or certificate validation.
The flags remain policy metadata. ELF loading is unchanged and is not newly
signature-gated by this phase.

`boot/phase6_test.c` uses hardcoded independently published expected results:
- `abc` and the 56-byte `abcdbc...nopq` input: [NIST SHA-256 worked examples](https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Standards-and-Guidelines/documents/examples/SHA256.pdf).
- Empty input: `SHA256ShortMsg.rsp`, `Len = 0`, from [NIST CAVP byte-oriented SHA vectors](https://csrc.nist.gov/projects/cryptographic-algorithm-validation-program/secure-hashing).

These are compiled into hosted/kernel selftests; no external image files are
needed. Tests cover one-shot and byte-wise streaming, two padding blocks,
finalized-context errors, invalid pointers/length overflow, policy denials,
missing/short digest, tampered content and each of the 32 comparison positions.
Logs: `sha256 known vectors verified`, `sign: digest verified`,
`sign: digest mismatch rejected`, `sign: reject missing/invalid digest or content`,
`phase6: sign accept/reject verified`, `phase6 selftest: PASS`.

Additional reproducible host checks:
```
python3 tests/check_sha256.py
python3 tests/check_sha256.py /path/to/SHA256ShortMsg.rsp /path/to/SHA256LongMsg.rsp
```
The script compares boundary/binary/multiblock inputs with Python hashlib and
optionally NIST response files, using both one-shot and varied streaming chunks.
Passing vectors is not FIPS/CAVP certification.

SIP reports actual CR4 SMEP/SMAP bits separately from `kpti_configured`.
KASLR random relocation remains unimplemented (`slide=0`). No CR3/entry-path,
VMO, ELF, IRQ or network design changes are made.
