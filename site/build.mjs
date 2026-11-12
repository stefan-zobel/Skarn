// Builds the Skarn guides (the markdown files in the repository root) into a static website.
//
//   cd site && npm ci && node build.mjs        -> site/dist/
//
// The markdown files stay the single source: nothing generated is checked in. The build fails
// when a link on a generated page does not resolve, so a broken anchor never gets published.
// SITE_URL overrides the public base URL (default: the project's GitHub Pages address).

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import MarkdownIt from "markdown-it";
import anchor from "markdown-it-anchor";
import { createHighlighter } from "shiki";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..");
const dist = path.join(here, "dist");

const REPO_URL = "https://github.com/stefan-zobel/Skarn";
const BASE_URL = (process.env.SITE_URL || "https://stefan-zobel.github.io/Skarn/").replace(/\/*$/, "/");

// The four documents, in reading order.
const DOCS = [
  {
    md: "SkarnIn30Minutes.md",
    out: "30-minutes.html",
    nav: "In 30 minutes",
    blurb: "The working core, enough to write a real program.",
  },
  {
    md: "SkarnGuide.md",
    out: "guide.html",
    nav: "Language Guide",
    blurb: "The whole language and its cost model.",
  },
  {
    md: "SkarnStdlib.md",
    out: "stdlib.html",
    nav: "Standard Library",
    blurb: "Every function of every std module, with worked examples.",
  },
  {
    md: "SkarnActors.md",
    out: "actors.html",
    nav: "Actors",
    blurb: "Long-lived workers that share nothing and talk by messages: supervision, back-pressure and the rest.",
  },
];
const OUT_OF = new Map(DOCS.map((d) => [d.md, d.out]));

const fail = (msg) => {
  console.error("site build: " + msg);
  process.exit(1);
};

const escapeHtml = (s) =>
  s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");

// The anchor rule the guides' own links were written against (the same as GitHub's for these
// headings, and as tests/guide_claims/check_anchors.py): lowercase, keep letters/digits/space/hyphen,
// spaces to hyphens. Duplicates are numbered by markdown-it-anchor (-1, -2, ...).
const slugify = (s) => s.trim().toLowerCase().replace(/[^0-9a-z \-]/g, "").replace(/ /g, "-");

// ---- highlighting: the TextMate grammar the VS Code extension uses --------------------------------
const grammar = JSON.parse(fs.readFileSync(path.join(repoRoot, "docs", "skarn.tmLanguage.json"), "utf8"));
const highlighter = await createHighlighter({
  themes: ["github-light", "github-dark"],
  langs: [{ ...grammar, name: "skarn" }],
});
const SKARN_FENCES = new Set(["rust", "skarn"]);

// ---- links ------------------------------------------------------------------------------------------
function rewriteHref(href, source) {
  if (href.startsWith("#") || /^[a-z][a-z0-9+.-]*:/i.test(href) || href.startsWith("//")) return href;
  const hashAt = href.indexOf("#");
  const target = hashAt < 0 ? href : href.slice(0, hashAt);
  const hash = hashAt < 0 ? "" : href.slice(hashAt);
  const rel = path.posix.normalize(target);
  if (OUT_OF.has(rel)) return OUT_OF.get(rel) + hash;
  const onDisk = path.join(repoRoot, ...rel.split("/"));
  if (!fs.existsSync(onDisk)) fail(`${source}: link to a missing file: ${href}`);
  const kind = fs.statSync(onDisk).isDirectory() ? "tree" : "blob";
  return `${REPO_URL}/${kind}/HEAD/${rel.replace(/\/$/, "")}${hash}`;
}

function makeMarkdown(source) {
  const md = new MarkdownIt({ html: false, linkify: false, typographer: false });
  md.use(anchor, {
    slugify,
    permalink: anchor.permalink.linkInsideHeader({ symbol: "#", placement: "after", ariaHidden: true }),
    level: [1, 2, 3, 4, 5, 6],
  });
  md.core.ruler.push("skarn_links", (state) => {
    for (const tok of state.tokens) {
      for (const child of tok.children || []) {
        if (child.type === "link_open") {
          const i = child.attrIndex("href");
          if (i >= 0) child.attrs[i][1] = rewriteHref(child.attrs[i][1], source);
        }
      }
    }
  });
  md.renderer.rules.fence = (tokens, idx) => {
    const tok = tokens[idx];
    const lang = tok.info.trim().split(/\s+/)[0];
    if (SKARN_FENCES.has(lang)) {
      return highlighter.codeToHtml(tok.content, {
        lang: "skarn",
        themes: { light: "github-light", dark: "github-dark" },
        defaultColor: false,
      });
    }
    return `<pre class="plain"><code>${escapeHtml(tok.content)}</code></pre>\n`;
  };
  return md;
}

// ---- sidebar ----------------------------------------------------------------------------------------
function headingText(inline) {
  return (inline.children || [])
    .filter((c) => c.type === "text" || c.type === "code_inline")
    .map((c) => c.content)
    .join("")
    .trim();
}

function buildToc(tokens) {
  const items = []; // { id, text, children: [] }
  for (let i = 0; i < tokens.length; i++) {
    const t = tokens[i];
    if (t.type !== "heading_open" || (t.tag !== "h2" && t.tag !== "h3")) continue;
    const entry = { id: t.attrGet("id"), text: headingText(tokens[i + 1]), children: [] };
    if (t.tag === "h2" || items.length === 0) items.push(entry);
    else items[items.length - 1].children.push(entry);
  }
  const link = (e) => `<a href="#${e.id}">${escapeHtml(e.text)}</a>`;
  const li = (e) =>
    `<li>${link(e)}` +
    (e.children.length ? `<ul>${e.children.map((c) => `<li>${link(c)}</li>`).join("")}</ul>` : "") +
    "</li>";
  return `<ul>${items.map(li).join("")}</ul>`;
}

// ---- pages ------------------------------------------------------------------------------------------
const template = fs.readFileSync(path.join(here, "template.html"), "utf8");

function pageUrl(out) {
  return out === "index.html" ? BASE_URL : BASE_URL + out;
}

function renderPage({ out, title, description, content, toc }) {
  const nav = DOCS.map(
    (d) => `<a href="${d.out}"${d.out === out ? ' aria-current="page"' : ""}>${escapeHtml(d.nav)}</a>`,
  ).join("");
  const sidebar = toc
    ? `<details class="sidebar" open><summary>Contents</summary><nav aria-label="Contents">${toc}</nav></details>`
    : "";
  const html = template
    .split("{{TITLE}}").join(escapeHtml(title))
    .split("{{DESCRIPTION}}").join(escapeHtml(description))
    .split("{{CANONICAL}}").join(pageUrl(out))
    .split("{{NAV}}").join(nav)
    .split("{{LAYOUT}}").join(toc ? "with-sidebar" : "single")
    .split("{{SIDEBAR}}").join(sidebar)
    .split("{{CONTENT}}").join(content);
  fs.writeFileSync(path.join(dist, out), html);
}

fs.rmSync(dist, { recursive: true, force: true });
fs.mkdirSync(dist, { recursive: true });
for (const f of ["style.css", "toc.js"]) fs.copyFileSync(path.join(here, f), path.join(dist, f));

for (const d of DOCS) {
  const text = fs.readFileSync(path.join(repoRoot, d.md), "utf8").replace(/^﻿/, "");
  const md = makeMarkdown(d.md);
  const env = {};
  const tokens = md.parse(text, env);
  const first = tokens.find((t) => t.type === "heading_open" && t.tag === "h1");
  const title = first ? headingText(tokens[tokens.indexOf(first) + 1]) : d.nav;
  renderPage({
    out: d.out,
    title: `${title} – Skarn`,
    description: d.blurb,
    content: `<article>${md.renderer.render(tokens, md.options, env)}</article>`,
    toc: buildToc(tokens),
  });
  d.title = title;
}

// The landing page: one card per document.
const cards = DOCS.map(
  (d) =>
    `<li><a class="card" href="${d.out}"><strong>${escapeHtml(d.title)}</strong><span>${escapeHtml(d.blurb)}</span></a></li>`,
).join("");
renderPage({
  out: "index.html",
  title: "Skarn",
  description:
    "Skarn is a statically typed language with a Rust-flavored surface and a garbage collector instead of a borrow checker. Guides, standard library reference and actors.",
  content: `<article class="landing">
<h1>Skarn</h1>
<p><strong>Skarn</strong> is a statically typed language with a Rust-flavored surface: enums with exhaustive
<code>match</code>, <code>Option</code>/<code>Result</code> with <code>?</code>, traits with bounds, generics,
<code>dyn</code> trait objects, lazy iterators, modules, and everything an expression. It has no borrow checker:
memory is managed by a garbage collector, so you never write lifetimes or think about ownership. Programs are
type-checked, compiled to bytecode and run on <strong>vMachine</strong>, a compact register VM with a moving
garbage collector.</p>
<h2>Documentation</h2>
<ul class="cards">${cards}</ul>
<h2>Source and downloads</h2>
<p><a href="${REPO_URL}">Source code and issue tracker</a> ·
<a href="${REPO_URL}/releases">Releases</a> (Windows and macOS)</p>
</article>`,
  toc: "",
});

// ---- sitemap and robots.txt --------------------------------------------------------------------------
function lastModified(file) {
  try {
    const out = execFileSync("git", ["log", "-1", "--format=%cs", "--", file], {
      cwd: repoRoot,
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
    }).trim();
    return /^\d{4}-\d\d-\d\d$/.test(out) ? out : "";
  } catch {
    return ""; // no git, or not a checkout: leave the date out rather than invent one
  }
}

const dates = new Map(DOCS.map((d) => [d.out, lastModified(d.md)]));
dates.set("index.html", [...dates.values()].filter(Boolean).sort().pop() || "");
const pages = ["index.html", ...DOCS.map((d) => d.out)];
const sitemap =
  `<?xml version="1.0" encoding="UTF-8"?>\n` +
  `<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n` +
  pages
    .map((p) => {
      const mod = dates.get(p);
      return `  <url>\n    <loc>${pageUrl(p)}</loc>\n${mod ? `    <lastmod>${mod}</lastmod>\n` : ""}  </url>\n`;
    })
    .join("") +
  `</urlset>\n`;
fs.writeFileSync(path.join(dist, "sitemap.xml"), sitemap);
fs.writeFileSync(path.join(dist, "robots.txt"), `User-agent: *\nAllow: /\n\nSitemap: ${BASE_URL}sitemap.xml\n`);

// ---- verification: the build fails on a link that does not resolve ----------------------------------
const generated = fs.readdirSync(dist).filter((f) => f.endsWith(".html")).sort();
if (JSON.stringify(generated) !== JSON.stringify([...pages].sort())) {
  fail(`the sitemap lists ${pages.join(", ")} but dist holds ${generated.join(", ")}`);
}
const idsOf = new Map();
const htmlOf = new Map();
for (const f of generated) {
  const html = fs.readFileSync(path.join(dist, f), "utf8");
  htmlOf.set(f, html);
  const ids = new Set();
  for (const m of html.matchAll(/\sid="([^"]*)"/g)) {
    if (m[1] === "") fail(`${f}: a heading has an empty id`);
    if (ids.has(m[1])) fail(`${f}: duplicate id "${m[1]}"`);
    ids.add(m[1]);
  }
  idsOf.set(f, ids);
}
let links = 0;
let broken = 0;
for (const [f, html] of htmlOf) {
  for (const m of html.matchAll(/<a\s[^>]*href="([^"]*)"/g)) {
    const href = m[1].replace(/&amp;/g, "&");
    if (/^[a-z][a-z0-9+.-]*:/i.test(href)) continue; // external
    links++;
    const hashAt = href.indexOf("#");
    const file = hashAt === 0 ? f : hashAt < 0 ? href : href.slice(0, hashAt);
    const hash = hashAt < 0 ? "" : href.slice(hashAt + 1);
    if (!idsOf.has(file) || (hash && !idsOf.get(file).has(hash))) {
      console.error(`site build: ${f}: broken link "${href}"`);
      broken++;
    }
  }
}
if (broken > 0) fail(`${broken} broken link(s)`);

const size = generated.reduce((n, f) => n + fs.statSync(path.join(dist, f)).size, 0);
console.log(`site build: ${generated.length} pages (${Math.round(size / 1024)} KB), ${links} internal links ok, sitemap + robots.txt written`);
