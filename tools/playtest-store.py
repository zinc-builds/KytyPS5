#!/usr/bin/env python3
"""playtest-store.py <install> <title id> <builds dir> [--seed]: is the install's shader store warm
for its build, and if not, can another install's store seed it?

The shader store (_PipelineCache/<title>.shaders, with the driver's pipeline cache <title>.bin) is
per install directory, and the emulator reuses it only when its header matches the build: the
header carries KYTY_SHADER_STORE_SOURCE_HASH, the hash of the sources that decide what the store
records (shader_store_key.cmake). A new build of a commit that changed none of them can reuse the
store of any other build with the same hash; one that changed them starts cold. This computes the
hash for the install's commit (from BUILD_MODE) from git, the way shader_store_key.cmake does, and
looks for the largest store under <builds dir>/*/install with that hash in its header.

Prints one line: "warm ...", "seeded ..." (with --seed: the donor's store copied in) or "cold ...".
Exit status 0 in every case but a usage error.
"""
import hashlib
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The fixed part of the input list in CMakeLists.txt (kyty_shader_store_inputs); the rest is every
# .cpp/.h/.inc under src/graphics/shader.
FIXED = [
    'graphics/guest_gpu/gpu_defs.h', 'graphics/guest_gpu/gpu_format.h', 'graphics/guest_gpu/gpu_format.cpp',
    'graphics/guest_gpu/hardwareContext.h', 'graphics/host_gpu/vulkanCommon.h',
    'graphics/host_gpu/vulkanCommon.cpp', 'graphics/host_gpu/renderer/pipeline/blendMapping.h',
    'graphics/host_gpu/renderer/pipeline/pipelineCache.h', 'graphics/host_gpu/renderer/pipeline/pipelineCache.cpp',
    'graphics/host_gpu/renderer/pipeline/pipelineSerialization.cpp', 'graphics/host_gpu/renderer/renderTarget.h',
]


def git(*args, binary=False):
    r = subprocess.run(['git', '-C', REPO, *args], capture_output=True, text=not binary)
    return r.stdout if r.returncode == 0 else None


def store_hash(commit):
    """The 8 header bytes after the format and manual-stamp words (little-endian), or None."""
    names = git('ls-tree', '-r', '--name-only', commit, '--', 'src/graphics/shader')
    if names is None:
        return None
    files = sorted([n[len('src/'):] for n in names.split() if n.endswith(('.cpp', '.h', '.inc'))] + FIXED)
    text = ''
    for f in files:
        blob = git('cat-file', 'blob', f'{commit}:src/{f}', binary=True)
        if blob is None:
            return None
        text += f'{f}:{hashlib.sha256(blob).hexdigest()}\n'
    return bytes.fromhex(hashlib.sha256(text.encode()).hexdigest()[:16])[::-1]


def header_matches(path, key):
    try:
        with open(path, 'rb') as f:
            head = f.read(16)
    except OSError:
        return False
    # u32 format, u32 manual stamp (0: the source hash is used), u64 source hash.
    return len(head) == 16 and head[4:8] == b'\0\0\0\0' and head[8:16] == key


def size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def commit_of(install):
    try:
        with open(os.path.join(install, '..', 'BUILD_MODE')) as f:
            text = f.read()
    except OSError:
        return None
    for word in text.split():
        if word.startswith('commit='):
            return word[len('commit='):]
    return None


def main():
    args = [a for a in sys.argv[1:] if a != '--seed']
    if len(args) != 3:
        sys.exit(__doc__)
    install, title, builds = (os.path.realpath(a) if i != 1 else a for i, a in enumerate(args))
    mine = os.path.join(install, '_PipelineCache', f'{title}.shaders')
    commit = commit_of(install)
    key = store_hash(commit) if commit else None
    if key is None:
        print(f'cold? cannot tell: no commit for {install} in this repository; '
              f'if this build is new, expect a long first load (shaders compile as they appear)')
        return
    have = size(mine) if header_matches(mine, key) else 0
    donor, donor_size = None, 0
    try:
        dirs = sorted(os.listdir(builds))
    except OSError:
        dirs = []
    for d in dirs:
        other = os.path.join(builds, d, 'install')
        if os.path.realpath(other) == install:
            continue
        f = os.path.join(other, '_PipelineCache', f'{title}.shaders')
        if size(f) > donor_size and header_matches(f, key):
            donor, donor_size = other, size(f)
    mb = lambda n: f'{n / 1e6:.0f} MB'
    if have and have >= donor_size // 2:
        print(f'warm: the shader store has {mb(have)} for this build')
        return
    # Seed only installs in the build cache (never a checkout's own install).
    if donor and '--seed' in sys.argv and os.path.dirname(os.path.dirname(install)) == builds:
        os.makedirs(os.path.dirname(mine), exist_ok=True)
        for ext in ('.shaders', '.bin'):
            src = os.path.join(donor, '_PipelineCache', title + ext)
            dst = os.path.join(install, '_PipelineCache', title + ext)
            if os.path.exists(src):
                shutil.copyfile(src, dst + '.seed')
                os.replace(dst + '.seed', dst)
        print(f'seeded: the shader store had {mb(have)} for this build; copied {mb(donor_size)} from '
              f'{donor} (same store key), so the first load does not compile every shader')
        return
    state = f'{mb(have)} for this build' if have else ('none' if not size(mine) else 'one from another build')
    hint = f'; {donor} has {mb(donor_size)} with the same key' if donor else ''
    print(f'cold: first run on this build (shader store: {state}{hint}): expect a long first load and '
          f'shader-compile hitches until the store warms up')


if __name__ == '__main__':
    main()
