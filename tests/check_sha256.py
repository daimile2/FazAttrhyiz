#!/usr/bin/env python3
"""Host differential check; optional argument: NIST SHA256*.rsp vector file."""
import ctypes as c
import hashlib
import pathlib
import random
import re
import subprocess
import sys
import tempfile
root = pathlib.Path(__file__).resolve().parents[1]
class Context(c.Structure):
    _fields_ = [('state', c.c_uint32 * 8), ('bytes', c.c_uint64),
                ('block', c.c_uint8 * 64), ('used', c.c_size_t), ('finalized', c.c_bool)]
with tempfile.TemporaryDirectory() as tmp:
    so = pathlib.Path(tmp) / 'sha256.so'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    '-O2', '-I'+str(root/'include'), str(root/'lib/sha256.c'), '-o', str(so)], check=True)
    lib = c.CDLL(str(so))
    lib.cm_sha256.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    lib.cm_sha256_init.argtypes = [c.POINTER(Context)]
    lib.cm_sha256_update.argtypes = [c.POINTER(Context), c.c_void_p, c.c_size_t]
    lib.cm_sha256_final.argtypes = [c.POINTER(Context), c.c_void_p]
    rng = random.Random(256)
    cases = [(bytes(rng.randrange(256) for _ in range(n)), None) for n in
             list(range(130)) + [255, 256, 511, 512, 513, 4096, 65536]]
    for name in sys.argv[1:]:
        for bits, message, digest in re.findall(r'Len = (\d+)\s+Msg = ([0-9a-fA-F]+)\s+MD = ([0-9a-fA-F]+)', pathlib.Path(name).read_text()):
            assert int(bits) % 8 == 0
            cases.append((bytes.fromhex(message)[:int(bits)//8], bytes.fromhex(digest)))
    for message, expected in cases:
        expected = expected or hashlib.sha256(message).digest()
        out = (c.c_uint8 * 32)()
        assert lib.cm_sha256(message, len(message), out) == 0
        assert bytes(out) == expected
        ctx = Context()
        assert lib.cm_sha256_init(c.byref(ctx)) == 0
        offset = 0
        while offset < len(message):
            count = min(rng.randrange(1, 98), len(message)-offset)
            assert lib.cm_sha256_update(c.byref(ctx), message[offset:offset+count], count) == 0
            offset += count
        assert lib.cm_sha256_final(c.byref(ctx), out) == 0
        assert bytes(out) == expected
    print(f'SHA256 differential/vector checks PASS: {len(cases)} cases, one-shot + streaming')
