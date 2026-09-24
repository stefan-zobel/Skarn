# run_guide_examples.py -- every ```rust block of the guides, extracted and run.
#
# The guides are the single source of truth: this runner reads SkarnGuide.md, SkarnIn30Minutes.md and
# SkarnActors.md at run time, turns every ```rust fence into a program, runs it with the driver, and checks
# it against the annotations the reader sees. Nothing is copied into the repository, so an example cannot
# drift from its test. (The hand-written claim fixtures next to this script are run by run_guide_claims.py.)
#
# Fence info string -- ```rust followed by optional space-separated markers:
#   (none)          run it; exit 0; checked as described below
#   group=<name>    blocks of one guide with the same group are concatenated, in document order, into ONE
#                   program (a later example that continues an earlier one)
#   file=<path>     (needs group=) this block is not part of the program text but the module file <path>
#                   beside it, e.g. file=geo.skn for `import geo`
#   fail            the program must be rejected; see `// error:` below
#   check           type-check only (--dump-ast), never run -- for examples that need the network or
#                   another external resource
#   ignore=<reason> not a program (a fragment); reported, never run. <reason> has no spaces.
# An unknown marker, a group without a program block, or `fail` inside a group is a malformed fence.
#
# Annotations inside a block:
#   // => <line>        one expected stdout line. A block with at least one `// =>` must print EXACTLY
#                       those lines, in order. The annotation may stand alone on its own line (for a second
#                       output line of one statement) or follow code. Text after two or more spaces and a
#                       `(` is a note, not output: `// => 3   (integer division)`.
#   // error: <text>    (after code, `fail` blocks only) the checker must report an error whose caret is on
#                       THIS line and whose message contains <text> up to the first ` -- `, em dash or ` (`.
#                       Every error it reports must be annotated this way.
#   // warning: <text>  the same for a warning. Every warning a block produces must be annotated.
# A comment line that starts with `//` is commented-out code: annotations in it are not read (except a
# standalone `// =>` line).
#
# Usage (repo root or anywhere; Python 3.9+):
#   python tests/guide_claims/run_guide_examples.py
#   ... --config Debug | --exe <driver> | --guide <file.md> [<file.md> ...]
#   ... --only <line>       run only the program containing the fence at (or the block around) that line
#   ... --emit <dir>        also write every extracted program to <dir> (for debugging; not committed)
#
# Exit code: 0 = every example held; 1 = at least one failed or a fence was malformed.

import argparse
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from skarn_testlib import add_driver_options, find_driver, green, red, repo_root, run_driver, say, yellow  # noqa: E402

GUIDES = ("SkarnGuide.md", "SkarnIn30Minutes.md", "SkarnActors.md")
EM_DASH = "—"


def read_lines(path):
    """The file's lines, split on LF or CRLF, without a trailing empty line."""
    text = path.read_text(encoding="utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


# ---------------------------------------------------------------------------------------------------------
# Extraction

class Block:
    def __init__(self, line, body, markers, problem):
        self.line = line              # the fence line (1-based)
        self.body_start = line + 1    # the guide line of body[0]
        self.body = body
        self.markers = markers
        self.problem = problem        # a malformed-fence message, or None


def guide_blocks(path):
    """The ```rust blocks of one markdown file."""
    lines = read_lines(path)
    blocks = []
    i = 0
    while i < len(lines):
        m = re.match(r"^```(\S*)(.*)$", lines[i])
        if not m:
            i += 1
            continue
        lang = m.group(1)
        fence_line = i + 1
        j = i + 1
        while j < len(lines) and not re.match(r"^```\s*$", lines[j]):
            j += 1
        if lang == "rust":
            markers = {}
            problem = None
            for tok in m.group(2).strip().split():
                key, sep, val = tok.partition("=")
                val = val if sep else None
                if key in ("fail", "check"):
                    if val is not None:
                        problem = "marker '{}' takes no value".format(key)
                elif key in ("group", "file", "ignore"):
                    if not val:
                        problem = "marker '{0}' needs a value ({0}=...)".format(key)
                else:
                    problem = "unknown marker '{}'".format(tok)
                markers[key] = val
            blocks.append(Block(fence_line, lines[i + 1:j], markers, problem))
        i = j + 1
    return blocks


class Program:
    def __init__(self, group, line):
        self.blocks = []    # the concatenated main text, in order
        self.files = []     # file= blocks
        self.group = group
        self.line = line
        self.problem = None
        self.mode = "run"   # run / fail / check / ignore
        self.reason = None


def guide_programs(blocks):
    """Groups blocks into programs."""
    programs = []
    groups = {}
    for b in blocks:
        g = b.markers.get("group")
        if g is None:
            if "file" in b.markers and b.problem is None:
                b.problem = "marker 'file=' needs 'group='"
            p = Program(None, b.line)
            p.blocks.append(b)
            programs.append(p)
        else:
            if g not in groups:
                groups[g] = Program(g, b.line)
                programs.append(groups[g])
            (groups[g].files if "file" in b.markers else groups[g].blocks).append(b)
    for p in programs:
        every = p.blocks + p.files
        bad = next((b for b in every if b.problem is not None), None)
        if bad is not None:
            p.problem = "fence at line {}: {}".format(bad.line, bad.problem)
        for b in every:
            if "ignore" in b.markers:
                p.mode = "ignore"
                p.reason = b.markers["ignore"]
            elif "check" in b.markers and p.mode != "ignore":
                p.mode = "check"
            elif "fail" in b.markers and p.mode == "run":
                p.mode = "fail"
        if p.problem is None:
            if not p.blocks:
                p.problem = "group '{}' has only file= blocks, no program".format(p.group)
            elif p.group is not None and p.mode == "fail":
                p.problem = "group '{}': 'fail' is not supported inside a group".format(p.group)
    return programs


# ---------------------------------------------------------------------------------------------------------
# Annotations

RE_OUTPUT = re.compile(r"^(?P<code>.*?)//\s*=>\s?(?P<text>.*)$")
RE_DIAG = re.compile(r"^(?P<code>.*?)//\s*(?P<kind>error|warning):\s*(?P<text>.*)$")


def commented_out(code):
    return code.lstrip().startswith("//")


def note_stripped(text):
    return re.sub(r"\s{2,}\(.*$", "", text).strip()


def diag_key(text):
    t = text
    for sep in (" -- ", " " + EM_DASH, " ("):
        k = t.find(sep)
        if k >= 0:
            t = t[:k]
    return t.strip().rstrip("-").strip()


class Diagnostic:
    def __init__(self, kind, message):
        self.kind = kind
        self.message = message
        self.line = 0       # the first caret excerpt's line, 0 if none


def diagnostics(stderr):
    """The `error:` / `warning:` diagnostics in the driver's standard error."""
    diags = []
    cur = None
    for ln in stderr.split("\n"):
        h = re.match(r"^(error|warning):\s*(.*)$", ln)
        if h:
            cur = Diagnostic(h.group(1), h.group(2))
            diags.append(cur)
            continue
        if cur is not None and cur.line == 0:
            x = re.match(r"^\s*(\d+) \| ", ln)
            if x:
                cur.line = int(x.group(1))
    return diags


class WantedDiagnostic:
    def __init__(self, kind, key, line, guide_line):
        self.kind = kind
        self.key = key
        self.line = line
        self.guide_line = guide_line
        self.matched = False


# ---------------------------------------------------------------------------------------------------------
# Running

def write_program(prog, directory):
    """Writes main.skn and the module files; returns the program's lines and, per line, its guide line."""
    directory.mkdir(parents=True, exist_ok=True)
    text = []
    guide_line = []
    for b in prog.blocks:
        for k, ln in enumerate(b.body):
            text.append(ln)
            guide_line.append(b.body_start + k)
    with open(directory / "main.skn", "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(text) + "\n")
    for fb in prog.files:
        target = directory / fb.markers["file"]
        target.parent.mkdir(parents=True, exist_ok=True)
        with open(target, "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(fb.body) + "\n")
    return text, guide_line


def test_program(exe, prog, directory, timeout):
    """Checks one program; returns the reasons it failed (empty = passed)."""
    why = []
    lines, guide_line = write_program(prog, directory)
    for fb in prog.files:
        for ln in fb.body:
            o = RE_OUTPUT.match(ln)
            if o and not commented_out(o.group("code")):
                why.append("module file '{}' carries a '// =>' annotation; output belongs to the program block"
                           .format(fb.markers["file"]))
                break

    expected = []
    want = []
    for k, ln in enumerate(lines):
        o = RE_OUTPUT.match(ln)
        if o and (o.group("code").strip() == "" or not commented_out(o.group("code"))):
            expected.append(note_stripped(o.group("text")))
        d = RE_DIAG.match(ln)
        if d and d.group("code").strip() != "" and not commented_out(d.group("code")):
            want.append(WantedDiagnostic(d.group("kind"), diag_key(d.group("text")), k + 1, guide_line[k]))

    want_errors = [w for w in want if w.kind == "error"]
    if prog.mode == "fail":
        if not want_errors:
            why.append("marked 'fail' but no line carries a '// error:' annotation")
        if expected:
            why.append("a 'fail' block must not carry '// =>' annotations")
    elif want_errors:
        why.append("line {} carries '// error:' but the fence is not marked 'fail'".format(want_errors[0].guide_line))
    if why:
        return why

    driver_args = ["--dump-ast", "main.skn"] if prog.mode == "check" else ["main.skn"]
    r = run_driver(exe, driver_args, cwd=directory, timeout=timeout)
    if r.timed_out:
        return [r.err]

    diags = diagnostics(r.err)
    for dg in diags:
        hit = next((w for w in want if not w.matched and w.kind == dg.kind and w.line == dg.line
                    and w.key in dg.message), None)
        if hit is not None:
            hit.matched = True
            continue
        at = "line {}".format(guide_line[dg.line - 1]) if 0 < dg.line <= len(guide_line) else "no line"
        why.append("unannotated {} ({}): {}".format(dg.kind, at, dg.message))
    for w in want:
        if not w.matched:
            why.append("line {}: expected {} '{}' on this line, none reported".format(w.guide_line, w.kind, w.key))

    if prog.mode == "fail":
        if r.code == 0:
            why.append("expected a compile rejection, but the program ran (exit 0)")
        return why
    if r.code != 0:
        if not any(dg.kind == "error" for dg in diags):
            why.append("exit {}: {}".format(r.code, " ".join(r.err.strip().split("\n")[:3])))
        return why
    if prog.mode == "check" or not expected:
        return why

    actual = [ln.strip() for ln in r.out.split("\n")]
    while actual and actual[-1] == "":
        actual.pop()
    for k in range(max(len(expected), len(actual))):
        e = expected[k] if k < len(expected) else None
        a = actual[k] if k < len(actual) else None
        if e != a:
            if e is None:
                why.append("output line {} '{}' has no '// =>' annotation".format(k + 1, a))
            elif a is None:
                why.append("expected output line {} '{}' was not printed".format(k + 1, e))
            else:
                why.append("output line {}: expected '{}', got '{}'".format(k + 1, e, a))
            break
    return why


# ---------------------------------------------------------------------------------------------------------
# Main

def main():
    ap = argparse.ArgumentParser(description="Extract every ```rust block of the guides and run it.")
    add_driver_options(ap)
    ap.add_argument("--guide", nargs="+", default=[], help="markdown files (default: the three guides)")
    ap.add_argument("--only", type=int, default=0, help="run only the program containing this guide line")
    ap.add_argument("--emit", default="", help="also write every extracted program to this directory")
    opts = ap.parse_args()

    exe = find_driver(opts.exe, opts.config)
    guides = [Path(g) for g in opts.guide] or [repo_root() / g for g in GUIDES]

    passed = checked = ignored = 0
    fail_names = []
    ignored_names = []
    work_root = Path(tempfile.mkdtemp(prefix="skarn_guide_examples_"))
    try:
        for guide in guides:
            guide = guide.resolve()
            for p in guide_programs(guide_blocks(guide)):
                every = p.blocks + p.files
                if opts.only > 0 and not any(b.line <= opts.only <= b.body_start + len(b.body) for b in every):
                    continue
                label = "{}:{}".format(guide.name, "+".join(str(n) for n in sorted(b.line for b in every)))
                if p.group is not None:
                    label += " [group={}]".format(p.group)

                if p.problem is not None:
                    fail_names.append(label)
                    say(red("FAIL  {}  malformed: {}".format(label, p.problem)))
                    continue
                if p.mode == "ignore":
                    ignored += 1
                    ignored_names.append("{} ({})".format(label, p.reason))
                    say(yellow("SKIP  {}  ignore={}".format(label, p.reason)))
                    continue

                dir_name = "{}_L{:04d}".format(guide.stem, p.line)
                directory = work_root / dir_name
                why = test_program(exe, p, directory, opts.timeout)
                if opts.emit:
                    shutil.copytree(str(directory), str(Path(opts.emit) / dir_name), dirs_exist_ok=True)
                if not why:
                    if p.mode == "check":
                        checked += 1
                    else:
                        passed += 1
                    say(green("PASS  {}  [{}]".format(label, p.mode)))
                else:
                    fail_names.append(label)
                    say(red("FAIL  {}  [{}]".format(label, p.mode)))
                    for w in why:
                        say(red("        " + w))
    finally:
        shutil.rmtree(str(work_root), ignore_errors=True)

    failed = len(fail_names)
    say()
    say("==== guide examples: {} passed, {} check-only, {} ignored, {} failed ====".format(
        passed, checked, ignored, failed))
    for n in ignored_names:
        say(yellow("  ignored: " + n))
    if passed + checked + ignored + failed == 0:
        what = "no ```rust block contains line {}".format(opts.only) if opts.only > 0 else "no ```rust blocks found"
        say(red("ERROR: {}.".format(what)))
        return 1
    if fail_names:
        say(red("Failed examples:"))
        for n in fail_names:
            say(red("  - " + n))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
