"""Prepare a Prism workspace from your own Horizon OS OTA, in one go.

  1. OTA payload -> partition images            (work/images)
  2. ext4 partitions -> files + metadata manifest (work/fs)
  3. APEX modules -> files + manifests            (work/fs/apex)
  4. stock Android 14 x86_64 emulator image       (work/baseline)
  5. inventory against stock                       (work/inventory/report.md)

    python tools/prepare.py OTA.zip [--sdk %LOCALAPPDATA%/Android/Sdk] [--skip-done]
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXT4_PARTITIONS = ['system', 'system_ext', 'product', 'vendor', 'odm', 'vendor_dlkm', 'odm_dlkm']
BASELINE_PARTITIONS = ['system', 'system_ext', 'product']


def run(*args):
    print('>', ' '.join(os.path.relpath(a) if os.path.exists(a) else a for a in args), flush=True)
    subprocess.run([sys.executable, *args], check=True)


def run_parallel(jobs):
    with concurrent.futures.ThreadPoolExecutor(len(jobs) or 1) as pool:
        for future in [pool.submit(run, *job) for job in jobs]:
            future.result()


def default_sdk():
    for candidate in (os.environ.get('ANDROID_SDK_ROOT'), os.environ.get('ANDROID_HOME'),
                      os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'),
                      os.path.expanduser('~/Android/Sdk')):
        if candidate and os.path.isdir(candidate):
            return candidate
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ota', help='Horizon OS full OTA zip (or payload.bin)')
    ap.add_argument('--sdk', default=default_sdk(), help='Android SDK with system-images;android-34;google_apis;x86_64')
    ap.add_argument('--work', default='work')
    ap.add_argument('--skip-done', action='store_true', help='skip steps whose output already exists')
    args = ap.parse_args()
    ota = os.path.join(HERE, 'ota')
    images, fs = os.path.join(args.work, 'images'), os.path.join(args.work, 'fs')
    done = lambda path: args.skip_done and os.path.exists(path)  # noqa: E731

    if not done(os.path.join(images, 'system_ext.img')):
        run(os.path.join(ota, 'payload.py'), args.ota, '-o', images)
    run_parallel([(os.path.join(ota, 'ext4.py'), os.path.join(images, p + '.img'), '-o', os.path.join(fs, p))
                  for p in EXT4_PARTITIONS
                  if os.path.exists(os.path.join(images, p + '.img')) and not done(os.path.join(fs, p + '.manifest.tsv'))])
    if not done(os.path.join(fs, 'apex')):
        run(os.path.join(ota, 'apex.py'), '--fs', fs, '--images', images)

    baseline = os.path.join(args.work, 'baseline')
    stock = os.path.join(args.sdk or '', 'system-images', 'android-34', 'google_apis', 'x86_64', 'system.img')
    if not os.path.exists(stock):
        sys.exit(f'stock image not found at {stock}; install system-images;android-34;google_apis;x86_64')
    if not done(os.path.join(baseline, 'images', 'system.img')):
        run(os.path.join(ota, 'super.py'), stock, '-o', os.path.join(baseline, 'images'))
    run_parallel([(os.path.join(ota, 'ext4.py'), os.path.join(baseline, 'images', p + '.img'),
                   '-o', os.path.join(baseline, 'fs', p))
                  for p in BASELINE_PARTITIONS if not done(os.path.join(baseline, 'fs', p + '.manifest.tsv'))])

    run(os.path.join(HERE, 'inventory', 'inventory.py'), '--fs', fs, '--baseline', os.path.join(baseline, 'fs'),
        '-o', os.path.join(args.work, 'inventory'))


if __name__ == '__main__':
    main()
