# =============================================================================
# ebnf_railroad.py -- render the Skarn grammar as a railroad (syntax) diagram SVG.
#
# Usage:  python tools\ebnf_railroad.py
#         (writes docs\skarn_grammar_railroad.svg)
#
# Dependency-free (stdlib only): a tiny railroad layout engine with Manhattan
# (right-angle) routing so every connector meets exactly. The grammar is the
# SINGLE SOURCE OF TRUTH in docs/skarn_grammar.ebnf -- this script PARSES that EBNF
# (tokenizer + recursive-descent, see _tokenize_ebnf / _Parser) into railroad
# nodes (T=terminal, N=nonterminal, Seq, Choice, Opt=[ ], Rep=zero-or-more { }),
# so there is no second copy of the grammar to keep in sync. Styling is INLINE
# presentation attributes (not a CSS <style> block), so limited renderers (Qt's
# QSvgRenderer) match browsers. The theme matches tools/ebnf_to_svg.ps1 / docs/skarn_grammar.svg.
#
# Optional raster (docs\skarn_grammar_railroad.png) via PyQt5/QtSvg:
#   from PyQt5.QtWidgets import QApplication; from PyQt5.QtSvg import QSvgRenderer
#   from PyQt5.QtGui import QImage, QPainter
#   app=QApplication([]); r=QSvgRenderer('docs/skarn_grammar_railroad.svg')
#   img=QImage(r.defaultSize(), QImage.Format_ARGB32); img.fill(0)
#   p=QPainter(img); r.render(p); p.end(); img.save('docs/skarn_grammar_railroad.png')
# =============================================================================

import os

# ---- layout metrics ---------------------------------------------------------
FS    = 13.0     # node text font size
CW    = 7.8      # monospace advance per char (generous, so text never overflows)
PADX  = 11.0     # horizontal text padding inside a node box
H     = 22.0     # node box height
AR    = 14.0     # branch inset / stub length
VS    = 10.0     # vertical gap between stacked alternatives / loop lines
SEQ   = 14.0     # horizontal gap between sequence items


# Theme colours (inline as presentation attributes -- NOT a CSS <style> block, so
# limited SVG renderers such as Qt's QSvgRenderer render it identically to browsers).
C_BG   = "#0f172a"
C_CARD = "#111827"
C_LINE = "#94a3b8"
C_CAP  = "#f472b6"
C_TITLE = "#e2e8f0"
C_SUB  = "#64748b"
C_RULE = "#fbbf24"
TM_FILL, TM_STROKE, TM_TEXT = "#064e3b", "#34d399", "#a7f3d0"   # terminal
NT_FILL, NT_STROKE, NT_TEXT = "#1e293b", "#60a5fa", "#bfdbfe"   # nonterminal


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def hline(x, y, length):
    if length <= 0:
        return ""
    return ("<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' stroke-width='1.5'/>"
            % (x, y, x + length, y, C_LINE))


def vline(x, y1, y2):
    if abs(y2 - y1) < 0.01:
        return ""
    return ("<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' stroke-width='1.5'/>"
            % (x, y1, x, y2, C_LINE))


# ---- nodes ------------------------------------------------------------------
class Leaf(object):
    def __init__(self, text, cls):
        self.text = text
        self.cls = cls                      # 'tm' terminal / 'nt' nonterminal
        self.width = max(len(text) * CW + 2 * PADX, 24.0)
        self.up = H / 2.0
        self.down = H / 2.0

    def fmt(self, x, y):
        if self.cls == "tm":
            rx, fill, stroke, tcol = H / 2.0, TM_FILL, TM_STROKE, TM_TEXT
        else:
            rx, fill, stroke, tcol = 4.0, NT_FILL, NT_STROKE, NT_TEXT
        box = ("<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='%.1f' "
               "fill='%s' stroke='%s' stroke-width='1.5'/>"
               % (x, y - H / 2.0, self.width, H, rx, fill, stroke))
        txt = ("<text x='%.1f' y='%.1f' text-anchor='middle' fill='%s' font-size='%.0f'>%s</text>"
               % (x + self.width / 2.0, y + FS * 0.34, tcol, FS, esc(self.text)))
        return box + txt


def T(text):
    return Leaf(text, "tm")


def N(name):
    return Leaf(name, "nt")


class Empty(object):
    """An empty alternative / sequence (e.g. the nullary tuple-struct branch) --
    a straight pass-through with no box. Zero width; the enclosing Choice/Seq
    draws the connecting line."""
    def __init__(self):
        self.width = 0.0
        self.up = H / 2.0
        self.down = H / 2.0

    def fmt(self, x, y):
        return ""


class Seq(object):
    def __init__(self, *items):
        self.items = list(items)
        self.width = sum(i.width for i in self.items) + SEQ * (len(self.items) - 1)
        self.up = max(i.up for i in self.items)
        self.down = max(i.down for i in self.items)

    def fmt(self, x, y):
        out = []
        cx = x
        for idx, it in enumerate(self.items):
            if idx > 0:
                out.append(hline(cx, y, SEQ))
                cx += SEQ
            out.append(it.fmt(cx, y))
            cx += it.width
        return "".join(out)


class Choice(object):
    def __init__(self, *items):
        self.items = list(items)
        self.inner = max(i.width for i in self.items)
        self.width = self.inner + 2 * AR
        self.up = self.items[0].up
        # stack the remaining alternatives below the first, remembering each baseline
        self.ys = [0.0]                      # baseline offsets from the entry line
        bottom = self.items[0].down
        for it in self.items[1:]:
            base = bottom + VS + it.up
            self.ys.append(base)
            bottom = base + it.down
        self.down = bottom

    def fmt(self, x, y):
        out = []
        xr = x + self.width - AR             # right rail
        xl = x + AR                          # left rail
        last = y + self.ys[-1]
        out.append(hline(x, y, AR))          # left stub
        out.append(hline(xr, y, AR))         # right stub
        out.append(vline(xl, y, last))       # left rail down to the lowest alt
        out.append(vline(xr, y, last))       # right rail
        for it, dy in zip(self.items, self.ys):
            yb = y + dy
            out.append(it.fmt(xl, yb))
            out.append(hline(xl + it.width, yb, (xr) - (xl + it.width)))  # filler to right rail
        return "".join(out)


class Opt(object):
    """[ item ] -- zero or one; straight skip on the line, item dips below."""
    def __init__(self, item):
        self.item = item
        self.width = item.width + 2 * AR
        self.up = 0.0
        self.item_y = VS + item.up
        self.down = self.item_y + item.down

    def fmt(self, x, y):
        yi = y + self.item_y
        xl = x + AR
        xr = x + self.width - AR
        out = [hline(x, y, self.width), vline(xl, y, yi), vline(xr, y, yi),
               self.item.fmt(xl, yi)]
        out.append(hline(xl + self.item.width, yi, xr - (xl + self.item.width)))
        return "".join(out)


class Rep(object):
    """{ item } -- zero or more; straight skip, item below, loop-back under it."""
    def __init__(self, item):
        self.item = item
        self.width = item.width + 2 * AR
        self.up = 0.0
        self.item_y = VS + item.up
        self.loop_y = self.item_y + item.down + VS
        self.down = self.loop_y + 6.0

    def fmt(self, x, y):
        yi = y + self.item_y
        yl = y + self.loop_y
        xl = x + AR
        xr = x + self.width - AR
        out = [hline(x, y, self.width),         # skip
               vline(xl, y, yl),                # left rail (skip-leg + loop-leg)
               vline(xr, y, yl),                # right rail
               self.item.fmt(xl, yi)]
        out.append(hline(xl + self.item.width, yi, xr - (xl + self.item.width)))  # item filler
        out.append(hline(xl, yl, xr - xl))      # loop return line
        # left-pointing arrow at the middle of the return line (flow repeats)
        mx = (xl + xr) / 2.0
        out.append("<path d='M %.1f %.1f l 7 -4 l 0 8 z' fill='%s'/>" % (mx - 3, yl, C_LINE))
        return "".join(out)


# ---- EBNF parser (single source of truth: docs/skarn_grammar.ebnf) ----------
# A tiny tokenizer + recursive-descent parser over ISO-style EBNF, so the grammar
# lives ONLY in docs/skarn_grammar.ebnf and is never transcribed here. Standard operators
# map onto the railroad nodes: `,`->Seq, `|`->Choice, `[ ]`->Opt, `{ }`->Rep,
# `( )`->group, "..."/'...'->T(erminal), name->N(onterminal), and the character
# range "a".."f" -> one T(erminal) box labelled a..f. An empty alternative
# (the nullary tuple-struct branch) parses as Empty; a Choice carrying one is then
# normalized to Opt, reproducing the hand-drawn look. Comments `(* .. *)` (the
# lexical/semantic notes) are skipped. Undefined names (letter/digit/char/NEWLINE)
# render as plain nonterminal boxes, exactly as before.

class _Tok(object):
    __slots__ = ("kind", "val")
    def __init__(self, kind, val):
        self.kind = kind          # 'ident' | 'term' | 'op' | 'eq' | 'semi'
        self.val = val


def _tokenize_ebnf(text):
    toks = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == "(" and i + 1 < n and text[i + 1] == "*":       # (* comment *)
            j = text.find("*)", i + 2)
            if j < 0:
                raise ValueError("unterminated (* .. *) comment")
            i = j + 2
        elif c == '"' or c == "'":                                # "term" or 'term'
            j = text.find(c, i + 1)
            if j < 0:
                raise ValueError("unterminated terminal at offset %d" % i)
            toks.append(_Tok("term", text[i + 1:j]))
            i = j + 1
        elif c.isalpha() or c == "_":                             # ident / NEWLINE
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            toks.append(_Tok("ident", text[i:j]))
            i = j
        elif c == "=":
            toks.append(_Tok("eq", "=")); i += 1
        elif c == ";":
            toks.append(_Tok("semi", ";")); i += 1
        elif c in ",|[]{}()":
            toks.append(_Tok("op", c)); i += 1
        elif c == "." and i + 1 < n and text[i + 1] == ".":        # "a".."f" range
            toks.append(_Tok("op", "..")); i += 2
        else:
            raise ValueError("unexpected EBNF character %r at offset %d" % (c, i))
    return toks


class _Parser(object):
    # grammar     := { ident '=' alternation ';' }
    # alternation := concatenation { '|' concatenation }
    # concat      := [ term { ',' term } ]        (* empty -> Empty *)
    # term        := '[' alternation ']' | '{' alternation '}' | '(' alternation ')'
    #              | terminal [ '..' terminal ] | ident
    _STOP = ("|", "]", "}", ")")

    def __init__(self, toks):
        self.toks = toks
        self.pos = 0

    def _peek(self):
        return self.toks[self.pos] if self.pos < len(self.toks) else None

    def _next(self):
        t = self.toks[self.pos]
        self.pos += 1
        return t

    def _expect(self, kind, val=None):
        t = self._next()
        if t.kind != kind or (val is not None and t.val != val):
            raise ValueError("expected %s %r, got %s %r" % (kind, val, t.kind, t.val))
        return t

    def parse_grammar(self):
        rules = []
        while self._peek() is not None:
            name = self._expect("ident").val
            self._expect("eq")
            node = self.parse_alternation()
            self._expect("semi")
            rules.append((name, node))
        return rules

    def parse_alternation(self):
        alts = [self.parse_concatenation()]
        while True:
            t = self._peek()
            if t is None or not (t.kind == "op" and t.val == "|"):
                break
            self._next()
            alts.append(self.parse_concatenation())
        if len(alts) == 1:
            return alts[0]
        # Normalize an empty branch (e.g. the nullary tuple-struct case) into an
        # optional group, matching the hand-drawn Opt(Choice(..)) look.
        real = [a for a in alts if not isinstance(a, Empty)]
        if len(real) < len(alts):
            return Opt(real[0] if len(real) == 1 else Choice(*real))
        return Choice(*alts)

    def parse_concatenation(self):
        items = []
        while True:
            t = self._peek()
            if t is None or t.kind in ("semi", "eq") or (t.kind == "op" and t.val in self._STOP):
                break
            items.append(self.parse_term())
            t = self._peek()
            if t is not None and t.kind == "op" and t.val == ",":   # explicit concat separator
                self._next()
        if not items:
            return Empty()
        return items[0] if len(items) == 1 else Seq(*items)

    def parse_term(self):
        t = self._next()
        if t.kind == "term":
            nxt = self._peek()
            if nxt is not None and nxt.kind == "op" and nxt.val == "..":
                self._next()
                hi = self._expect("term").val
                return T(t.val + ".." + hi)
            return T(t.val)
        if t.kind == "ident":
            return N(t.val)
        if t.kind == "op" and t.val == "[":
            inner = self.parse_alternation(); self._expect("op", "]")
            return Opt(inner)
        if t.kind == "op" and t.val == "{":
            inner = self.parse_alternation(); self._expect("op", "}")
            return Rep(inner)
        if t.kind == "op" and t.val == "(":
            inner = self.parse_alternation(); self._expect("op", ")")
            return inner
        raise ValueError("unexpected token %s %r" % (t.kind, t.val))


def build_grammar():
    here = os.path.dirname(os.path.abspath(__file__))
    ebnf_path = os.path.join(here, "..", "docs", "skarn_grammar.ebnf")
    with open(ebnf_path, "r", encoding="utf-8") as f:
        text = f.read()
    return _Parser(_tokenize_ebnf(text)).parse_grammar()


# ---- document assembly ------------------------------------------------------
MARGIN = 30.0
RULE_TITLE_FS = 15.0
TITLE_GAP = 16.0     # between a rule's title and its diagram
BLOCK_PAD = 30.0     # below each rule's diagram
ENDCAP = 22.0        # start/end marker + connector width on each side


def render(grammar):
    # measure
    body_w = 0.0
    y = 96.0
    placed = []
    for name, node in grammar:
        yb = y + TITLE_GAP + node.up
        placed.append((name, node, y, yb))
        body_w = max(body_w, ENDCAP * 2 + node.width)
        y = yb + node.down + BLOCK_PAD
    width = MARGIN * 2 + body_w
    height = y + 10.0

    out = []
    out.append("<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' "
               "viewBox='0 0 %d %d' font-family='Consolas, \"DejaVu Sans Mono\", monospace'>"
               % (int(width), int(height), int(width), int(height)))
    out.append("<rect x='0' y='0' width='%d' height='%d' fill='%s'/>"
               % (int(width), int(height), C_BG))
    out.append("<rect x='14' y='14' width='%d' height='%d' rx='10' fill='%s'/>"
               % (int(width) - 28, int(height) - 28, C_CARD))
    out.append("<text x='%.0f' y='46' fill='%s' font-size='20' font-weight='bold'>"
               "Skarn &#8212; Railroad Diagram</text>" % (MARGIN, C_TITLE))
    out.append("<text x='%.0f' y='66' fill='%s' font-size='12'>Rust-family syntax, "
               "sound static types &#183; generated from docs/skarn_grammar.ebnf</text>"
               % (MARGIN, C_SUB))

    for name, node, ytop, yb in placed:
        out.append("<text x='%.0f' y='%.0f' fill='%s' font-size='%.0f' font-weight='bold'>%s</text>"
                   % (MARGIN, ytop + RULE_TITLE_FS, C_RULE, RULE_TITLE_FS, esc(name)))
        x0 = MARGIN
        # start marker + connector, diagram, exit connector + end marker
        out.append("<circle cx='%.1f' cy='%.1f' r='5' fill='%s'/>" % (x0 + 5, yb, C_CAP))
        out.append(hline(x0 + 10, yb, ENDCAP - 10))
        out.append(node.fmt(x0 + ENDCAP, yb))
        xe = x0 + ENDCAP + node.width
        out.append(hline(xe, yb, ENDCAP - 10))
        out.append("<circle cx='%.1f' cy='%.1f' r='5' fill='%s'/>" % (xe + ENDCAP - 5, yb, C_CAP))

    out.append("</svg>")
    return "".join(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(here, "..", "docs", "skarn_grammar_railroad.svg")
    svg = render(build_grammar())
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(svg)
    print("wrote %s (%d bytes)" % (os.path.normpath(out_path), len(svg)))


if __name__ == "__main__":
    main()
