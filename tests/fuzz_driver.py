#!/usr/bin/env python3
"""Runs fuzz_methods over every work item, surviving crashes and hangs.

The fuzzer writes the index of the item it is about to run into a state file.
When the process dies (signal, sanitizer report, qFatal) or stops making
progress, the item is recorded and the run resumes with the next one.
Everything runs in a throwaway sandbox directory with its own HOME.
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--timeout", type=float, default=20.0, help="seconds without progress before an item counts as hung")
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--end", type=int, default=-1)
    ap.add_argument("--classes", default=None, help="regular expression: only these classes")
    ap.add_argument("--lifetime", action="store_true",
                    help="lifetime mode: destroy what objects point at or hand out, then use them (see fuzz_methods.cpp)")
    ap.add_argument("--valgrind", action="store_true",
                    help="run under valgrind memcheck; the first invalid read, write or free fails the item")
    ap.add_argument("--by-name", action="store_true",
                    help="call every method by name with arguments of its own kinds (finds overloads qtc cannot tell apart)")
    ap.add_argument("--seed", type=int, default=0,
                    help="> 0: the items in an order of their own and random argument rounds (see fuzz_methods.cpp)")
    args = ap.parse_args()
    args.binary = os.path.abspath(args.binary)
    args.work = os.path.abspath(args.work)
    args.report = os.path.abspath(args.report)

    if os.path.exists(args.work):
        shutil.rmtree(args.work)
    sandbox = os.path.join(args.work, "sandbox")
    home = os.path.join(args.work, "home")
    os.makedirs(sandbox)
    os.makedirs(home)
    env = dict(os.environ)
    env.update({"HOME": home, "XDG_CONFIG_HOME": os.path.join(home, ".config"),
                "XDG_DATA_HOME": os.path.join(home, ".local/share"), "XDG_CACHE_HOME": os.path.join(home, ".cache"),
                "XDG_RUNTIME_DIR": os.path.join(home, "run"), "TMPDIR": os.path.join(home, "tmp"),
                "QT_QPA_PLATFORM": "offscreen",
                "QT_LOGGING_RULES": "*=false", "QT_FATAL_WARNINGS": "0",
                "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1:allocator_may_return_null=1:handle_abort=1",
                "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1"})
    os.makedirs(env["XDG_RUNTIME_DIR"], mode=0o700)
    os.makedirs(env["TMPDIR"])

    # The fuzzer calls methods that write and delete files. It gets no absolute path from its own
    # values, but started as root it runs as 'nobody', so that its sandbox and home are all it can
    # change (a disk cache once given "/" as its directory deleted system files).
    drop = {}
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        import pwd
        try:
            nobody = pwd.getpwnam("nobody")
            for top in (args.work,):
                for root, dirs, files in os.walk(top):
                    for d in [root] + [os.path.join(root, x) for x in dirs + files]:
                        os.chown(d, nobody.pw_uid, nobody.pw_gid)
            drop = {"user": nobody.pw_uid, "group": nobody.pw_gid, "extra_groups": []}
        except KeyError:
            print("fuzz: started as root and there is no 'nobody' user: the fuzzer keeps root's rights",
                  file=sys.stderr)

    def own(path):   # files the driver writes and the fuzzer must write too
        if drop:
            os.chown(path, drop["user"], drop["group"])

    extra = ["--classes", args.classes] if args.classes else []
    if args.lifetime:
        extra.append("--lifetime")
    if args.seed:
        extra += ["--seed", str(args.seed)]
    if args.by_name:
        extra.append("--by-name")
    listing = subprocess.run([args.binary, "--list"] + extra, env=env, cwd=sandbox, capture_output=True, text=True,
                             timeout=600, **drop)
    items = [l.split("\t") for l in listing.stdout.splitlines() if l.strip()]
    end = len(items) if args.end < 0 else min(args.end, len(items))
    state = os.path.join(args.work, "state.txt")
    log_path = os.path.join(args.work, "fuzz.log")
    skipped_path = os.path.join(args.work, "skipped.txt")
    calls = 0
    failures = []
    ambiguous = set()
    idx = args.start
    t0 = time.time()
    runs = 0
    while idx < end:
        runs += 1
        with open(state, "w") as f:
            f.write("%d\n" % idx)
        own(state)
        with open(log_path, "w") as log:
            vg_log = os.path.join(args.work, "valgrind.log")
            supp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "valgrind.supp")
            wrapper = ["valgrind", "--quiet", "--undef-value-errors=no", "--exit-on-first-error=yes",
                       "--error-exitcode=99", "--num-callers=30", "--suppressions=" + supp,
                       "--log-file=" + vg_log] if args.valgrind else []
            p = subprocess.Popen(wrapper + [args.binary, "--start", str(idx), "--end", str(end), "--state", state,
                                  "--skipped", skipped_path] + extra,
                                 env=env, cwd=sandbox, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True, **drop)
            last, last_change, hung = None, time.time(), False
            while True:
                try:
                    rc = p.wait(timeout=0.25)
                    break
                except subprocess.TimeoutExpired:
                    pass
                try:
                    cur = open(state).read().strip()
                except OSError:
                    cur = last
                if cur != last:
                    last, last_change = cur, time.time()
                elif time.time() - last_change > args.timeout:
                    hung = True
                    os.killpg(p.pid, signal.SIGKILL)
                    rc = p.wait()
                    break
        cur = open(state).read().split()
        with open(log_path, errors="replace") as f:   # --by-name reports what it could not resolve
            ambiguous.update(l[10:].rstrip("\n") for l in f if l.startswith("AMBIGUOUS "))
        try:
            calls += int(cur[1]) if len(cur) > 1 else 0
        except ValueError:
            pass
        if rc == 0 and cur and cur[0] == "done":
            break
        # every item ran, and then the process failed: in qtc_shutdown, destroying what the run made
        at_shutdown = bool(cur) and cur[0] == "done"
        try:
            at = end - 1 if at_shutdown else int(cur[0])
        except (ValueError, IndexError):
            at = idx
        full = open(log_path, errors="replace").read()
        tail = full[-6000:]
        # the whole sanitizer report (its stacks start well before the end of the log)
        report_at = -1
        for marker in ("==ERROR: ", "runtime error:", "ERROR: AddressSanitizer", "CRASH: fatal signal"):
            k = full.rfind(marker)
            if k >= 0:
                report_at = full.rfind("\n", 0, k) + 1
                break
        sanitizer = full[report_at:report_at + 20000] if report_at >= 0 else ""
        if args.valgrind and os.path.exists(vg_log):
            tail += "\n--- valgrind\n" + open(vg_log, errors="replace").read()[:6000]
        call_lines = [l for l in tail.splitlines() if l.startswith("CALL ")]
        last_call = call_lines[-1][5:] if call_lines else ""
        summary = ""
        for line in tail.splitlines():
            if "SUMMARY:" in line or "ERROR: AddressSanitizer" in line or "runtime error" in line or "FATAL" in line or \
                    "Invalid read" in line or "Invalid write" in line or "Invalid free" in line or "Mismatched free" in line:
                summary = line.strip()
                break
        item = items[at] if at < len(items) else ["?", "?", "?", "?"]
        what = "hang" if hung else ("signal %d" % -rc if rc < 0 else ("memory error" if args.valgrind and rc == 99 else "exit %d" % rc))
        if at_shutdown:
            what += " at shutdown"
        # "from": the first item of the process run, where fuzz_minimize.py starts looking
        rec = {"index": at, "from": idx, "class": item[1], "sig": item[2], "kind": item[3], "what": what, "summary": summary,
               "call": last_call, "tail": tail[-1500:], "sanitizer": sanitizer}
        failures.append(rec)
        print("FAILED %5d %-28s %-9s %s  %s" % (at, item[1], rec["what"], last_call, summary), flush=True)
        idx = at + 1
    skipped = []
    if os.path.exists(skipped_path):
        skipped = sorted({l.rstrip("\n") for l in open(skipped_path) if l.strip()})
    report = {"items": end - args.start, "calls": calls, "runs": runs, "seconds": round(time.time() - t0, 1),
              "items_without_receiver": len(skipped), "without_receiver": skipped, "failures": failures,
              "ambiguous": sorted(ambiguous)}
    with open(args.report, "w") as f:
        json.dump(report, f, indent=1)
    print("fuzz: %d items, %d calls in %.0fs, %d process runs, %d items without a receiver, %d failures" % (
        end - args.start, calls, time.time() - t0, runs, len(skipped), len(failures)))
    if args.by_name:
        print("by name: %d call shapes qtc cannot resolve without a signature" % len(ambiguous))
        for a in sorted(ambiguous):
            print("  " + a)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
