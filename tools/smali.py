"""smali/baksmali (Google's dex assembler and disassembler), fetched on first use into work/tools.

    python tools/smali.py baksmali d classes.dex -o out/
    python tools/smali.py smali a out/ -o classes.dex --api 34
"""
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request

GOOGLE = 'https://dl.google.com/android/maven2'
CENTRAL = 'https://repo1.maven.org/maven2'
VERSION = '3.0.10'
JARS = [
    (GOOGLE, 'com.android.tools.smali', 'smali', VERSION),
    (GOOGLE, 'com.android.tools.smali', 'smali-baksmali', VERSION),
    (GOOGLE, 'com.android.tools.smali', 'smali-dexlib2', VERSION),
    (GOOGLE, 'com.android.tools.smali', 'smali-util', VERSION),
    (CENTRAL, 'org.antlr', 'antlr-runtime', '3.5.2'),
    (CENTRAL, 'org.antlr', 'stringtemplate', '3.2.1'),
    (CENTRAL, 'antlr', 'antlr', '2.7.7'),
    (CENTRAL, 'org.jcommander', 'jcommander', '1.85'),
    (CENTRAL, 'com.google.guava', 'guava', '31.1-android'),
    (CENTRAL, 'com.google.guava', 'failureaccess', '1.0.1'),
    (CENTRAL, 'com.google.code.findbugs', 'jsr305', '3.0.2'),
]
MAINS = {'smali': 'com.android.tools.smali.smali.Main', 'baksmali': 'com.android.tools.smali.baksmali.Main'}
TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'work', 'tools', 'smali')


def fetch():
    os.makedirs(TOOLS, exist_ok=True)
    paths = []
    for repo, group, artifact, version in JARS:
        name = f'{artifact}-{version}.jar'
        path = os.path.join(TOOLS, name)
        if not os.path.exists(path):
            url = f'{repo}/{group.replace(".", "/")}/{artifact}/{version}/{name}'
            print(f'downloading {name}', file=sys.stderr)
            data = urllib.request.urlopen(url, timeout=60).read()
            expected = urllib.request.urlopen(url + '.sha1', timeout=60).read().decode().split()[0]
            if hashlib.sha1(data).hexdigest() != expected:
                sys.exit(f'{name}: checksum mismatch')
            with open(path + '.part', 'wb') as f:
                f.write(data)
            os.replace(path + '.part', path)
        paths.append(path)
    return paths


def java():
    candidates = [os.path.join(os.environ['JAVA_HOME'], 'bin', 'java')] if os.environ.get('JAVA_HOME') else []
    candidates += sorted(glob.glob(r'C:\Program Files\Java\jdk-*\bin\java.exe'), reverse=True)
    candidates += [r'C:\Program Files\Android\Android Studio\jbr\bin\java.exe', shutil.which('java') or '']
    for c in candidates:
        if c and (os.path.exists(c) or os.path.exists(c + '.exe')):
            return c
    sys.exit('Java not found; install a JDK or set JAVA_HOME')


def run(tool, *args):
    classpath = os.pathsep.join(fetch())
    subprocess.run([java(), '-Xmx4g', '-cp', classpath, MAINS[tool], *args], check=True)


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in MAINS:
        sys.exit(__doc__)
    run(sys.argv[1], *sys.argv[2:])
