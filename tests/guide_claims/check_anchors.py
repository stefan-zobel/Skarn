# check_anchors.py -- validate that every internal markdown anchor link resolves to a heading.
#
# The guides cross-reference themselves with `[text](#slug)` links. When a heading is renamed but a link
# is not, the link silently 404s a reader -- a blind spot no claim fixture can cover. This script
# mechanically checks every internal anchor: each `](#slug)` target must equal the GitHub-style slug of
# some heading in the same file. CI-gateable.
#
# Slug rule (GitHub): lowercase, drop every char that is not a letter/digit/space/hyphen, spaces -> hyphens;
# a duplicate slug is disambiguated with -1, -2, ... (the first occurrence stays bare). Fenced code blocks are
# skipped for BOTH headings and links, so a shell `# comment` inside ``` isn't mistaken for a heading (which
# would mask a genuinely broken link) and a `](#x)` shown as example code isn't checked as a real link.
#
# Usage (any cwd; Python 3.9+):
#   python tests/guide_claims/check_anchors.py                    # the three guides
#   python tests/guide_claims/check_anchors.py path/to/doc.md ...
#
# Exit code: 0 = every internal anchor resolves; 1 = at least one is broken (or a file is missing).

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from skarn_testlib import green, red, rel, repo_root, say  # noqa: E402

GUIDES = ("SkarnGuide.md", "SkarnIn30Minutes.md", "SkarnActors.md")

FENCE = re.compile(r"^\s*```")
HEADING = re.compile(r"^#{1,6}\s+(.*)$")
LINK = re.compile(r"\]\(#([^)]+)\)")


def slug(heading):
    s = heading.strip().lower()
    s = re.sub(r"[^0-9a-z \-]", "", s)  # keep alnum, space, hyphen
    return s.replace(" ", "-")


def outside_fences(lines):
    in_fence = False
    for ln in lines:
        if FENCE.match(ln):
            in_fence = not in_fence
            continue
        if not in_fence:
            yield ln


def check(path):
    """Checks one file; returns True when every internal anchor resolves."""
    if not path.is_file():
        say(red("ERROR: markdown file not found at '{}'.".format(path)))
        return False
    lines = path.read_text(encoding="utf-8-sig").splitlines()

    # Pass 1: heading slugs, with duplicate disambiguation.
    slugs = set()
    heading_count = 0
    for ln in outside_fences(lines):
        m = HEADING.match(ln)
        if not m:
            continue
        heading_count += 1
        s = slug(m.group(1))
        if s not in slugs:
            slugs.add(s)
        else:
            n = 1
            while "{}-{}".format(s, n) in slugs:
                n += 1
            slugs.add("{}-{}".format(s, n))

    # Pass 2: internal link targets.
    targets = [m.group(1) for ln in outside_fences(lines) for m in LINK.finditer(ln)]
    distinct = list(dict.fromkeys(targets))
    broken = [t for t in distinct if t not in slugs]

    say("anchors: {} headings, {} internal links ({} distinct targets) in {}".format(
        heading_count, len(targets), len(distinct), rel(path)))
    if broken:
        say(red("==== BROKEN: {} anchor(s) resolve to no heading ====".format(len(broken))))
        for b in sorted(broken):
            needle = "](#{})".format(b)
            hit = next((i + 1 for i, ln in enumerate(lines) if needle in ln), None)
            where = "first at line {}".format(hit) if hit else "location unknown"
            say(red("  #{}   ({})".format(b, where)))
        return False
    say(green("==== all internal anchors resolve ===="))
    return True


def main():
    ap = argparse.ArgumentParser(description="Check that every internal markdown anchor resolves.")
    ap.add_argument("files", nargs="*", help="markdown files (default: the three guides)")
    opts = ap.parse_args()
    files = [Path(f) for f in opts.files] or [repo_root() / g for g in GUIDES]
    ok = True
    for i, f in enumerate(files):
        if i > 0:
            say()
        ok = check(f) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
