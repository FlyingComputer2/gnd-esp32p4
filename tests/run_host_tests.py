#!/usr/bin/env python3
"""Build and run the portable USB service against the actual L3 queue handle."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--l3', type=Path, required=True)
p.add_argument('--bfc', type=Path, required=True)
p.add_argument('--idf', type=Path, default=os.environ.get('IDF_PATH'))
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
if not a.idf:
    p.error('source ESP-IDF export.sh or provide --idf')
root = Path(__file__).resolve().parents[1]
cjson = a.idf / 'components/json/cJSON'
flags = ['-fsanitize=address,undefined', '-g'] if a.sanitize else []
with tempfile.TemporaryDirectory(prefix='rover-usb-test-') as directory:
    work = Path(directory)
    subprocess.run(['cc', *flags, '-c', str(cjson / 'cJSON.c'), '-I', str(cjson), '-o', str(work / 'json.o')], check=True)
    includes = [root / 'tests/stubs', root / 'components/usb_upstream/include', a.l3 / 'src/manager', a.bfc / 'src', cjson]
    command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', *flags]
    for path in includes:
        command += ['-I', str(path.resolve())]
    command += [str(root / 'tests/usb_upstream_test.cpp'), str(work / 'json.o'), '-pthread', '-o', str(work / 'test')]
    subprocess.run(command, check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    subprocess.run([str(work / 'test')], check=True, env=env)
