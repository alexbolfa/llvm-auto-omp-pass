# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib", "pandas", "seaborn", "numpy"]
# ///

import argparse
import sys
from io import StringIO
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns

# Using a cleaner base style
plt.style.use("dark_background")  # Matches dark Slidev themes better


def plot_comparison(df_base, df_opt, out_path):
    merged = df_base.merge(df_opt, on="short_name", suffixes=("_base", "_opt"))
    merged["speedup"] = merged["cpu_time_base"] / merged["cpu_time_opt"]
    merged = merged.sort_values("speedup", ascending=True)

    # 1. Setup Figure (Higher DPI for crispness)
    fig, ax = plt.subplots(figsize=(12, 7), dpi=200)
    fig.patch.set_facecolor("#0f172a")  # Deep Slate background
    ax.set_facecolor("#0f172a")

    # 2. Dynamic Colors: Teal for win, Coral for loss
    colors = ["#2dd4bf" if s >= 1.0 else "#fb7185" for s in merged["speedup"]]

    y_pos = np.arange(len(merged))
    bars = ax.barh(
        y_pos, merged["speedup"], color=colors, height=0.7, edgecolors="none"
    )

    # 3. The "Baseline" Ghost
    ax.axvline(1.0, color="#94a3b8", linestyle=":", linewidth=2, alpha=0.5)
    ax.text(
        1.0,
        -0.8,
        "BASELINE",
        color="#94a3b8",
        fontsize=9,
        ha="center",
        fontweight="bold",
    )

    # 4. Beautiful Labels
    for i, bar in enumerate(bars):
        val = bar.get_width()
        # White text for speedup
        ax.text(
            val + 0.1,
            bar.get_y() + bar.get_height() / 2,
            f"{val:.2f}x",
            va="center",
            ha="left",
            fontsize=13,
            color=colors[i],
            fontweight="bold",
        )

        # Add "Time Saved" label in dim text
        time_diff = (
            merged.iloc[i]["cpu_time_base"] - merged.iloc[i]["cpu_time_opt"]
        ) / 1e6
        if val > 1.0:
            ax.text(
                val + 0.9,
                bar.get_y() + bar.get_height() / 2,
                f"({time_diff:.1f}ms saved)",
                va="center",
                ha="left",
                fontsize=10,
                color="#64748b",
            )

    # 5. Professional Spacing & Labels
    ax.set_yticks(y_pos)
    ax.set_yticklabels(
        merged["short_name"], fontsize=12, color="#f1f5f9", fontweight="medium"
    )

    # Title & Subtitle
    plt.title(
        "AutoOMPPass: Execution Speedup",
        loc="left",
        pad=30,
        fontsize=20,
        color="#f1f5f9",
        fontweight="bold",
    )
    ax.text(
        0,
        1.05,
        "Relative performance of parallelized loops vs sequential baseline",
        transform=ax.transAxes,
        color="#94a3b8",
        fontsize=12,
    )

    # 6. Remove Clutter
    ax.set_xlabel("")  # Speedup is obvious from labels
    ax.set_xticks([])  # Remove x-axis numbers
    sns.despine(left=True, bottom=True)

    plt.tight_layout()
    plt.savefig(out_path, facecolor=fig.get_facecolor(), bbox_inches="tight")
    print(f"✨ Beautiful chart saved to {out_path}")
