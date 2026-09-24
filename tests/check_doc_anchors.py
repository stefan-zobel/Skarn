# check_doc_anchors.py -- validate that every section NAME cited from source and docs still exists.
#
# Source comments and documents cite sections of the architecture documents by name, not by markdown
# anchor:
#     // See "Native functions" in docs/VirtualMachine.md.
#     // GUIDE: docs/Compiler.md "No truthiness"
# Renaming or moving a heading is invisible to every other check, so such a citation would rot silently.
# This script collects every citable target of the document family -- headings AND bold paragraph leads
# (`**Name**`), since documents are often cited by those -- and checks every citation it finds.
#
# What a CITATION is. A quoted name on a line that mentions one of the documents, where a TRIGGER sits just
# before the quote with citation-shaped text between: the document name itself (`Compiler.md "X"`) or a
# continuation word (`see "X"`, `under "X"`, `section "X"`). Anything else in quotes on such a line -- an
# error message, a grep pattern -- is not a citation.
#
# Matching is deliberately generous: names are normalized (lowercase, every run of non-alphanumerics -> one
# space), and match strength scales with the citation's length. ONE word must match a target exactly, TWO
# words may match a prefix, THREE or more may match anywhere inside the target. That lets an ASCII comment
# cite an em-dashed heading, and a short citation name a longer heading, without a bare "Dispatch" resolving
# by accident against an unrelated target.
#
# Usage (any cwd; Python 3.9+):
#   python tests/check_doc_anchors.py
#   python tests/check_doc_anchors.py --docs docs/Compiler.md --trigger "Compiler\.md"
#
# Options (all optional; the defaults check docs/VirtualMachine.md, docs/Compiler.md and
# docs/LanguageServer.md):
#   --docs                 repo-relative paths of the document family (missing files are skipped)
#   --trigger              regex a line must match to be scanned; also the primary citation trigger
#   --extra-dir            an additional directory of *.md files to scan for citations
#   --reject-part-numbers  reject `Part I/II/III` references next to the trigger
#   --label                the name printed in the summary
#
# Exit code: 0 = every citation resolves; 1 = at least one does not (or no document was found).

import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from skarn_testlib import green, red, repo_root, say  # noqa: E402

SCANNED = {".h", ".cpp", ".md", ".skn", ".ebnf", ".vcxproj"}
# Build output, the extension's npm dependencies and hidden directories (.git, .vs, ...) are never scanned.
SKIPPED_DIRS = {"x64", "node_modules", "build"}
CONNECTOR_WORDS = {"s", "part", "under", "section", "in", "of", "the", "and", "see", "i", "ii", "iii"}


def norm(s):
    return re.sub(r"[^0-9a-z]+", " ", s.strip().lower()).strip()


def read_lines(path):
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    return text.replace("\r\n", "\n").replace("\r", "\n").split("\n")


def targets(family):
    """Every citable target: headings and bold paragraph leads, normalized."""
    headings = []
    bold_leads = []
    for f in family:
        in_fence = False
        for ln in read_lines(f):
            if re.match(r"^\s*```", ln):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            m = re.match(r"^#{1,6}\s+(.*)$", ln)
            if m:
                headings.append(norm(m.group(1)))
                continue
            for bm in re.finditer(r"\*\*(.+?)\*\*", ln):
                b = norm(bm.group(1))
                if len(b) >= 3:
                    bold_leads.append(b)
    return headings, bold_leads


def matches_any(names, n, words):
    for h in names:
        if words == 1:
            if h == n:
                return True
        elif words == 2:
            if h.startswith(n):
                return True
        elif n in h:
            return True
    return False


def resolve(cite, headings, bold_leads):
    """'heading', 'bold', or '' (unresolved)."""
    n = norm(cite)
    if not n:
        return "heading"
    words = len(re.findall(r"[0-9a-z]+", n))
    if matches_any(headings, n, words):
        return "heading"
    if matches_any(bold_leads, n, words):
        return "bold"
    return ""


def is_connector(between):
    """The text between a trigger and the opening quote must be citation-shaped: punctuation, a section
    sign, a possessive, a path prefix -- and no ordinary word except the few that really do introduce a
    citation."""
    if len(between) > 30:
        return False
    return all(w in CONNECTOR_WORDS for w in re.findall(r"[a-z]+", between.lower()))


def scanned_files(root, extra_dir):
    files = []
    for dirpath, dirnames, filenames in os.walk(str(root)):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIPPED_DIRS and not d.startswith("."))
        for name in sorted(filenames):
            if os.path.splitext(name)[1].lower() in SCANNED:
                files.append(Path(dirpath) / name)
    if extra_dir and Path(extra_dir).is_dir():
        files += sorted(p for p in Path(extra_dir).iterdir() if p.is_file() and p.suffix == ".md")
    return files


def main():
    ap = argparse.ArgumentParser(description="Check that every cited section name still exists.")
    ap.add_argument("--docs", nargs="+",
                    default=["docs/VirtualMachine.md", "docs/Compiler.md", "docs/LanguageServer.md"])
    ap.add_argument("--trigger", default=r"(VirtualMachine|Compiler|LanguageServer)\.md")
    ap.add_argument("--extra-dir", default="")
    ap.add_argument("--reject-part-numbers", action="store_true")
    ap.add_argument("--label", default="doc anchors")
    opts = ap.parse_args()

    root = repo_root()
    family = [root / d for d in opts.docs if (root / d).is_file()]
    if not family:
        say(red("ERROR: none of the documents ({}) exists under '{}'.".format(", ".join(opts.docs), root)))
        return 1
    headings, bold_leads = targets(family)

    # Whether a line is scanned at all follows the trigger without regard to case; a trigger IN FRONT OF a
    # quote must match exactly, only the continuation words ignore case.
    line_trigger = re.compile(opts.trigger, re.IGNORECASE)
    cite_trigger = re.compile(opts.trigger + r"|(?i:\bunder\b|\bsection\b|\bsee\b)")
    part_before = re.compile("(" + opts.trigger + r")[^.]{0,40}\bPart\s+(I|II|III)\b", re.IGNORECASE)
    part_after = re.compile(r"\bPart\s+(I|II|III)\b[^.]{0,40}(" + opts.trigger + ")", re.IGNORECASE)

    bad = []
    citations = sites = via_bold = 0
    for f in scanned_files(root, opts.extra_dir):
        try:
            where = f.relative_to(root).as_posix()
        except ValueError:
            where = "extra/" + f.name
        for i, ln in enumerate(read_lines(f)):
            if not line_trigger.search(ln):
                continue
            sites += 1
            if opts.reject_part_numbers and (part_before.search(ln) or part_after.search(ln)):
                bad.append("{}:{}  [Part number]  {}".format(where, i + 1, ln.strip()))
            for mm in re.finditer(r'"([^"]{2,80})"', ln):
                cite = mm.group(1)
                if not re.search(r"[A-Za-z]", cite):
                    continue                      # not a name
                if line_trigger.search(cite):
                    continue                      # the quote contains the document name
                head = ln[:mm.start()]
                is_cite = any(tm.end() <= mm.start() and is_connector(head[tm.end():])
                              for tm in cite_trigger.finditer(head))
                if not is_cite:
                    continue
                citations += 1
                kind = resolve(cite, headings, bold_leads)
                if kind == "":
                    bad.append('{}:{}  "{}"'.format(where, i + 1, cite))
                elif kind == "bold":
                    via_bold += 1

    say("{}: {} heading(s) + {} bold lead(s) across {} file(s); {} citing line(s), {} named citation(s), "
        "{} resolved only via a bold lead".format(opts.label, len(headings), len(bold_leads), len(family),
                                                  sites, citations, via_bold))
    if bad:
        say(red("==== BROKEN: {} citation(s) resolve to no heading ====".format(len(bad))))
        for b in sorted(bad):
            say(red("  " + b))
        return 1
    say(green("==== every cited section resolves ===="))
    return 0


if __name__ == "__main__":
    sys.exit(main())
