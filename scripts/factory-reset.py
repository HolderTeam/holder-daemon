#!/usr/bin/env python3
import argparse
import errno
import fcntl
import os
import shutil
import sys
from pathlib import Path


def is_lock_held(lock_path: Path) -> bool:
    if not lock_path.exists():
        return False

    fd = os.open(lock_path, os.O_RDWR)
    try:
        try:
            fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.lockf(fd, fcntl.LOCK_UN)
            return False
        except OSError as ex:
            if ex.errno in (errno.EACCES, errno.EAGAIN):
                return True
            raise
    finally:
        os.close(fd)


def parse_args() -> argparse.Namespace:
    home = Path.home()
    parser = argparse.ArgumentParser(
        prog="factory-reset",
        description="Deletes holder data, config, and cache directories.",
    )
    parser.add_argument("--data-dir", default=str(home / ".local/share/holder"))
    parser.add_argument("--config-dir", default=str(home / ".config/holder"))
    parser.add_argument("--cache-dir", default=str(home / ".cache/holder"))
    parser.add_argument("--force", "--yes", action="store_true", dest="force")
    return parser.parse_args()


def remove_path(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink(missing_ok=True)
        return
    shutil.rmtree(path, ignore_errors=True)


def main() -> int:
    args = parse_args()

    if not args.force:
        print("Refusing to run without --force", file=sys.stderr)
        return 2

    data_dir = Path(args.data_dir)
    config_dir = Path(args.config_dir)
    cache_dir = Path(args.cache_dir)

    lock_path = data_dir / "server" / "holder.lock"
    if is_lock_held(lock_path):
        print(
            f"Holder lock is currently held at {lock_path}. Stop holder before running factory reset.",
            file=sys.stderr,
        )
        return 3

    remove_path(data_dir)
    remove_path(config_dir)
    remove_path(cache_dir)

    print("Factory reset complete:")
    print(f"- data:   {data_dir}")
    print(f"- config: {config_dir}")
    print(f"- cache:  {cache_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
