#!/usr/bin/env python3
"""Print backtraces for the ctest entries that died from a signal.

The Linux run reports a crash as `***Exception: SegFault` and nothing else: the
test harness never gets to print an assertion message, so the log names the test
but not the line. This script replays the failed entries under gdb and keeps
only the runs that actually crashed, which turns each `SegFault` into a stack.

It runs after ctest has already failed, so it must never make things worse:
every step is best effort, a test that times out or misbehaves is skipped, and
the caller marks the whole step non-fatal anyway.

    python3 scripts/linux-crash-backtraces.py --build-dir build/<preset>
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

FAILED_LOG = os.path.join("Testing", "Temporary", "LastTestsFailed.log")
BACKTRACE_FRAMES = 14
# gdb needs a ceiling: a test that hangs would otherwise eat the job budget.
PER_TEST_TIMEOUT = 180
TOTAL_BUDGET = 1500
CRASH_MARKERS = ("Program received signal", "SIGSEGV", "SIGABRT", "SIGBUS", "SIGILL", "SIGFPE")


def failed_test_names(build_dir):
    path = os.path.join(build_dir, FAILED_LOG)
    if not os.path.exists(path):
        return []
    names = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            _, _, name = line.partition(":")
            names.append(name or line)
    return names


def ctest_commands(build_dir):
    """`ctest --show-only=json-v1` is the only supported way to learn the exact
    command line and environment of every entry."""
    try:
        completed = subprocess.run(
            ["ctest", "--test-dir", build_dir, "-N", "--show-only=json-v1"],
            capture_output=True, text=True, timeout=300, check=False)
    except (OSError, subprocess.SubprocessError) as error:
        print("could not ask ctest for the test list: %s" % error)
        return {}
    if completed.returncode != 0:
        print("ctest --show-only failed: %s" % completed.stderr.strip()[:400])
        return {}
    try:
        payload = json.loads(completed.stdout)
    except ValueError as error:
        print("ctest json is not json: %s" % error)
        return {}

    table = {}
    for test in payload.get("tests", []):
        properties = {}
        for entry in test.get("properties", []):
            properties[entry.get("name")] = entry.get("value")
        environment = test.get("environment")
        if environment is None:
            environment = properties.get("ENVIRONMENT")
        if isinstance(environment, str):
            environment = [environment]
        table[test.get("name")] = {
            "command": test.get("command") or [],
            "environment": environment or [],
            "working_directory": properties.get("WORKING_DIRECTORY"),
        }
    return table


def test_environment(entry):
    environment = dict(os.environ)
    for item in entry["environment"]:
        key, separator, value = item.partition("=")
        if not separator:
            continue
        if key == "PATH":
            current = environment.get("PATH", "")
            environment["PATH"] = value + (os.pathsep + current if current else "")
        else:
            environment[key] = value
    return environment


def run_under_gdb(command, environment, working_directory):
    invocation = [
        "gdb", "--batch", "--nx", "--quiet",
        "-ex", "set confirm off",
        "-ex", "set pagination off",
        "-ex", "run",
        "-ex", "thread apply all bt %d" % BACKTRACE_FRAMES,
        "-ex", "quit",
        "--args",
    ] + command
    try:
        completed = subprocess.run(invocation, capture_output=True, text=True,
                                   timeout=PER_TEST_TIMEOUT, env=environment,
                                   cwd=working_directory or None, check=False)
    except subprocess.TimeoutExpired:
        return None
    return (completed.stdout or "") + (completed.stderr or "")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--max-tests", type=int, default=40)
    parser.add_argument("--only", nargs="*", default=None,
                        help="restrict the replay to these ctest names")
    arguments = parser.parse_args()

    if shutil.which("gdb") is None:
        print("gdb is not installed; skipping the crash backtraces")
        return 0

    names = arguments.only if arguments.only else failed_test_names(arguments.build_dir)
    if not names:
        print("no failed tests recorded; nothing to replay")
        return 0
    table = ctest_commands(arguments.build_dir)
    print("replaying %d failed entries under gdb" % len(names))

    deadline = time.time() + TOTAL_BUDGET
    printed = 0
    for name in names[: arguments.max_tests]:
        entry = table.get(name)
        if not entry or not entry["command"]:
            print("\n===== %s: no command recorded, skipped" % name)
            continue
        if time.time() > deadline:
            print("\n(budget exhausted, %d entries not replayed)" % (len(names) - printed))
            break
        print("\n===== %s" % name)
        output = run_under_gdb(entry["command"], test_environment(entry),
                               entry["working_directory"])
        if output is None:
            print("(timed out after %ds)" % PER_TEST_TIMEOUT)
            continue
        if not any(marker in output for marker in CRASH_MARKERS):
            print("(did not crash on replay; first failing assertion is in the ctest log)")
            continue
        printed += 1
        print(output.strip())
    print("\ncrash backtraces printed: %d" % printed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
