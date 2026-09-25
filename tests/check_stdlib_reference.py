# check_stdlib_reference.py -- validate that SkarnStdlib.md documents every public name of the std, and
# names nothing that does not exist.
#
# The truth set is the compiler's own: `skarnvm --dump-names` lists every public name of the embedded std
# (functions, methods, types, traits, constants, enum variants, and the builtins and natives with the
# module whose `use` makes each reachable). The reference is written by hand, so without this check a new
# function goes undocumented and a renamed one leaves a stale table row, and nothing notices.
#
# What DOCUMENTED means. SkarnStdlib.md has one numbered section per module: sections 1-7 (Part 1) hold
# the always-available names -- the builtins and natives reachable without a `use`, and the ring modules
# std::core, std::iter and std::string -- and every later section names its module in its heading
# (`## 8. `std::io`: ...`). A name is documented when its module's section says it:
#   - a function, builtin, native or constant appears in the FIRST COLUMN of one of the section's tables;
#     an associated function as `Type::name`, a method as `.name` (the receiver is a variable there);
#   - a trait method appears there as a free call or as `.name` -- both call forms are valid;
#   - a type or trait appears anywhere in the section.
# Enum variants are not required (the reference shows them where they matter) but count in the reverse
# direction below.
#
# The reverse direction: every call-shaped name in a first column -- `name(`, `.name(`, `Type::name(` --
# must exist in the section's module(s). That catches a table row the library has outgrown.
#
# The natives a module wraps -- `raw*` everywhere, and the `tcp*` sockets under std::net's TcpConn and
# TcpListener -- are reachable through the module's `use`, but they are the wrappers' building blocks, not
# part of the library a program should see: they are never documented, and they must not appear in any of
# the user documents (the four guides and the README) at all. The architecture documents under docs/,
# which describe the native layer, are not checked.
#
# A public name left out on purpose goes into UNDOCUMENTED below, with the reason. What the check cannot
# see: two types of one module with a same-named method (a `close` documented for one counts for both),
# and signatures -- only names are compared.
#
# Usage (any cwd; Python 3.9+):
#   python tests/check_stdlib_reference.py
#   python tests/check_stdlib_reference.py --config Debug
#   python tests/check_stdlib_reference.py --exe build/skarnvm
#
# Exit code: 0 = every public name is documented (or allowed) and every documented call exists; 1 otherwise.

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from skarn_testlib import add_driver_options, find_driver, green, red, rel, repo_root, run_driver, say  # noqa: E402

REFERENCE = "SkarnStdlib.md"

# Modules documented in Part 1 (sections 1-7); "-" is the checker's name for "reachable without a use".
PART1_MODULES = ("-", "std::core", "std::iter", "std::string")
PART1_SECTIONS = range(1, 8)

# The natives under the wrappers (see the header): never documented, and banned from USER_DOCUMENTS.
INTERNAL_NATIVE = re.compile(r"^(raw|tcp)[A-Z]")
USER_DOCUMENTS = ("SkarnStdlib.md", "SkarnGuide.md", "SkarnIn30Minutes.md", "SkarnActors.md", "README.md")

# Public names the reference leaves out on purpose: module -> {name: reason}. Names as --dump-names
# prints them (`Type.method`, `Type::assoc`).
UNDOCUMENTED = {
    "-": {
        "f64ToBytes": "a leftover of the removed self-hosting compiler, which needed a Double's bit pattern; "
                      "nothing uses it any more",
    },
}

SECTION = re.compile(r"^##\s+(\d+)\.\s+(.*)$")
MODULE_IN_HEADING = re.compile(r"`(std::[a-z_]+)`")
FENCE = re.compile(r"^\s*```")
BACKTICKS = re.compile(r"`([^`]+)`")
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
FREE_CALL = re.compile(r"(?<![\w.:])([a-z_][A-Za-z0-9_]*)\s*\(")
METHOD_CALL = re.compile(r"\.([a-z_][A-Za-z0-9_]*)\s*\(")
PATH_CALL = re.compile(r"\b([A-Z][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)\s*[({]")


class Section:
    def __init__(self, number, title, line):
        self.number = number
        self.title = title
        self.line = line
        self.text = []          # every line of the section
        self.first_cells = []   # (line number, the backticked code of a table row's first cell)


def read_sections(path):
    sections = []
    current = None
    in_fence = False
    for n, ln in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if FENCE.match(ln):
            in_fence = not in_fence
        m = None if in_fence else SECTION.match(ln)
        if m:
            current = Section(int(m.group(1)), m.group(2), n)
            sections.append(current)
            continue
        if ln.startswith("## "):   # an unnumbered section (a Part heading) ends the current one
            current = None
            continue
        if current is None:
            continue
        current.text.append(ln)
        if not in_fence and ln.startswith("|") and not ln.startswith("|--") and not ln.startswith("| Function"):
            cells = ln.split(" | ")
            first = cells[0].lstrip("|").strip()
            code = " ".join(BACKTICKS.findall(first))
            if code:
                current.first_cells.append((n, code))
    return sections


def read_names(exe):
    r = run_driver(exe, ["--dump-names"], cwd=repo_root(), timeout=60)
    if r.code != 0:
        say(red("ERROR: {} --dump-names exited {}: {}".format(rel(exe), r.code, r.err.strip())))
        sys.exit(1)
    names = []   # (module, kind, name)
    for ln in r.out.split("\n"):
        parts = ln.split("\t")
        if len(parts) >= 3:
            names.append((parts[0], parts[1], parts[2]))
    return names


def documented(kind, name, sec_code, sec_words, sec_text_words, traits):
    """Whether `name` (of `kind`) is documented by a section with these first-cell code strings / words."""
    if kind in ("type", "trait"):
        return name in sec_text_words
    if kind in ("fn", "ambient", "const"):
        if "::" in name:                                   # an associated function: Type::name
            pattern = re.compile(r"\b" + re.escape(name) + r"\b")
            return any(pattern.search(code) for code in sec_code)
        return name in sec_words
    if kind == "method":
        owner, member = name.split(".", 1)
        if owner in traits:                                # a trait method: `f(x)` or `x.f()`
            return member in sec_words
        pattern = re.compile(r"\." + re.escape(member) + r"\b")   # `.nextInt` is not `.nextInt48`
        return any(pattern.search(code) for code in sec_code)
    return True                                            # variants are not required


def main():
    ap = argparse.ArgumentParser(description="Check that SkarnStdlib.md documents every public std name.")
    add_driver_options(ap)
    opts = ap.parse_args()
    exe = find_driver(opts.exe, opts.config)
    root = repo_root()

    names = read_names(exe)
    sections = read_sections(root / REFERENCE)
    traits = {n for (_, k, n) in names if k == "trait"}

    # Which sections document which module.
    module_sections = {m: [s for s in sections if s.number in PART1_SECTIONS] for m in PART1_MODULES}
    problems = []
    for s in sections:
        if s.number in PART1_SECTIONS:
            continue
        m = MODULE_IN_HEADING.search(s.title)
        if not m:
            problems.append("line {}: section {} names no `std::` module in its heading".format(s.line, s.number))
            continue
        module_sections.setdefault(m.group(1), []).append(s)

    modules = sorted({m for (m, _, _) in names})
    for m in modules:
        if m not in module_sections:
            problems.append("module {} has no section".format(m))
    known = set(modules)
    for m in module_sections:
        if m not in known:
            problems.append("a section documents {}, which the std does not have".format(m))

    # The internal natives: exempt from documentation, and absent from every user document.
    internal = sorted({n for (_, k, n) in names if k == "ambient" and INTERNAL_NATIVE.match(n)})
    if internal:
        banned = re.compile(r"\b(" + "|".join(re.escape(n) for n in internal) + r")\b")
        for doc in USER_DOCUMENTS:
            path = root / doc
            if not path.is_file():
                continue
            for n, ln in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
                for m in banned.finditer(ln):
                    problems.append("{}:{}: `{}` is an internal native and must not appear in a user document"
                                    .format(doc, n, m.group(1)))

    # Forward: every public name documented in its module's section.
    missing = {}
    allowed = 0
    for (mod, kind, name) in names:
        if kind == "ambient" and INTERNAL_NATIVE.match(name):
            continue
        secs = module_sections.get(mod, [])
        code = [c for s in secs for (_, c) in s.first_cells]
        words = {w for c in code for w in WORD.findall(c)}
        text_words = {w for s in secs for ln in s.text for w in WORD.findall(ln)}
        if documented(kind, name, code, words, text_words, traits):
            continue
        if name in UNDOCUMENTED.get(mod, {}):
            allowed += 1
            continue
        missing.setdefault(mod, []).append("{} {}".format(kind, name))
    for mod, reasons in UNDOCUMENTED.items():
        for name in reasons:
            if (mod, name) not in {(m, n) for (m, _, n) in names}:
                problems.append("UNDOCUMENTED lists {} {}, which the std does not have".format(mod, name))

    # Reverse: every call-shaped name in a first cell exists in the section's module(s).
    by_module = {}
    for (mod, kind, name) in names:
        by_module.setdefault(mod, []).append((kind, name))
    unknown = []
    for s in sections:
        mods = PART1_MODULES if s.number in PART1_SECTIONS else \
            [m.group(1) for m in [MODULE_IN_HEADING.search(s.title)] if m]
        own = [kn for m in mods for kn in by_module.get(m, [])]
        free = {n for (k, n) in own if k in ("fn", "ambient") and "::" not in n}
        free |= {n.split(".", 1)[1] for (k, n) in own if k == "method" and n.split(".", 1)[0] in traits}
        members = {n.split(".", 1)[1] for (k, n) in own if k == "method"}
        paths = {n for (k, n) in own if k in ("fn", "variant") and "::" in n}
        for (line, code) in s.first_cells:
            for m in FREE_CALL.finditer(code):
                if m.group(1) not in free:
                    unknown.append("line {}: `{}(` is no function of {}".format(line, m.group(1), ", ".join(mods)))
            for m in METHOD_CALL.finditer(code):
                if m.group(1) not in members:
                    unknown.append("line {}: `.{}(` is no method of {}".format(line, m.group(1), ", ".join(mods)))
            for m in PATH_CALL.finditer(code):
                path = "{}::{}".format(m.group(1), m.group(2))
                if path not in paths:
                    unknown.append("line {}: `{}` is no function or variant of {}".format(line, path, ", ".join(mods)))

    total = len(names)
    n_missing = sum(len(v) for v in missing.values())
    say("stdlib reference: {} public names in {} modules, {} sections in {}".format(
        total, len(modules), len(sections), REFERENCE))
    for mod in sorted(missing):
        say(red("  {}: {} undocumented".format(mod, len(missing[mod]))))
        for entry in missing[mod]:
            say(red("    " + entry))
    for u in unknown:
        say(red("  " + u))
    for p in problems:
        say(red("  " + p))
    say()
    summary = ("==== stdlib reference: {} documented, {} internal natives, {} allowed undocumented, {} missing, "
               "{} unknown ====").format(total - n_missing - allowed - len(internal), len(internal), allowed,
                                         n_missing, len(unknown) + len(problems))
    if n_missing or unknown or problems:
        say(red(summary))
        return 1
    say(green(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
