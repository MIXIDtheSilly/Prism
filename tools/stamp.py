"""Whether a build is current. A build leaves a stamp in its output directory: a fingerprint of what
it was built from (each input file's path, size and modification time, and any extra values, such
as a setting). The build is current while its inputs give the same fingerprint, so a rebuild is
needed after any change, back to an older file included, and a deleted output directory takes its
stamp with it. The fingerprint is taken before building, so a file changed during a build makes the
next run build again.

    fp = stamp.fingerprint(inputs())
    if not args.force and stamp.recorded(args.out) == fp:
        ...current: nothing to do...
    ... build ...
    stamp.write(args.out, fp)
"""
import hashlib
import os

NAME = '.prism-stamp'
SKIPPED = {'__pycache__', '.git'}


def files(path):
    """The files at path: itself, or every file under it, in a stable order."""
    if os.path.isfile(path):
        yield path
        return
    for top, dirs, names in os.walk(path):
        dirs[:] = sorted(d for d in dirs if d not in SKIPPED)
        for name in sorted(names):
            if name != NAME:
                yield os.path.join(top, name)


def fingerprint(inputs, extra=()):
    h = hashlib.sha256()
    for path in sorted({os.path.abspath(p) for p in inputs}):
        if not os.path.exists(path):
            h.update(f'{path}\0missing\n'.encode())
            continue
        for f in files(path):
            st = os.stat(f)
            h.update(f'{f}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode())
    for value in extra:
        h.update(f'\0{value}\n'.encode())
    return h.hexdigest()


def recorded(out):
    """The fingerprint out's build was made from, or None."""
    try:
        with open(os.path.join(out, NAME)) as f:
            return f.read().strip() or None
    except OSError:
        return None


def current(out, inputs, extra=()):
    return recorded(out) == fingerprint(inputs, extra)


def up_to_date(out, fingerprint_, force=False):
    """Whether out was built from fingerprint_, and so needs nothing; says so if it was."""
    if not force and recorded(out) == fingerprint_:
        print(f'{out}: current (--force builds it anyway)')
        return True
    return False


def write(out, fingerprint_):
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, NAME)
    with open(path + '.tmp', 'w', encoding='utf-8') as f:
        f.write(fingerprint_ + '\n')
    os.replace(path + '.tmp', path)
