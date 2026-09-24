# run_examples.py -- runs every program under examples/ and checks it still prints exactly what it did.
#
# The examples are realistic, newcomer-style programs (see examples/README.md). Each lives in its own
# directory:
#   examples/<name>/main.skn       the entry program (sibling .skn files / sub-directories are its modules)
#   examples/<name>/expected.out   the exact standard output
#   examples/<name>/args.txt       optional: whitespace-separated command-line arguments
#   anything else                  data files the program reads (input.txt, data.csv, ...)
#
# Each example runs with `--strict` (a warning is fatal) in a FRESH COPY of its directory in a temporary
# directory, as the working directory -- so relative paths resolve to its data files and anything it
# writes never lands in the repository. It passes when the exit code is 0, standard error is empty, and
# standard output equals expected.out byte for byte (line endings normalized to LF).
#
# Usage (repo root or anywhere; Python 3.9+):
#   python tests/run_examples.py
#   python tests/run_examples.py --config Debug
#   python tests/run_examples.py --exe build/skarnvm
#   python tests/run_examples.py --only word_freq
#   python tests/run_examples.py --update     # (re)write expected.out from the current output --
#                                             # review the diff before keeping it
#
# Exit code: 0 = every example matched; 1 = at least one did not (or the driver is missing).

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from skarn_testlib import add_driver_options, find_driver, green, red, repo_root, run_driver, say  # noqa: E402


def first_difference(out, expected):
    o = out.split("\n")
    e = expected.split("\n")
    i = 0
    while i < min(len(o), len(e)) and o[i] == e[i]:
        i += 1
    got = o[i] if i < len(o) else "<end of output>"
    want = e[i] if i < len(e) else "<end of expected.out>"
    return "output differs at line {}: got '{}', expected '{}'".format(i + 1, got, want)


def main():
    ap = argparse.ArgumentParser(description="Run every program under examples/ against its expected.out.")
    add_driver_options(ap)
    ap.add_argument("--only", default="", help="run only the example with this directory name")
    ap.add_argument("--update", action="store_true", help="(re)write expected.out from the current output")
    opts = ap.parse_args()

    exe = find_driver(opts.exe, opts.config)
    examples_dir = repo_root() / "examples"
    dirs = sorted((d for d in examples_dir.iterdir() if d.is_dir() and (d / "main.skn").is_file()),
                  key=lambda d: d.name.lower())
    if opts.only:
        dirs = [d for d in dirs if d.name == opts.only]
    if not dirs:
        suffix = " matching '{}'".format(opts.only) if opts.only else ""
        say(red("ERROR: no examples found under '{}'{}.".format(examples_dir, suffix)))
        return 1

    passed = 0
    fail_names = []
    work_root = Path(tempfile.mkdtemp(prefix="skarn_examples_"))
    try:
        for d in dirs:
            work = work_root / d.name
            shutil.copytree(str(d), str(work))
            expected_path = d / "expected.out"
            (work / "expected.out").unlink(missing_ok=True)  # not visible to the program

            arguments = ["--strict", "main.skn"]
            args_path = d / "args.txt"
            if args_path.is_file():
                arguments += args_path.read_text(encoding="utf-8-sig").split()

            r = run_driver(exe, arguments, cwd=work, timeout=opts.timeout)
            why = ""
            if r.code != 0:
                why = "exit code {}; stderr: {}".format(r.code, r.err.strip())
            elif r.err:
                why = "unexpected stderr: {}".format(r.err.strip())
            elif opts.update:
                with open(expected_path, "w", encoding="utf-8", newline="") as f:
                    f.write(r.out)
            elif not expected_path.is_file():
                why = "no expected.out (run with --update to create it)"
            else:
                expected = expected_path.read_text(encoding="utf-8-sig").replace("\r\n", "\n")
                if r.out != expected:
                    why = first_difference(r.out, expected)

            if not why:
                passed += 1
                say(green("{:<7} examples/{}".format("UPDATED" if opts.update else "PASS", d.name)))
            else:
                fail_names.append(d.name)
                say(red("FAIL    examples/{}  {}".format(d.name, why)))
    finally:
        shutil.rmtree(str(work_root), ignore_errors=True)

    say()
    say("==== examples: {} passed, {} failed ====".format(passed, len(fail_names)))
    if fail_names:
        say(red("Failing examples:"))
        for n in fail_names:
            say(red("  - " + n))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
