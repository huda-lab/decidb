"""Replace the vendored HiGHS (third_party/highs) with an upstream release and reapply DeciDB's patch series.

usage: make upgrade-highs HIGHS_VERSION=<x.y.z>

1. Download the release tarball from github.com/ERGO-Code/HiGHS.
2. Ask upstream's own CMake build, through CMake's file API, which sources its static library compiles in DeciDB's
   configuration (HConfig.h: no HiPO, no GPU, no zlib).
3. Replace highs/ and extern/ with upstream's, minus every source file that build does not compile. The vendored tree
   is then the source list: CMakeLists.txt and the amalgamation (scripts/package_build.py) compile all of it.
4. Write the version into HConfig.h and README.md.
5. Apply patches/*.patch in order, stopping at the first that does not apply. The tree then holds the new release plus
   the patches before the failing one; README.md says how to port it.

Rerunning at the current version leaves the tree unchanged.
"""

import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join('third_party', 'highs')
UPSTREAM_DIRS = ['highs', 'extern']
SOURCE_EXTENSIONS = ('.c', '.cc', '.cpp')
# Upstream options matching HConfig.h: a static library without the optional parts that need new dependencies.
CONFIGURATION = [
    '-DFAST_BUILD=ON',
    '-DHIPO=OFF',
    '-DCUPDLP_GPU=OFF',
    '-DZLIB=OFF',
    '-DBUILD_SHARED_LIBS=OFF',
    '-DBUILD_SHARED_EXTRAS_LIB=OFF',
    '-DBUILD_TESTING=OFF',
]


def download(version, work):
    url = f'https://github.com/ERGO-Code/HiGHS/archive/refs/tags/v{version}.tar.gz'
    tarball = os.path.join(work, 'highs.tar.gz')
    urllib.request.urlretrieve(url, tarball)
    with tarfile.open(tarball) as archive:
        commit = archive.pax_headers['comment']
        top = archive.getmembers()[0].name
        archive.extractall(work, filter='data')
    return os.path.join(work, top), commit[:9]


def compiled_sources(upstream, work):
    """The sources upstream's static libraries compile, relative to the upstream root."""
    build = os.path.join(work, 'build')
    query = os.path.join(build, '.cmake', 'api', 'v1', 'query')
    os.makedirs(query)
    open(os.path.join(query, 'codemodel-v2'), 'w').close()
    configure = subprocess.run(
        ['cmake', '--no-warn-unused-cli', '-S', upstream, '-B', build] + CONFIGURATION, capture_output=True, text=True
    )
    if configure.returncode != 0:
        sys.exit(f'upgrade_highs: configuring upstream failed:\n{configure.stdout}{configure.stderr}')
    reply = os.path.join(build, '.cmake', 'api', 'v1', 'reply')
    index = json.load(open(glob.glob(os.path.join(reply, 'index-*.json'))[0]))
    model = json.load(open(os.path.join(reply, index['reply']['codemodel-v2']['jsonFile'])))
    sources = set()
    for entry in model['configurations'][0]['targets']:
        target = json.load(open(os.path.join(reply, entry['jsonFile'])))
        if target['type'] != 'STATIC_LIBRARY':
            continue
        for source in target['sources']:
            path = source['path']
            # Unity-build files are generated in the build directory, which lies outside the upstream root.
            if path.endswith(SOURCE_EXTENSIONS) and not os.path.isabs(path):
                sources.add(os.path.normpath(path))
    return sources


def replace_tree(upstream, sources):
    for name in UPSTREAM_DIRS:

        def unbuilt_sources(directory, names):
            relative = os.path.relpath(directory, upstream)
            return [n for n in names if n.endswith(SOURCE_EXTENSIONS) and os.path.join(relative, n) not in sources]

        target = os.path.join(ROOT, VENDOR, name)
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(os.path.join(upstream, name), target, ignore=unbuilt_sources)
    shutil.copyfile(os.path.join(upstream, 'LICENSE.txt'), os.path.join(ROOT, VENDOR, 'LICENSE'))


def write_version(version, commit):
    major, minor, patch = version.split('.')
    defines = {
        'HIGHS_GITHASH': f'"{commit}"',
        'HIGHS_VERSION_MAJOR': major,
        'HIGHS_VERSION_MINOR': minor,
        'HIGHS_VERSION_PATCH': patch,
        'HIGHS_EXTRAS_VERSION': f'"{version}"',
    }
    stamp('HConfig.h', [(rf'(?m)^#define {name} .*$', f'#define {name} {value}') for name, value in defines.items()])
    stamp('README.md', [(r'HiGHS \d+\.\d+\.\d+ \(`\w+`\)', f'HiGHS {version} (`{commit}`)')])


def stamp(file, substitutions):
    path = os.path.join(ROOT, VENDOR, file)
    text = open(path).read()
    for pattern, replacement in substitutions:
        text = re.sub(pattern, replacement, text)
    open(path, 'w').write(text)


def apply_patches():
    for patch in sorted(glob.glob(os.path.join(ROOT, VENDOR, 'patches', '*.patch'))):
        result = subprocess.run(['git', 'apply', f'--directory={VENDOR}', patch], cwd=ROOT)
        if result.returncode != 0:
            sys.exit(
                f'upgrade_highs: {os.path.basename(patch)} does not apply; the tree holds the release plus the '
                f'patches before it (see {VENDOR}/README.md)'
            )
        print(f'applied {os.path.basename(patch)}')


def main():
    if len(sys.argv) != 2 or not re.fullmatch(r'\d+\.\d+\.\d+', sys.argv[1]):
        sys.exit('usage: make upgrade-highs HIGHS_VERSION=<x.y.z>')
    version = sys.argv[1]
    with tempfile.TemporaryDirectory() as work:
        upstream, commit = download(version, work)
        replace_tree(upstream, compiled_sources(upstream, work))
    write_version(version, commit)
    apply_patches()
    print(f'HiGHS {version} ({commit}) vendored in {VENDOR}')


if __name__ == '__main__':
    main()
