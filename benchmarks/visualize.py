#!/usr/bin/env python3
"""
Visualize Skarn benchmark results from results.csv.
Usage:
    python3 benchmarks/visualize.py
    python3 benchmarks/visualize.py --csv path/to/results.csv
    python3 benchmarks/visualize.py --out benchmarks/results/charts.png
"""
import argparse
import sys
from pathlib import Path

import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

RESULTS_CSV = Path(__file__).parent / "results" / "results.csv"

LANG_COLORS = {
    "skarn":  "#5B9BD5",
    "python": "#FFB347",
    "cpp":    "#70AD47",
}
LANG_ORDER = ["cpp", "python", "skarn"]

BENCH_ORDER = ["arith", "fib", "iter", "sort", "strings", "alloc", "hashmap"]


def fmt_ns(ns: float) -> str:
    if ns < 1_000:
        return f"{ns:.0f} ns"
    if ns < 1_000_000:
        return f"{ns/1_000:.1f} µs"
    if ns < 1_000_000_000:
        return f"{ns/1_000_000:.1f} ms"
    return f"{ns/1_000_000_000:.2f} s"


def load(csv_path: Path) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    # Keep only successful runs
    df = df[df["exit_code"] == 0].copy()
    df["language"] = df["language"].str.lower()
    df["benchmark"] = df["benchmark"].str.lower()
    # If multiple runs exist for the same (language, benchmark), keep the latest
    df = df.sort_values("timestamp").groupby(["language", "benchmark"], as_index=False).last()
    return df


def wall_chart(df: pd.DataFrame, ax: plt.Axes) -> None:
    """Grouped bar chart: wall_vm_ns per benchmark, grouped by language."""
    benches = [b for b in BENCH_ORDER if b in df["benchmark"].unique()]
    langs   = [l for l in LANG_ORDER   if l in df["language"].unique()]

    x      = np.arange(len(benches))
    width  = 0.8 / len(langs)
    offset = np.linspace(-(len(langs)-1)/2, (len(langs)-1)/2, len(langs)) * width

    for i, lang in enumerate(langs):
        sub  = df[df["language"] == lang].set_index("benchmark")
        vals = [sub.loc[b, "wall_vm_ns"] if b in sub.index else 0 for b in benches]
        bars = ax.bar(x + offset[i], vals, width * 0.9,
                      label=lang.capitalize(),
                      color=LANG_COLORS.get(lang, "#888"),
                      edgecolor="white", linewidth=0.5)
        for bar, v in zip(bars, vals):
            if v > 0:
                ax.text(bar.get_x() + bar.get_width() / 2,
                        bar.get_height() * 1.04,
                        fmt_ns(v),
                        ha="center", va="bottom",
                        fontsize=6.5, rotation=45, color="#444")

    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels(benches, fontsize=9)
    ax.set_ylabel("Wall time (ns, log scale)", fontsize=9)
    ax.legend(fontsize=8)
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: fmt_ns(v)))
    ax.tick_params(axis="y", labelsize=7)
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    ax.set_axisbelow(True)


def rss_chart(df: pd.DataFrame, ax: plt.Axes) -> None:
    """Peak RSS (MB) grouped bar chart."""
    benches = [b for b in BENCH_ORDER if b in df["benchmark"].unique()]
    langs   = [l for l in LANG_ORDER   if l in df["language"].unique()]
    x      = np.arange(len(benches))
    width  = 0.8 / len(langs)
    offset = np.linspace(-(len(langs)-1)/2, (len(langs)-1)/2, len(langs)) * width

    for i, lang in enumerate(langs):
        sub  = df[df["language"] == lang].set_index("benchmark")
        vals = [sub.loc[b, "rss_mb"] if b in sub.index else 0 for b in benches]
        bars = ax.bar(x + offset[i], vals, width * 0.9,
                      label=lang.capitalize(),
                      color=LANG_COLORS.get(lang, "#888"),
                      edgecolor="white", linewidth=0.5)
        for bar, v in zip(bars, vals):
            if v > 0.5:
                ax.text(bar.get_x() + bar.get_width() / 2,
                        bar.get_height() + 0.3,
                        f"{v:.1f}",
                        ha="center", va="bottom",
                        fontsize=6.5, color="#444")

    ax.set_xticks(x)
    ax.set_xticklabels(benches, fontsize=9)
    ax.set_ylabel("Peak RSS (MB)", fontsize=9)
    ax.legend(fontsize=8)
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    ax.set_axisbelow(True)


def speedup_chart(df: pd.DataFrame, ax: plt.Axes, baseline: str = "cpp") -> None:
    """
    Horizontal bar chart showing how many times slower each language is
    compared to the baseline (default: cpp).
    """
    benches = [b for b in BENCH_ORDER if b in df["benchmark"].unique()]
    base_df = df[df["language"] == baseline].set_index("benchmark")

    langs_vs = [l for l in LANG_ORDER if l != baseline and l in df["language"].unique()]
    colors   = [LANG_COLORS.get(l, "#888") for l in langs_vs]

    y_positions = np.arange(len(benches))
    height = 0.8 / len(langs_vs)
    offset = np.linspace(-(len(langs_vs)-1)/2, (len(langs_vs)-1)/2, len(langs_vs)) * height

    for i, lang in enumerate(langs_vs):
        sub     = df[df["language"] == lang].set_index("benchmark")
        ratios  = []
        for b in benches:
            if b in sub.index and b in base_df.index:
                base_ns = base_df.loc[b, "wall_vm_ns"]
                lang_ns = sub.loc[b, "wall_vm_ns"]
                ratios.append(lang_ns / base_ns if base_ns > 0 else 0)
            else:
                ratios.append(0)
        bars = ax.barh(y_positions + offset[i], ratios, height * 0.9,
                       label=lang.capitalize(), color=colors[i],
                       edgecolor="white", linewidth=0.5)
        for bar, r in zip(bars, ratios):
            if r > 0:
                ax.text(bar.get_width() + 0.5,
                        bar.get_y() + bar.get_height() / 2,
                        f"{r:.1f}×",
                        va="center", fontsize=7, color="#444")

    ax.set_yticks(y_positions)
    ax.set_yticklabels(benches, fontsize=9)
    ax.set_xscale("log")
    ax.set_xlabel(f"× slower than {baseline.upper()} (log scale)", fontsize=9)
    ax.axvline(1, color="#999", linestyle="--", linewidth=0.8)
    ax.legend(fontsize=8)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:,.0f}×"))
    ax.grid(axis="x", linestyle="--", alpha=0.4)
    ax.set_axisbelow(True)


def gc_chart(df: pd.DataFrame, ax: plt.Axes) -> None:
    """GC % of wall time for all languages."""
    langs   = [l for l in LANG_ORDER if l in df["language"].unique()]
    benches = [b for b in BENCH_ORDER if b in df["benchmark"].unique()]

    x      = np.arange(len(benches))
    width  = 0.8 / len(langs)
    offset = np.linspace(-(len(langs)-1)/2, (len(langs)-1)/2, len(langs)) * width

    for i, lang in enumerate(langs):
        sub  = df[df["language"] == lang].set_index("benchmark")
        vals = [sub.loc[b, "gc_pct"] if b in sub.index else 0.0 for b in benches]
        bars = ax.bar(x + offset[i], vals, width * 0.9,
                      label=lang.capitalize(),
                      color=LANG_COLORS.get(lang, "#888"),
                      edgecolor="white", linewidth=0.5)
        for bar, v in zip(bars, vals):
            if v > 0.05:
                ax.text(bar.get_x() + bar.get_width() / 2,
                        bar.get_height() + 0.03,
                        f"{v:.2f}%",
                        ha="center", va="bottom", fontsize=7, color="#444")

    ax.set_xticks(x)
    ax.set_xticklabels(benches, fontsize=9)
    ax.set_ylabel("GC time %", fontsize=9)
    ax.legend(fontsize=8)
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    ax.set_axisbelow(True)


def speedup_table(df: pd.DataFrame, ax: plt.Axes, baseline: str = "cpp") -> None:
    """Table showing exact ×-slowdown ratios so extreme outliers don't crush the chart."""
    langs   = [l for l in LANG_ORDER if l != baseline and l in df["language"].unique()]
    benches = [b for b in BENCH_ORDER if b in df["benchmark"].unique()]
    base_df = df[df["language"] == baseline].set_index("benchmark")

    col_labels = ["Benchmark"] + [f"{l.capitalize()} Wall" for l in langs] \
                               + [f"vs {baseline.upper()}: {l.capitalize()}" for l in langs]
    rows = []
    for b in benches:
        row = [b]
        for l in langs:
            sub = df[(df["language"] == l) & (df["benchmark"] == b)]
            row.append(fmt_ns(sub["wall_vm_ns"].values[0]) if len(sub) else "—")
        for l in langs:
            sub = df[(df["language"] == l) & (df["benchmark"] == b)]
            if len(sub) and b in base_df.index:
                ratio = sub["wall_vm_ns"].values[0] / base_df.loc[b, "wall_vm_ns"]
                row.append(f"{ratio:.2f}×")
            else:
                row.append("—")
        rows.append(row)

    ax.axis("off")
    tbl = ax.table(cellText=rows, colLabels=col_labels, cellLoc="center", loc="center")
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(8)
    tbl.scale(1, 1.45)
    for j in range(len(col_labels)):
        tbl[0, j].set_facecolor("#2D3748")
        tbl[0, j].set_text_props(color="white", fontweight="bold")
    for i in range(1, len(rows) + 1):
        bg = "#F7FAFC" if i % 2 == 0 else "white"
        for j in range(len(col_labels)):
            tbl[i, j].set_facecolor(bg)


def summary_table(df: pd.DataFrame, ax: plt.Axes) -> None:
    """Render a formatted table with wall time and RSS for all (lang, bench) combos."""
    langs   = [l for l in LANG_ORDER if l in df["language"].unique()]
    benches = [b for b in BENCH_ORDER if b in df["benchmark"].unique()]

    col_labels = ["Benchmark"] + [l.capitalize() + " Wall" for l in langs] \
                               + [l.capitalize() + " RSS" for l in langs]
    rows = []
    for b in benches:
        row = [b]
        for l in langs:
            sub = df[(df["language"] == l) & (df["benchmark"] == b)]
            row.append(fmt_ns(sub["wall_vm_ns"].values[0]) if len(sub) else "—")
        for l in langs:
            sub = df[(df["language"] == l) & (df["benchmark"] == b)]
            row.append(f"{sub['rss_mb'].values[0]:.1f} MB" if len(sub) else "—")
        rows.append(row)

    ax.axis("off")
    tbl = ax.table(cellText=rows, colLabels=col_labels,
                   cellLoc="center", loc="center")
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(8.5)
    tbl.scale(1, 1.4)

    # Header styling
    for j in range(len(col_labels)):
        tbl[0, j].set_facecolor("#2D3748")
        tbl[0, j].set_text_props(color="white", fontweight="bold")

    # Alternate row shading
    for i, row in enumerate(rows, start=1):
        bg = "#F7FAFC" if i % 2 == 0 else "white"
        for j in range(len(col_labels)):
            tbl[i, j].set_facecolor(bg)



def make_fig(title: str, figsize=(12, 6)) -> tuple[plt.Figure, plt.Axes]:
    fig, ax = plt.subplots(figsize=figsize)
    fig.suptitle(title, fontsize=11, fontweight="bold", y=0.98)
    fig.subplots_adjust(top=0.88, bottom=0.12, left=0.10, right=0.97)
    return fig, ax


def save_or_show(fig: plt.Figure, out_dir: Path | None, name: str) -> None:
    if out_dir is not None:
        path = out_dir / f"{name}.png"
        fig.savefig(path, dpi=300, bbox_inches="tight")
        print(f"  {path}")
    else:
        plt.show()
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=str(RESULTS_CSV), help="Path to results.csv")
    ap.add_argument("--out", default=None,
                    help="Output directory for PNGs (default: show interactively)")
    args = ap.parse_args()

    csv_path = Path(args.csv)
    if not csv_path.exists():
        print(f"error: CSV not found at {csv_path}", file=sys.stderr)
        print("       Run benchmarks first: python3 benchmarks/run.py", file=sys.stderr)
        sys.exit(1)

    df = load(csv_path)
    if df.empty:
        print("error: no successful benchmark rows found in CSV", file=sys.stderr)
        sys.exit(1)

    latest = df.sort_values("timestamp").iloc[-1]
    cpu  = latest["cpu"]
    os_  = latest["os"]
    date = latest["timestamp"][:10]
    sub  = f"{cpu}  ·  {os_}  ·  {date}"

    out_dir = Path(args.out) if args.out else None
    if out_dir is not None:
        out_dir.mkdir(parents=True, exist_ok=True)
        print(f"Saving charts to {out_dir}/")

    # 1 — wall time
    fig, ax = make_fig(f"Wall Time by Benchmark & Language  ·  {sub}", figsize=(14, 6))
    wall_chart(df, ax)
    save_or_show(fig, out_dir, "01_wall_time")

    # 2 — peak RSS
    fig, ax = make_fig(f"Peak Memory Usage  ·  {sub}", figsize=(12, 6))
    rss_chart(df, ax)
    save_or_show(fig, out_dir, "02_peak_rss")

    # 3 — relative slowdown vs C++
    fig, ax = make_fig(f"Relative Slowdown vs C++  ·  {sub}", figsize=(12, 6))
    speedup_chart(df, ax)
    save_or_show(fig, out_dir, "03_slowdown_vs_cpp")

    # 4 — GC overhead
    # NOTE: Python measures only cyclic-collector (refcount frees are invisible);
    # C++ hardcodes 0. Skarn shows real tracing-collector stats. Not directly comparable.
    fig, ax = make_fig(f"GC Overhead (% of wall time)  ·  {sub}  *Skarn only; see note", figsize=(12, 5))
    gc_chart(df, ax)
    save_or_show(fig, out_dir, "04_gc_overhead")

    # 5 — exact ratio table
    fig, ax = make_fig(f"Slowdown vs C++ (exact ratios)  ·  {sub}", figsize=(13, 5))
    speedup_table(df, ax)
    save_or_show(fig, out_dir, "05_slowdown_table")


if __name__ == "__main__":
    main()
