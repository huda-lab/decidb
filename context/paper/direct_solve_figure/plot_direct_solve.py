"""Draws the paper's query-rewrite figure from the recorded benchmark CSV.

One column per query: median wall time on top, peak memory below. The CSV beside this script is a copy of
benchmark/decide/results/direct_solve/direct_solve_20261102T124429Z.csv; that folder's README describes it. Writes
experiment.pdf beside this script. Needs matplotlib and a TeX install that has Linux Libertine.
"""

import csv
import subprocess
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
from matplotlib import font_manager
import matplotlib.pyplot as plt

OUT_DIR = Path(__file__).resolve().parent
CSV_PATH = OUT_DIR / "direct_solve_20261102T124429Z.csv"

QUERIES = [
	("s1", "Q1: per-group selection"),
	("r1", "Q2: per-group budget"),
	("s1_global", "Q3: one-limit selection"),
	("r1_global", "Q4: one-limit budget"),
	("a1", "Q5: independent ranges"),
]

font_manager.fontManager.addfont(subprocess.check_output(["kpsewhich", "LinLibertine_R.otf"], text=True).strip())
plt.rcParams.update({"font.family": "Linux Libertine O", "font.size": 8, "pdf.fonttype": 42})


def load():
	rows = {}
	for row in csv.DictReader(CSV_PATH.open()):
		rows.setdefault(row["query"], []).append(row)
	for series in rows.values():
		series.sort(key=lambda r: float(r["decisions"]))
	return rows


def column(series, name, scale=1.0):
	return [float(r[name]) * scale for r in series]


def main():
	rows = load()
	fig, axes = plt.subplots(2, len(QUERIES), figsize=(7.0, 3.2), sharex=True, sharey="row")

	for col, (query, title) in enumerate(QUERIES):
		series = rows[query]
		x = column(series, "decisions")
		for ax, gurobi, rewrite in (
			(axes[0, col], column(series, "gurobi median s"), column(series, "direct median s")),
			(axes[1, col], column(series, "gurobi peak MiB", 1 / 1024), column(series, "direct peak MiB", 1 / 1024)),
		):
			ax.plot(x, gurobi, "s-", color="tab:orange", markersize=2.5, linewidth=1, label="Gurobi (up to 32 threads)")
			ax.plot(x, rewrite, "o-", color="tab:blue", markersize=2.5, linewidth=1, label="Rewrite (160 threads)")
			ax.set_xscale("log")
			ax.set_yscale("log")
			ax.grid(True, linewidth=0.3)
		axes[0, col].set_title(title, fontsize=7.5)
		axes[1, col].set_xlabel("Decisions")

	axes[1, 0].set_xlim(7e4, 3e8)
	axes[1, 0].set_xticks([1e5, 1e6, 1e7, 1e8], ["100K", "1M", "10M", "100M"])
	axes[0, 0].set_ylim(0.05, 5000)
	axes[0, 0].set_yticks([0.1, 1, 10, 100, 1000], ["0.1", "1", "10", "100", "1000"])
	axes[1, 0].set_ylim(0.02, 400)
	axes[1, 0].set_yticks([0.1, 1, 10, 100], ["0.1", "1", "10", "100"])
	axes[0, 0].set_ylabel("Time (s)")
	axes[1, 0].set_ylabel("Peak memory (GiB)")
	fig.legend(*axes[0, 0].get_legend_handles_labels(), loc="upper center", ncol=2, frameon=False)
	fig.subplots_adjust(left=0.075, right=0.99, top=0.87, bottom=0.12, wspace=0.1, hspace=0.15)

	fig.savefig(OUT_DIR / "experiment.pdf")


if __name__ == "__main__":
	main()
