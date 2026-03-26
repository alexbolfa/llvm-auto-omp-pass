# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib", "pandas"]
# ///

import argparse
from io import StringIO
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


def load_bench_csv(path):
    text = Path(path).read_text()
    lines = text.strip().splitlines()
    csv_start = next(i for i, l in enumerate(lines) if l.startswith("name,"))
    df = pd.read_csv(StringIO("\n".join(lines[csv_start:])))
    df = df[df["name"].str.contains("_median")]
    df["short_name"] = df["name"].str.extract(r'"?(\w+?)Bench/')[0]
    return df


def plot_comparison(df_base, df_opt, out_path):
    merged = df_base.merge(df_opt, on="short_name", suffixes=("_base", "_opt"))
    merged["speedup"] = merged["cpu_time_base"] / merged["cpu_time_opt"]
    merged = merged.sort_values("speedup", ascending=True)

    bar_colors = ["#3b4d7a", "#3f8f8f", "#6abf69"]
    colors = bar_colors[-len(merged):]

    fig, ax = plt.subplots(figsize=(10, 5), dpi=200)
    fig.patch.set_facecolor("#faf6f0")
    ax.set_facecolor("#faf6f0")

    bars = ax.barh(
        merged["short_name"], merged["speedup"],
        color=colors, height=0.6, edgecolor="none",
    )

    ax.axvline(1.0, color="#e57373", linestyle="--", linewidth=2.5, zorder=3)

    for bar in bars:
        val = bar.get_width()
        ax.text(
            val - 0.15, bar.get_y() + bar.get_height() / 2,
            f"{val:.2f}x", va="center", ha="right",
            fontsize=12, fontweight="bold", color="white",
        )

    ax.set_xlabel(r"Speedup Factor ($T_{base}/T_{opt}$)", fontsize=13)
    ax.set_title("AutoOMPPass Optimization Impact", fontsize=16, pad=15)
    ax.tick_params(axis="y", labelsize=13)
    ax.grid(axis="x", color="#cccccc", linewidth=0.7)
    ax.set_axisbelow(True)
    ax.spines[["top", "right", "bottom"]].set_visible(False)

    plt.tight_layout()
    plt.savefig(out_path, bbox_inches="tight", facecolor=fig.get_facecolor())
    print(f"Chart saved to {out_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline")
    parser.add_argument("optimized")
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args()

    plot_comparison(load_bench_csv(args.baseline), load_bench_csv(args.optimized), args.output)
