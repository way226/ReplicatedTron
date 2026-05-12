#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path


REQUIRED_COLUMNS = {
    "sample_index",
    "leader_role",
    "leader_id",
    "epoch",
    "tick",
    "due_at_ms",
    "tick_started_at_ms",
    "publish_at_ms",
    "tick_runtime_ms",
    "tick_lateness_ms",
}


def load_rows(csv_path: Path):
    with csv_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        missing = REQUIRED_COLUMNS.difference(reader.fieldnames or [])
        if missing:
            missing_text = ", ".join(sorted(missing))
            raise SystemExit(f"{csv_path} is missing required columns: {missing_text}")

        rows = []
        for row in reader:
            rows.append(
                {
                    "sample_index": int(row["sample_index"]),
                    "leader_role": row["leader_role"],
                    "leader_id": row["leader_id"],
                    "epoch": int(row["epoch"]),
                    "tick": int(row["tick"]),
                    "due_at_ms": float(row["due_at_ms"]),
                    "publish_at_ms": float(row["publish_at_ms"]),
                    "tick_runtime_ms": float(row["tick_runtime_ms"]),
                    "tick_lateness_ms": float(row["tick_lateness_ms"]),
                }
            )

    if not rows:
        raise SystemExit(f"{csv_path} does not contain any tick samples.")

    return rows


def default_output_path(csv_path: Path) -> Path:
    return csv_path.with_suffix(".png")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Graph per-tick lateness from a server tick-lateness CSV log."
    )
    parser.add_argument("csv_path", help="Path to a tick-lateness CSV file.")
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

    x_values = [row["due_at_ms"] / 1000.0 for row in rows]
    y_values = [row["tick_lateness_ms"] for row in rows]

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(x_values, y_values, color="#1f77b4", marker="o", linewidth=1.4, markersize=3.2)
    ax.axhline(0.0, color="#222222", linewidth=1.0)
    ax.set_xlabel("Scheduled publish time since leadership start (s)")
    ax.set_ylabel("Lateness (ms)")

    leader_label = f"{rows[0]['leader_role']} @ {rows[0]['leader_id']}"
    epoch_values = sorted({row["epoch"] for row in rows})
    if len(epoch_values) == 1:
        title = f"Per-tick lateness for {leader_label} (epoch {epoch_values[0]})"
    else:
        title = f"Per-tick lateness for {leader_label}"
    ax.set_title(title)

    ax.grid(True, alpha=0.3, linewidth=0.8)
    fig.tight_layout()
    fig.savefig(output_path, dpi=160)

    max_lateness = max(y_values)
    average_lateness = sum(y_values) / len(y_values)
    print(f"Wrote graph to {output_path}")
    print(f"Samples: {len(rows)}")
    print(f"Max lateness: {max_lateness:.2f} ms")
    print(f"Average lateness: {average_lateness:.2f} ms")


if __name__ == "__main__":
    main()
