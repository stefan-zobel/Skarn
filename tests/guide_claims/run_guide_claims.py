# run_guide_claims.py -- the executable guide-claim checker.
#
# Each *.skn under this directory is a self-contained Skarn program that encodes ONE
# categorical claim one of the guides makes -- SkarnGuide.md and SkarnActors.md (tier1/2/3) or
# SkarnIn30Minutes.md (intro/). A header comment declares the expected outcome; this runner
# compiles+runs each program with the driver and asserts the outcome matches. The point:
# a guide claim that silently drifts from the compiler's actual behaviour fails here instead
# of misleading a reader.
#
# Directive (a line inside the .skn, so the file still compiles):
#   // EXPECT: ok   <exact-stdout>     -- exit 0, trimmed stdout equals <exact-stdout>
#   // EXPECT: warn <stderr-substring> -- exit 0, stderr CONTAINS <stderr-substring>
#   // EXPECT: fail <stderr-substring> -- exit != 0, stderr CONTAINS <stderr-substring>
#   // EXPECT: exit <n> <exact-stdout> -- exit code n (std::process's exit), trimmed stdout equals
#                                        <exact-stdout>, and no error message on stderr
# (// CLAIM:/// GUIDE: lines are documentation only.)
#
# Usage (repo root or anywhere; Python 3.9+):
#   python tests/guide_claims/run_guide_claims.py
#   python tests/guide_claims/run_guide_claims.py --config Debug
#   python tests/guide_claims/run_guide_claims.py --exe build/skarnvm
#
# After the claims it runs run_guide_examples.py (every ```rust block of the guides), then
# tests/run_examples.py (every program under examples/, exact output), then check_anchors.py over all
# guides (every link to a heading resolves), then tests/check_stdlib_reference.py (SkarnStdlib.md names
# every public std name) and tests/check_highlighters.py (the three syntax highlighters agree and list every
# callable name and type), so one command is the whole doc gate.
# --no-examples skips all five, and so does a --dir subset.
#
# Exit code: 0 = all claims (and examples) held; 1 = at least one drifted (or a fixture was malformed).

import argparse
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR.parent))
from skarn_testlib import add_driver_options, find_driver, green, red, rel, run_driver, say  # noqa: E402

DIRECTIVE = re.compile(r"^\s*//\s*EXPECT:\s*(ok|warn|fail|exit)\b\s?(.*)$")


def check_claim(exe, path, verb, expected, timeout):
    """Runs one claim; returns '' when it held, otherwise why not."""
    r = run_driver(exe, [path], timeout=timeout)
    if r.timed_out:
        return r.err
    if verb == "ok":
        if r.code != 0:
            return "expected exit 0, got {}; stderr: {}".format(r.code, r.err.strip())
        if r.out.strip() != expected:
            return "stdout '{}' != expected '{}'".format(r.out.strip(), expected)
    elif verb == "exit":
        code, _, text = expected.partition(" ")
        if r.code != int(code):
            return "expected exit {}, got {}; stderr: {}".format(code, r.code, r.err.strip())
        if r.out.strip() != text.strip():
            return "stdout '{}' != expected '{}'".format(r.out.strip(), text.strip())
        if "error:" in r.err:
            return "an error message on stderr: {}".format(r.err.strip())
    elif verb == "warn":
        if r.code != 0:
            return "expected exit 0 (warn), got {}".format(r.code)
        if expected not in r.err:
            return "stderr missing warning substring '{}'".format(expected)
    else:
        if r.code == 0:
            return "expected a rejection, but it compiled+ran (exit 0)"
        if expected not in r.err:
            return "stderr missing substring '{}'; got: {}".format(expected, r.err.strip())
    return ""


def run_script(script, exe):
    """Runs another runner with this interpreter; True when it passed."""
    return subprocess.run([sys.executable, str(script), "--exe", str(exe)]).returncode == 0


def main():
    ap = argparse.ArgumentParser(description="Run the guide claims, then the rest of the doc gate.")
    add_driver_options(ap)
    ap.add_argument("--dir", default="", help="run only the claims under this directory (skips the rest)")
    ap.add_argument("--no-examples", action="store_true",
                    help="skip the guide examples, examples/, the anchor check, the stdlib reference check "
                         "and the highlighter check")
    opts = ap.parse_args()

    run_rest = not opts.no_examples and not opts.dir
    claim_dir = Path(opts.dir) if opts.dir else SCRIPT_DIR
    exe = find_driver(opts.exe, opts.config)

    files = sorted(claim_dir.rglob("*.skn"), key=lambda p: str(p).lower())
    if not files:
        say(red("ERROR: no *.skn claim files found under '{}'.".format(claim_dir)))
        return 1

    passed = 0
    skipped = 0
    fail_names = []
    for f in files:
        directive = None
        for ln in f.read_text(encoding="utf-8-sig").splitlines():
            directive = DIRECTIVE.match(ln)
            if directive:
                break
        if directive is None:
            # A file with no EXPECT directive is a SUPPORT MODULE (imported by a multi-file
            # claim's entry, e.g. Tier 3), not a claim. It is never run on its own.
            skipped += 1
            continue
        verb = directive.group(1)
        why = check_claim(exe, f, verb, directive.group(2).strip(), opts.timeout)
        if not why:
            passed += 1
            say(green("PASS  {}  [{}]".format(rel(f), verb)))
        else:
            fail_names.append(rel(f))
            say(red("FAIL  {}  [{}]  {}".format(rel(f), verb, why)))

    say()
    skip_note = "  ({} support module(s) skipped)".format(skipped) if skipped else ""
    say("==== guide claims: {} passed, {} failed  ({} claims){} ====".format(
        passed, len(fail_names), passed + len(fail_names), skip_note))
    if fail_names:
        say(red("Drifted claims:"))
        for n in fail_names:
            say(red("  - " + n))

    rest_failed = False
    if run_rest:
        # The guides' own internal links run here too: a check that stands alone is easy to forget, and
        # a renumbering of the sections once left six table-of-contents links pointing at headings that
        # no longer existed while every routine gate stayed green.
        for script in (SCRIPT_DIR / "run_guide_examples.py", SCRIPT_DIR.parent / "run_examples.py"):
            say()
            rest_failed = not run_script(script, exe) or rest_failed
        say()
        rest_failed = subprocess.run([sys.executable, str(SCRIPT_DIR / "check_anchors.py")]).returncode != 0 \
            or rest_failed
        say()
        rest_failed = not run_script(SCRIPT_DIR.parent / "check_stdlib_reference.py", exe) or rest_failed
        say()
        rest_failed = not run_script(SCRIPT_DIR.parent / "check_highlighters.py", exe) or rest_failed
    return 1 if fail_names or rest_failed else 0


if __name__ == "__main__":
    sys.exit(main())
