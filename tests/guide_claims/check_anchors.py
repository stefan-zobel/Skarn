# check_anchors.py -- validate that every markdown anchor link resolves to a heading.
#
# The guides cross-reference themselves with `[text](#slug)` links, and each other with
# `[text](SkarnGuide.md#slug)` links. When a heading is renamed but a link is not, the link silently 404s a
# reader -- a blind spot no claim fixture can cover. This script mechanically checks both kinds: each
# `](#slug)` target must equal the GitHub-style slug of some heading in the same file, and each
# `](Name.md#slug)` target the slug of some heading in the file `Name.md` next to it. CI-gateable.
#
# Slug rule (GitHub): lowercase, drop every char that is not a letter/digit/space/hyphen, spaces -> hyphens;
# a duplicate slug is disambiguated with -1, -2, ... (the first occurrence stays bare). Fenced code blocks are
# skipped for BOTH headings and links, so a shell `# comment` inside ``` isn't mistaken for a heading (which
# would mask a genuinely broken link) and a `](#x)` shown as example code isn't checked as a real link.
#
# Usage (any cwd; Python 3.9+):
#   python tests/guide_claims/check_anchors.py                    # the four guides
#   python tests/guide_claims/check_anchors.py path/to/doc.md ...
#
# Exit code: 0 = every anchor resolves; 1 = at least one is broken (or a file is missing).

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from skarn_testlib import green, red, rel, repo_root, say  # noqa: E402

GUIDES = ("SkarnGuide.md", "SkarnIn30Minutes.md", "SkarnActors.md", "SkarnStdlib.md")

FENCE = re.compile(r"^\s*```")
HEADING = re.compile(r"^#{1,6}\s+(.*)$")
LINK = re.compile(r"\]\(#([^)]+)\)")
SIBLING_LINK = re.compile(r"\]\(([^)#/\\]+\.md)#([^)]+)\)")  # a file in the same directory, no path


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


def heading_slugs(lines):
    """The slugs of the headings in `lines`, with duplicate disambiguation, and the heading count."""
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
    return slugs, heading_count


def read_lines(path):
    return path.read_text(encoding="utf-8-sig").splitlines()


def check(path):
    """Checks one file; returns True when every anchor it links to resolves."""
    if not path.is_file():
        say(red("ERROR: markdown file not found at '{}'.".format(path)))
        return False
    lines = read_lines(path)
    slugs, heading_count = heading_slugs(lines)

    # Internal links: `](#slug)`, written back as they appear.
    targets = ["#" + m.group(1) for ln in outside_fences(lines) for m in LINK.finditer(ln)]
    distinct = list(dict.fromkeys(targets))
    broken = [t for t in distinct if t[1:] not in slugs]

    # Links into a sibling file: `](Name.md#slug)`. Each target file is read once.
    sibling_targets = ["{}#{}".format(m.group(1), m.group(2))
                       for ln in outside_fences(lines) for m in SIBLING_LINK.finditer(ln)]
    sibling_distinct = list(dict.fromkeys(sibling_targets))
    sibling_slugs = {}
    for t in sibling_distinct:
        name, anchor = t.split("#", 1)
        if name not in sibling_slugs:
            target = path.parent / name
            sibling_slugs[name] = heading_slugs(read_lines(target))[0] if target.is_file() else None
        if sibling_slugs[name] is None or anchor not in sibling_slugs[name]:
            broken.append(t)

    say("anchors: {} headings, {} internal links ({} distinct targets), {} links into other files "
        "({} distinct) in {}".format(heading_count, len(targets), len(distinct), len(sibling_targets),
                                     len(sibling_distinct), rel(path)))
    if broken:
        say(red("==== BROKEN: {} anchor(s) resolve to no heading ====".format(len(broken))))
        for b in sorted(broken):
            needle = "]({})".format(b)
            hit = next((i + 1 for i, ln in enumerate(lines) if needle in ln), None)
            where = "first at line {}".format(hit) if hit else "location unknown"
            missing = "" if b.startswith("#") or sibling_slugs.get(b.split("#", 1)[0]) is not None                 else "; no such file"
            say(red("  {}   ({}{})".format(b, where, missing)))
        return False
    say(green("==== all anchors resolve ===="))
    return True


def main():
    ap = argparse.ArgumentParser(description="Check that every markdown anchor link resolves.")
    ap.add_argument("files", nargs="*", help="markdown files (default: the four guides)")
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
