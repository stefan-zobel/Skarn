# check_highlighters.py -- validate the word lists of the three syntax highlighters.
#
# Skarn has three hand-maintained highlighters: tools/skarn.npp-udl.xml (Notepad++), docs/skarn.tmLanguage.json
# (the TextMate grammar, canonical) and tools/vscode-skarn/syntaxes/skarn.tmLanguage.json (the VS Code
# extension's copy of it). A name missing from them is merely not coloured, so they drift silently with every
# library change. This script checks:
#   1. that they agree: UDL Keywords4 = TextMate builtin-functions, Keywords5 = prelude-types, Keywords3 =
#      builtin-types, each list without duplicates and in order (case-insensitive, ties by case), and the two
#      .tmLanguage.json files byte-identical;
#   2. that they are complete and hold nothing else, against the compiler's own list of public names
#      (`skarnvm --dump-names`): every callable name of the std library -- function, method, builtin, native --
#      is a function, and every type, trait and constant a library type (or a built-in type); in the other
#      direction every function is callable and every library type is one of those, with the exceptions below.
#
# Three rules the lists follow, each a judgement about the highlighters rather than something the source says,
# so they are written down here and not derived:
#   - the natives under the library's wrappers (`raw*`, and `tcp*` in std::net) are never listed: they are not
#     part of the library a program should see, and the user documents never name them either;
#   - a NEW enum's type goes in, its variants do not -- `Debug`, `Stop`, `Full` or `Data` would colour in every
#     program that uses the word. The older variants in LEGACY_VARIANTS predate the rule and stay;
#   - EXTRA_CALLABLE names a callable that the compiler's list cannot show, with the reason.
#
# The TextMate file is PARSED (json), never matched with a regex over the text: the function list is one line
# of several kilobytes, and the rules end differently (a call-site lookahead on the functions).
#
# Usage (any cwd; Python 3.9+):
#   python tests/check_highlighters.py
#   python tests/check_highlighters.py --sync      # copy the canonical grammar over the extension's copy
#   python tests/check_highlighters.py --exe build/skarnvm
#
# Exit code: 0 = the highlighters are consistent and complete; 1 otherwise.

import argparse
import json
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from skarn_testlib import add_driver_options, find_driver, green, red, rel, repo_root, run_driver, say  # noqa: E402

UDL = "tools/skarn.npp-udl.xml"
GRAMMAR = "docs/skarn.tmLanguage.json"                        # canonical
GRAMMAR_COPY = "tools/vscode-skarn/syntaxes/skarn.tmLanguage.json"

# (UDL keyword list, TextMate repository rule, what it holds)
LISTS = (
    ("Keywords4", "builtin-functions", "functions"),
    ("Keywords5", "prelude-types", "library types"),
    ("Keywords3", "builtin-types", "built-in types"),
)

ALTERNATION = re.compile(r"^\\b\(([^()]*)\)")   # `\b(a|b|c)\b...`: the word list of a rule

# The natives under the library's wrappers: never in a highlighter (see the header).
INTERNAL_NATIVE = re.compile(r"^(raw|tcp)[A-Z]")

# Variants listed as library types before the variants rule; frozen -- nothing is added here.
LEGACY_VARIANTS = frozenset(["Arr", "Boolean", "Err", "Integer", "MacOS", "None", "Obj", "Ok", "Other", "Some",
                             "Windows"])

# Callables the compiler's list does not show: name -> reason.
EXTRA_CALLABLE = {
    "peek": "a method of the private PeekableCursor, which the public peekable() returns",
}


def order_key(name):
    return (name.lower(), name)


def last_segment(name):
    return re.split(r"\.|::", name)[-1]


def grammar_lists(path):
    """rule name -> its word list, from the TextMate grammar."""
    repo = json.loads(path.read_text(encoding="utf-8"))["repository"]
    out = {}
    for _, rule, _ in LISTS:
        entry = repo[rule]
        words = []
        for pattern in entry.get("patterns", [entry]):
            m = ALTERNATION.match(pattern.get("match", ""))
            if not m:
                raise ValueError("{}: rule {} has no `\\b(...)` word list".format(rel(path), rule))
            words += m.group(1).split("|")
        out[rule] = words
    return out


def udl_lists(path):
    """keyword list name -> its words, from the Notepad++ UDL."""
    root = ET.parse(str(path)).getroot()
    return {kw.get("name"): (kw.text or "").split() for kw in root.iter("Keywords")}


def check_list(label, words, problems):
    seen = set()
    for w in words:
        if w in seen:
            problems.append("{}: `{}` appears twice".format(label, w))
        seen.add(w)
    for a, b in zip(words, words[1:]):
        if order_key(a) > order_key(b):
            problems.append("{}: `{}` comes before `{}` (the lists are sorted case-insensitively)".format(label, a, b))


def consistency(root, problems):
    """Stage 1. Returns the TextMate lists for stage 2."""
    tm = grammar_lists(root / GRAMMAR)
    udl = udl_lists(root / UDL)
    for kw, rule, what in LISTS:
        words = udl.get(kw, [])
        check_list("{} {}".format(UDL, kw), words, problems)
        check_list("{} {}".format(GRAMMAR, rule), tm[rule], problems)
        for w in sorted(set(words) - set(tm[rule]), key=order_key):
            problems.append("{} `{}` is in {} {} but not in the TextMate {}".format(what, w, UDL, kw, rule))
        for w in sorted(set(tm[rule]) - set(words), key=order_key):
            problems.append("{} `{}` is in the TextMate {} but not in {} {}".format(what, w, rule, UDL, kw))
    if (root / GRAMMAR).read_bytes() != (root / GRAMMAR_COPY).read_bytes():
        problems.append("{} differs from the canonical {} (run with --sync after editing {})".format(
            GRAMMAR_COPY, GRAMMAR, GRAMMAR))
    return tm


def read_names(exe):
    """(module, kind, name) of every public std name, from `skarnvm --dump-names`."""
    r = run_driver(exe, ["--dump-names"], cwd=repo_root(), timeout=60)
    if r.code != 0:
        say(red("ERROR: {} --dump-names exited {}: {}".format(rel(exe), r.code, r.err.strip())))
        sys.exit(1)
    names = []
    for ln in r.out.split("\n"):
        parts = ln.split("\t")
        if len(parts) >= 3:
            names.append((parts[0], parts[1], parts[2]))
    return names


def against_names(tm, names, problems):
    """Stage 2: the TextMate lists (equal to the UDL's once stage 1 passes) against the compiler's names."""
    callable_names = {last_segment(n) for (_, kind, n) in names if kind in ("fn", "method", "ambient")}
    type_names = {n for (_, kind, n) in names if kind in ("type", "trait", "const")}
    variant_names = {last_segment(n) for (_, kind, n) in names if kind == "variant"}
    functions = set(tm["builtin-functions"])
    library_types = set(tm["prelude-types"])
    all_types = library_types | set(tm["builtin-types"])

    for n in sorted(callable_names - functions, key=order_key):
        if not INTERNAL_NATIVE.match(n):
            problems.append("function `{}` is callable but in no highlighter".format(n))
    for n in sorted(functions, key=order_key):
        if INTERNAL_NATIVE.match(n):
            problems.append("function `{}` is an internal native and must not be highlighted".format(n))
        elif n not in callable_names and n not in EXTRA_CALLABLE:
            problems.append("function `{}` is highlighted but callable nowhere".format(n))
    for n in sorted(EXTRA_CALLABLE, key=order_key):
        if n not in functions:
            problems.append("EXTRA_CALLABLE lists `{}`, which no highlighter has".format(n))
    for n in sorted(type_names - all_types, key=order_key):
        problems.append("type `{}` is in no highlighter".format(n))
    for n in sorted(library_types - type_names, key=order_key):
        if n in LEGACY_VARIANTS:
            continue
        if n in variant_names:
            problems.append("`{}` is an enum variant: a new enum's type is highlighted, its variants are not".format(n))
        else:
            problems.append("library type `{}` is highlighted but exists nowhere".format(n))


def main():
    ap = argparse.ArgumentParser(description="Check the word lists of the syntax highlighters.")
    add_driver_options(ap)
    ap.add_argument("--sync", action="store_true",
                    help="copy {} over {} first".format(GRAMMAR, GRAMMAR_COPY))
    opts = ap.parse_args()
    root = repo_root()

    if opts.sync:
        shutil.copyfile(str(root / GRAMMAR), str(root / GRAMMAR_COPY))
        say("synced {} -> {}".format(GRAMMAR, GRAMMAR_COPY))

    problems = []
    tm = consistency(root, problems)
    against_names(tm, read_names(find_driver(opts.exe, opts.config)), problems)

    say("highlighters: {} functions, {} library types, {} built-in types".format(
        len(tm["builtin-functions"]), len(tm["prelude-types"]), len(tm["builtin-types"])))
    for p in problems:
        say(red("  " + p))
    say()
    summary = "==== highlighters: {} problem(s) ====".format(len(problems))
    if problems:
        say(red(summary))
        return 1
    say(green(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
