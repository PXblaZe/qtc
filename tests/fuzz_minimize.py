#!/usr/bin/env python3
"""Shrinks a fuzz failure to the items that cause it.

A failure that only shows up after other items ran (one corrupted an instance the
failing item then used) is found again here: starting from the item after the
previous failure, the start is moved forward for as long as the failure still
happens, then the items in between are dropped one at a time when the failure
happens without them. What is left is printed: the items that together crash.

  fuzz_minimize.py --binary ./fuzz_methods --seed 3 --index 4209 --from 4100 [--lifetime]
"""
import argparse
import os
import subprocess
import sys
import tempfile


def run(args, items, env, work, drop):
    """True when running exactly these items (in this order) fails the way the original did."""
    state = os.path.join(work, "state.txt")
    order = os.path.join(work, "order.txt")
    with open(order, "w") as f:
        f.write("\n".join(str(i) for i in items) + "\n")
    if drop and os.path.exists(state):
        os.chown(state, drop["user"], drop["group"])
    cmd = [args.binary, "--order", order, "--state", state]
    if args.seed:
        cmd += ["--seed", str(args.seed)]
    if args.lifetime:
        cmd.append("--lifetime")
    if args.by_name:
        cmd.append("--by-name")
    try:
        p = subprocess.run(cmd, env=env, cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           timeout=args.timeout, **drop)
    except subprocess.TimeoutExpired:
        return args.hang
    if p.returncode == 0:
        return False
    try:
        with open(state) as f:
            last = f.read().split()
    except OSError:
        return False
    if args.shutdown:   # every item ran, then qtc_shutdown failed
        return bool(last) and last[0] == "done"
    # the failure must happen at the failing item, not somewhere else
    return bool(last) and last[0] != "done" and int(last[0]) == len(items) - 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--index", type=int, required=True, help="the failing item")
    ap.add_argument("--from", dest="start", type=int, required=True, help="the first item of the process run that failed")
    ap.add_argument("--lifetime", action="store_true")
    ap.add_argument("--by-name", action="store_true", help="the failure came from fuzz_methods --by-name")
    ap.add_argument("--hang", action="store_true", help="the failure is a hang: a timeout counts as reproducing it")
    ap.add_argument("--shutdown", action="store_true", help="the failure happened at shutdown, after the last item")
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()
    args.binary = os.path.abspath(args.binary)
    with tempfile.TemporaryDirectory() as work:
        # a home of its own, as fuzz_driver.py gives: what the fuzzed methods save stays in here
        home = os.path.join(work, "home")
        os.makedirs(os.path.join(home, "run"), mode=0o700)
        os.makedirs(os.path.join(home, "tmp"))
        env = dict(os.environ)
        env.update({"HOME": home, "XDG_CONFIG_HOME": os.path.join(home, ".config"),
                    "XDG_DATA_HOME": os.path.join(home, ".local/share"), "XDG_CACHE_HOME": os.path.join(home, ".cache"),
                    "XDG_RUNTIME_DIR": os.path.join(home, "run"), "TMPDIR": os.path.join(home, "tmp"),
                    "QT_QPA_PLATFORM": "offscreen", "QT_LOGGING_RULES": "*=false"})
        # as in fuzz_driver.py: started as root, the fuzzer runs as 'nobody'
        drop = {}
        if hasattr(os, "geteuid") and os.geteuid() == 0:
            import pwd
            try:
                nobody = pwd.getpwnam("nobody")
                for root, dirs, files in os.walk(work):
                    for d in [root] + [os.path.join(root, x) for x in dirs + files]:
                        os.chown(d, nobody.pw_uid, nobody.pw_gid)
                drop = {"user": nobody.pw_uid, "group": nobody.pw_gid, "extra_groups": []}
            except KeyError:
                pass
        items = list(range(args.start, args.index + 1))
        if not run(args, items, env, work, drop):
            print("does not fail when run on its own from item %d" % args.start)
            return 1
        if run(args, [args.index], env, work, drop) and not args.shutdown:
            print("fails on its own: item %d" % args.index)
            return 0
        # the latest start that still fails
        lo, hi = 0, len(items) - 1   # items[lo:] fails, items[hi:] (just the item) does not
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if run(args, items[mid:], env, work, drop):
                lo = mid
            else:
                hi = mid
        keep = items[lo:]
        # drop what is not needed (delta debugging: halves first, then smaller chunks; the
        # failing item itself, last, always stays)
        n = 2
        while len(keep) > 1:
            head, last = keep[:-1], keep[-1:]
            size = max(1, len(head) // n)
            removed = False
            for start in range(0, len(head), size):
                trial = head[:start] + head[start + size:] + last
                if run(args, trial, env, work, drop):
                    keep = trial
                    n = max(n - 1, 2)
                    removed = True
                    break
            if not removed:
                if size == 1:
                    break
                n = min(n * 2, len(head))
        print("fails with items: " + " ".join(str(k) for k in keep))
    return 0


if __name__ == "__main__":
    sys.exit(main())
