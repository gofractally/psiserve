#!/usr/bin/env python3
# Run a command under a bounded concurrency lock.
#
# Tries to claim one of N non-blocking file locks under $TMPDIR/psizam-fuzz-locks.
# Exits non-zero with a clear message if all slots are held by live processes.
# Stale locks (owner PID gone) are reclaimed automatically.
#
# Usage: run-locked.py [--max-slots N] [--lock-name NAME] -- <cmd> [args...]

import argparse
import errno
import fcntl
import os
import signal
import sys
import tempfile


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def try_claim(path: str) -> int | None:
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as e:
        if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
            os.close(fd)
            return None
        os.close(fd)
        raise
    os.ftruncate(fd, 0)
    os.write(fd, f"{os.getpid()}\n".encode())
    os.fsync(fd)
    return fd


def read_holder(path: str) -> str:
    try:
        with open(path) as f:
            return f.read().strip() or "<empty>"
    except FileNotFoundError:
        return "<missing>"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-slots", type=int, default=2)
    ap.add_argument("--lock-name", default="psizam-fuzz")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    if not args.cmd:
        print("run-locked: no command given", file=sys.stderr)
        return 2
    if args.cmd and args.cmd[0] == "--":
        args.cmd = args.cmd[1:]
    if not args.cmd:
        print("run-locked: no command given", file=sys.stderr)
        return 2

    lock_dir = os.path.join(tempfile.gettempdir(), f"{args.lock_name}-locks")
    os.makedirs(lock_dir, exist_ok=True)

    held_fd = None
    slot_path = None
    for i in range(args.max_slots):
        path = os.path.join(lock_dir, f"slot-{i}.lock")
        fd = try_claim(path)
        if fd is not None:
            held_fd = fd
            slot_path = path
            break

    if held_fd is None:
        print(
            f"run-locked: refusing to start; all {args.max_slots} slots in use",
            file=sys.stderr,
        )
        for i in range(args.max_slots):
            p = os.path.join(lock_dir, f"slot-{i}.lock")
            holder = read_holder(p)
            alive = ""
            try:
                pid = int(holder.split()[0])
                alive = " (alive)" if pid_alive(pid) else " (STALE — flock should have released)"
            except (ValueError, IndexError):
                pass
            print(f"  slot {i}: pid={holder}{alive}", file=sys.stderr)
        print(
            "run-locked: wait for one to finish, or kill it explicitly before retrying",
            file=sys.stderr,
        )
        return 75  # EX_TEMPFAIL

    print(
        f"run-locked: claimed {slot_path} (pid {os.getpid()}); running: {' '.join(args.cmd)}",
        file=sys.stderr,
    )
    sys.stderr.flush()

    pid = os.fork()
    if pid == 0:
        os.execvp(args.cmd[0], args.cmd)

    def forward(signum, _frame):
        try:
            os.kill(pid, signum)
        except ProcessLookupError:
            pass

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, forward)

    _, status = os.waitpid(pid, 0)
    if os.WIFSIGNALED(status):
        return 128 + os.WTERMSIG(status)
    return os.WEXITSTATUS(status)


if __name__ == "__main__":
    sys.exit(main())
