# run_demos.py -- type-checks every demo program and runs the demos' self-tests.
#
# The demos under demo/ are larger programs than the examples and have no fixed output: a server waits
# for clients, a benchmark prints timings, the raytracer writes a picture. So this runner checks two
# weaker things than tests/run_examples.py does:
#   1. every ENTRY program type-checks (`--dump-ast`, exit 0, no `error:`). An entry program is a .skn
#      file no other .skn file under demo/ imports; the imported ones are checked as its modules.
#   2. the demos that check themselves run and report success (the SELFTESTS table below): each exits 0
#      and ends with its success line; on a mismatch it panics instead.
#
# Usage (repo root or anywhere; Python 3.9+):
#   python tests/run_demos.py
#   python tests/run_demos.py --config Debug
#   python tests/run_demos.py --exe build/skarnvm
#
# Exit code: 0 = every demo type-checks and every self-test passed; 1 otherwise.

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from skarn_testlib import add_driver_options, find_driver, green, red, rel, repo_root, run_driver, say  # noqa: E402

# (entry program, arguments, a pattern the last line of standard output must match)
SELFTESTS = [
    ("demo/regex.skn", [], r"^all regex demo checks passed$"),
    ("demo/const_crc_table.skn", [], r"^crc32 OK \(table-driven, via a const array\)$"),
    ("demo/aes256.skn", [], r"^AES-256 demo: all known-answer tests passed\.$"),
    ("demo/chat/server.skn", ["--selftest"], r"^selftest: \d+ steps ok$"),
]

IMPORT = re.compile(r"^\s*import\s+([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)")


def entry_programs(demo_dir):
    """Every .skn under demo_dir that no other one imports. An import resolves next to the importing file,
    `import net::http` to net/http.skn."""
    files = sorted(demo_dir.rglob("*.skn"), key=lambda p: str(p).lower())
    imported = set()
    for f in files:
        for ln in f.read_text(encoding="utf-8-sig").splitlines():
            m = IMPORT.match(ln)
            if m:
                imported.add((f.parent / (m.group(1).replace("::", "/") + ".skn")).resolve())
    return [f for f in files if f.resolve() not in imported]


def main():
    ap = argparse.ArgumentParser(description="Type-check every demo program and run the demos' self-tests.")
    add_driver_options(ap)
    opts = ap.parse_args()
    exe = find_driver(opts.exe, opts.config)
    root = repo_root()

    passed = 0
    fail_names = []

    entries = entry_programs(root / "demo")
    for f in entries:
        r = run_driver(exe, ["--dump-ast", f], cwd=f.parent, timeout=opts.timeout)
        errors = [ln for ln in r.err.split("\n") if ln.startswith("error:")]
        if r.code == 0 and not errors:
            passed += 1
            say(green("PASS  {}  [check]".format(rel(f))))
        else:
            fail_names.append(rel(f))
            first = errors[0] if errors else (r.err.strip().split("\n")[0] if r.err.strip() else "")
            say(red("FAIL  {}  [check]  exit {}: {}".format(rel(f), r.code, first)))

    for path, args, pattern in SELFTESTS:
        f = root / path
        r = run_driver(exe, [f] + args, cwd=f.parent, timeout=opts.timeout)
        lines = [ln for ln in r.out.split("\n") if ln.strip()]
        last = lines[-1] if lines else "<no output>"
        label = " ".join([path] + args)
        if r.code == 0 and re.match(pattern, last):
            passed += 1
            say(green("PASS  {}  [selftest]  {}".format(label, last)))
        else:
            fail_names.append(label)
            why = r.err.strip() if r.timed_out else "exit {}, last line '{}'".format(r.code, last)
            say(red("FAIL  {}  [selftest]  {}".format(label, why)))
            if r.err.strip() and not r.timed_out:
                say(red("        " + r.err.strip().split("\n")[0]))

    say()
    say("==== demos: {} passed, {} failed  ({} entry programs, {} self-test(s)) ====".format(
        passed, len(fail_names), len(entries), len(SELFTESTS)))
    if fail_names:
        say(red("Failing demos:"))
        for n in fail_names:
            say(red("  - " + n))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
