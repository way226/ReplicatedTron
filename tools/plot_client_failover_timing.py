#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path


REQUIRED_COLUMNS = {
    "sample_index",
    "observer_id",
    "source_label",
    "epoch",
    "tick",
    "published_tick",
    "safe_tick",
    "due_at_ms",
    "arrived_at_ms",
    "interarrival_ms",
    "tick_gap",
    "epoch_changed",
    "tick_lateness_ms",
}


def load_rows(csv_path: Path):
    with csv_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        missing = REQUIRED_COLUMNS.difference(reader.fieldnames or [])
        if missing:
            raise SystemExit(
                f"{csv_path} is missing required columns: {', '.join(sorted(missing))}"
            )

        rows = []
        for row in reader:
            rows.append(
                {
                    "sample_index": int(row["sample_index"]),
                    "observer_id": row["observer_id"],
                    "source_label": row["source_label"],
                    "epoch": int(row["epoch"]),
                    "tick": int(row["tick"]),
                    "published_tick": int(row["published_tick"]),
                    "safe_tick": int(row["safe_tick"]),
                    "due_at_ms": float(row["due_at_ms"]),
                    "arrived_at_ms": float(row["arrived_at_ms"]),
                    "interarrival_ms": float(row["interarrival_ms"]),
                    "tick_gap": int(row["tick_gap"]),
                    "epoch_changed": int(row["epoch_changed"]),
                    "tick_lateness_ms": float(row["tick_lateness_ms"]),
                }
            )

    if not rows:
        raise SystemExit(f"{csv_path} does not contain any authoritative frame samples.")

    return rows


def default_output_path(csv_path: Path) -> Path:
    return csv_path.with_suffix(".png")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plot client-visible authoritative frame timing across failover."
    )
    parser.add_argument("csv_path", help="Path to a client authoritative timing CSV file.")
    parser.add_argument(
        "--output",
        help="Optional output image path. Defaults to the CSV path with a .png suffix.",
    )
    return parser


def main():
    args = build_parser().parse_args()
    csv_path = Path(args.csv_path)
    if not csv_path.is_file():
        raise SystemExit(f"CSV file not found: {csv_path}")

    rows = load_rows(csv_path)

    try:
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit(
            "matplotlib is required to render the graph. Install it with "
            "`python3 -m pip install matplotlib`."
        ) from exc

    output_path = Path(args.output) if args.output else default_output_path(csv_path)

    arrival_seconds = [row["arrived_at_ms"] / 1000.0 for row in rows]
    lateness = [row["tick_lateness_ms"] for row in rows]
    interarrival = [row["interarrival_ms"] for row in rows]
    epoch_change_rows = [row for row in rows if row["epoch_changed"] == 1]

    fig, (ax_lateness, ax_gap) = plt.subplots(2, 1, figsize=(11, 7), sharex=True)

    ax_lateness.plot(arrival_seconds, lateness, color="#1f77b4", marker="o", linewidth=1.3, markersize=3)
    ax_lateness.set_ylabel("Authoritative lateness (ms)")
    ax_lateness.set_title(
        f"Client-visible failover timing for {rows[0]['observer_id']}"
    )
    ax_lateness.grid(True, alpha=0.3, linewidth=0.8)

    ax_gap.plot(arrival_seconds, interarrival, color="#d62728", marker="o", linewidth=1.3, markersize=3)
    ax_gap.axhline(150.0, color="#222222", linewidth=1.0, linestyle="--")
    ax_gap.set_xlabel("Client arrival time since first authoritative frame (s)")
    ax_gap.set_ylabel("Inter-arrival gap (ms)")
    ax_gap.grid(True, alpha=0.3, linewidth=0.8)

    for index, row in enumerate(epoch_change_rows):
        x_value = row["arrived_at_ms"] / 1000.0
        for axis in (ax_lateness, ax_gap):
            axis.axvline(x_value, color="#2ca02c", linestyle="--", linewidth=1.2, alpha=0.85)
        label = f"epoch {row['epoch']} tick {row['tick']} via {row['source_label']}"
        ax_lateness.text(
            x_value,
            max(lateness) if lateness else 0.0,
            label,
            rotation=90,
            va="top",
            ha="right",
            fontsize=8,
            color="#2ca02c",
        )

    fig.tight_layout()
    fig.savefig(output_path, dpi=160)

    max_lateness = max(lateness)
    max_interarrival = max(interarrival)
    print(f"Wrote graph to {output_path}")
    print(f"Samples: {len(rows)}")
    print(f"Max authoritative lateness: {max_lateness:.2f} ms")
    print(f"Max inter-arrival gap: {max_interarrival:.2f} ms")
    if epoch_change_rows:
        first_change = epoch_change_rows[0]
        print(
            "First epoch change: "
            f"tick {first_change['tick']}, epoch {first_change['epoch']}, "
            f"source {first_change['source_label']}"
        )


if __name__ == "__main__":
    main()
