#!/usr/bin/env python3
"""Exercise fsinspect against malformed media under sanitizer builds."""
from pathlib import Path
import os
import random
import subprocess
import sys
import tempfile

if len(sys.argv) != 2:
    raise SystemExit('usage: parser_adversarial_test.py <fsinspect>')

fsinspect = Path(sys.argv[1]).resolve()
if not fsinspect.is_file():
    raise SystemExit(f'fsinspect not found: {fsinspect}')

env = dict(os.environ)
env.setdefault('ASAN_OPTIONS', 'abort_on_error=1:halt_on_error=1:detect_leaks=1')
env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')

rng = random.Random(0x1F51A7E)
cases = []

# Boundary sizes around the detector prefix, sectors and supported root blocks.
for size in (0, 1, 3, 4, 31, 127, 128, 129, 511, 512, 513,
             1023, 1024, 4095, 4096, 4097, 65535, 65536, 65537):
    cases.append((f'zero-{size}', bytes(size)))
    cases.append((f'ones-{size}', bytes([0xFF]) * size))
    cases.append((f'random-{size}', rng.randbytes(size)))

# Signature-shaped garbage should still require valid geometry/checksums.
for dostype in range(8):
    image = bytearray(rng.randbytes(8192))
    image[:4] = b'DOS' + bytes([dostype])
    cases.append((f'dos-signature-{dostype}', bytes(image)))

for signature in (b'SFS\x00', b'SFS\x02', b'PFS\x01', b'PFS\x02'):
    image = bytearray(rng.randbytes(8192))
    image[:4] = signature
    cases.append((f'signature-{signature.hex()}', bytes(image)))

# Structured mutations across the entire 128-byte probe prefix catch parsers
# that validate a magic value but fail to reject hostile geometry nearby.
base = bytearray(4096)
base[:4] = b'SFS\x00'
for offset in range(0, 128, 4):
    for value in (0x00, 0x01, 0x7F, 0x80, 0xFF):
        image = bytearray(base)
        image[offset:offset + 4] = bytes([value]) * min(4, len(image) - offset)
        cases.append((f'prefix-{offset:03d}-{value:02x}', bytes(image)))

with tempfile.TemporaryDirectory(prefix='filesystem-support-adversarial-') as temp:
    root = Path(temp)
    for index, (name, payload) in enumerate(cases):
        target = root / f'{index:04d}-{name}.img'
        target.write_bytes(payload)
        result = subprocess.run(
            [str(fsinspect), str(target)],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
        )

        diagnostics = result.stderr
        sanitizer_markers = (
            'AddressSanitizer',
            'UndefinedBehaviorSanitizer',
            'runtime error:',
            'LeakSanitizer',
        )
        if result.returncode not in (0, 1) or any(
                marker in diagnostics for marker in sanitizer_markers):
            raise SystemExit(
                f'adversarial case failed: {target.name}\n'
                f'exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}'
            )

print(f'adversarial fsinspect corpus: {len(cases)} malformed media cases PASS')
