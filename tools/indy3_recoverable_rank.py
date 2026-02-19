#!/usr/bin/env python3
"""Rank recoverable Indy3 rejected print command patterns.

Input should typically be `triage_candidates.json` produced by
`indy3_rejection_triage.py`. The script focuses on recoverable cases and ranks
`reject_cmd` patterns to guide parser support priorities.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, DefaultDict, Dict, Iterable, List, Optional, Tuple


RECOVERABLE_CATEGORY = "recoverable_parser_gap"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Rank recoverable reject_cmd patterns for Indy3 extractor"
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Path to triage_candidates.json (or compatible rejected-candidate JSON)",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        help="Output directory for recoverable_rank_summary.* and recoverable_patterns.json",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        help="How many top reject_cmd patterns to include in text summary (default: 20)",
    )
    parser.add_argument(
        "--examples-per-cmd",
        type=int,
        default=3,
        help="How many sample locations to keep per reject_cmd pattern (default: 3)",
    )
    return parser.parse_args()


def load_rows(path: Path) -> List[Dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, list):
        raise ValueError("Input JSON must be an array")

    rows: List[Dict[str, Any]] = []
    for idx, row in enumerate(data):
        if not isinstance(row, dict):
            raise ValueError(f"Input row {idx} is not an object")
        rows.append(row)
    return rows


def is_recoverable(row: Dict[str, Any]) -> bool:
    category = str(row.get("triage_category", ""))
    if category == RECOVERABLE_CATEGORY:
        return True
    if bool(row.get("recoverable", False)):
        return True
    return str(row.get("reason", "")) == "unsupported_cmd_nibble"


def parse_hex_byte(value: Any) -> Optional[int]:
    if isinstance(value, int):
        if 0 <= value <= 0xFF:
            return value
        return None
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text:
        return None
    if text.startswith("0x") or text.startswith("0X"):
        text = text[2:]
    try:
        num = int(text, 16)
    except ValueError:
        return None
    if 0 <= num <= 0xFF:
        return num
    return None


def hex_byte(value: int) -> str:
    return f"0x{value:02X}"


def _sorted_counter(counter: Counter[str]) -> List[Tuple[str, int]]:
    return sorted(counter.items(), key=lambda item: (-item[1], item[0]))


def _sample_from_row(row: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "candidate_idx": row.get("candidate_idx"),
        "lfl_file": row.get("lfl_file"),
        "source_kind": row.get("source_kind"),
        "room": row.get("room"),
        "script": row.get("script"),
        "object": row.get("object"),
        "verb": row.get("verb"),
        "source_base": row.get("source_base"),
        "source_len": row.get("source_len"),
        "opcode": row.get("opcode"),
        "opcode_offset": row.get("opcode_offset"),
        "reject_cursor": row.get("reject_cursor"),
        "reject_cmd": row.get("reject_cmd"),
    }


def build_pattern_rows(
    recoverable_rows: Iterable[Dict[str, Any]], examples_per_cmd: int
) -> List[Dict[str, Any]]:
    per_cmd_rows: DefaultDict[str, List[Dict[str, Any]]] = defaultdict(list)
    for row in recoverable_rows:
        cmd = str(row.get("reject_cmd", ""))
        if not cmd:
            cmd = "<missing>"
        per_cmd_rows[cmd].append(row)

    pattern_rows: List[Dict[str, Any]] = []
    for cmd, rows in per_cmd_rows.items():
        opcode_counts: Counter[str] = Counter()
        source_kind_counts: Counter[str] = Counter()
        file_counts: Counter[str] = Counter()
        for row in rows:
            opcode_counts[str(row.get("opcode", ""))] += 1
            source_kind_counts[str(row.get("source_kind", ""))] += 1
            file_counts[str(row.get("lfl_file", ""))] += 1

        cmd_value = parse_hex_byte(cmd)
        cmd_nibble = (
            hex_byte(cmd_value & 0x0F) if cmd_value is not None else "<unknown>"
        )
        cmd_high = hex_byte(cmd_value & 0xF0) if cmd_value is not None else "<unknown>"

        sorted_rows = sorted(
            rows,
            key=lambda r: (
                str(r.get("lfl_file", "")),
                int(r.get("source_base", 0) or 0),
                int(r.get("opcode_offset", 0) or 0),
                int(r.get("candidate_idx", 0) or 0),
            ),
        )
        samples = [_sample_from_row(r) for r in sorted_rows[:examples_per_cmd]]

        pattern_rows.append(
            {
                "reject_cmd": cmd,
                "reject_cmd_value": cmd_value,
                "reject_cmd_low_nibble": cmd_nibble,
                "reject_cmd_high_bits": cmd_high,
                "count": len(rows),
                "unique_lfl_files": len(file_counts),
                "unique_source_kinds": len(source_kind_counts),
                "counts_by_opcode": {k: v for k, v in _sorted_counter(opcode_counts)},
                "counts_by_source_kind": {
                    k: v for k, v in _sorted_counter(source_kind_counts)
                },
                "samples": samples,
            }
        )

    pattern_rows.sort(
        key=lambda row: (
            -int(row["count"]),
            -int(row["unique_source_kinds"]),
            -int(row["unique_lfl_files"]),
            str(row["reject_cmd"]),
        )
    )
    return pattern_rows


def build_summary(
    all_rows: List[Dict[str, Any]],
    recoverable_rows: List[Dict[str, Any]],
    pattern_rows: List[Dict[str, Any]],
    top_n: int,
) -> Dict[str, Any]:
    reason_counts: Counter[str] = Counter()
    opcode_counts: Counter[str] = Counter()
    source_kind_counts: Counter[str] = Counter()
    nibble_counts: Counter[str] = Counter()
    high_bits_counts: Counter[str] = Counter()

    for row in recoverable_rows:
        reason_counts[str(row.get("reason", ""))] += 1
        opcode_counts[str(row.get("opcode", ""))] += 1
        source_kind_counts[str(row.get("source_kind", ""))] += 1
        cmd_value = parse_hex_byte(row.get("reject_cmd"))
        if cmd_value is not None:
            nibble_counts[hex_byte(cmd_value & 0x0F)] += 1
            high_bits_counts[hex_byte(cmd_value & 0xF0)] += 1
        else:
            nibble_counts["<unknown>"] += 1
            high_bits_counts["<unknown>"] += 1

    total_rejected = len(all_rows)
    total_recoverable = len(recoverable_rows)
    running = 0
    top_patterns: List[Dict[str, Any]] = []
    for row in pattern_rows[:top_n]:
        running += int(row["count"])
        top_patterns.append(
            {
                "reject_cmd": row["reject_cmd"],
                "count": row["count"],
                "share_of_recoverable": (
                    float(row["count"]) / float(total_recoverable)
                    if total_recoverable
                    else 0.0
                ),
                "cumulative_share_of_recoverable": (
                    float(running) / float(total_recoverable)
                    if total_recoverable
                    else 0.0
                ),
                "unique_lfl_files": row["unique_lfl_files"],
                "unique_source_kinds": row["unique_source_kinds"],
            }
        )

    return {
        "total_rejected_candidates": total_rejected,
        "total_recoverable_candidates": total_recoverable,
        "recoverable_ratio": (
            float(total_recoverable) / float(total_rejected) if total_rejected else 0.0
        ),
        "unique_recoverable_reject_cmds": len(pattern_rows),
        "counts_by_reason": {k: v for k, v in _sorted_counter(reason_counts)},
        "counts_by_opcode": {k: v for k, v in _sorted_counter(opcode_counts)},
        "counts_by_source_kind": {k: v for k, v in _sorted_counter(source_kind_counts)},
        "counts_by_cmd_low_nibble": {k: v for k, v in _sorted_counter(nibble_counts)},
        "counts_by_cmd_high_bits": {k: v for k, v in _sorted_counter(high_bits_counts)},
        "top_patterns": top_patterns,
    }


def write_json(path: Path, payload: Any) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=True, indent=2)
        handle.write("\n")


def write_summary_txt(
    path: Path, summary: Dict[str, Any], pattern_rows: List[Dict[str, Any]], top_n: int
) -> None:
    lines: List[str] = []
    lines.append(f"total_rejected_candidates\t{summary['total_rejected_candidates']}")
    lines.append(
        f"total_recoverable_candidates\t{summary['total_recoverable_candidates']}"
    )
    lines.append(f"recoverable_ratio\t{summary['recoverable_ratio']:.6f}")
    lines.append(
        f"unique_recoverable_reject_cmds\t{summary['unique_recoverable_reject_cmds']}"
    )
    lines.append("")

    lines.append("recoverable_by_opcode\tcount")
    for name, count in summary["counts_by_opcode"].items():
        lines.append(f"{name}\t{count}")
    lines.append("")

    lines.append("recoverable_by_source_kind\tcount")
    for name, count in summary["counts_by_source_kind"].items():
        lines.append(f"{name}\t{count}")
    lines.append("")

    lines.append("recoverable_by_cmd_low_nibble\tcount")
    for name, count in summary["counts_by_cmd_low_nibble"].items():
        lines.append(f"{name}\t{count}")
    lines.append("")

    lines.append(
        "top_recoverable_reject_cmd\tcount\tshare\tcumulative_share\tunique_lfl\tunique_source_kinds"
    )
    running = 0
    total_recoverable = int(summary["total_recoverable_candidates"])
    for row in pattern_rows[:top_n]:
        count = int(row["count"])
        running += count
        share = (float(count) / float(total_recoverable)) if total_recoverable else 0.0
        cumulative = (
            (float(running) / float(total_recoverable)) if total_recoverable else 0.0
        )
        lines.append(
            "{}\t{}\t{:.6f}\t{:.6f}\t{}\t{}".format(
                row["reject_cmd"],
                count,
                share,
                cumulative,
                row["unique_lfl_files"],
                row["unique_source_kinds"],
            )
        )

    with path.open("w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)
    out_dir = Path(args.out_dir)

    all_rows = load_rows(input_path)
    recoverable_rows = [row for row in all_rows if is_recoverable(row)]
    pattern_rows = build_pattern_rows(recoverable_rows, args.examples_per_cmd)
    summary = build_summary(all_rows, recoverable_rows, pattern_rows, args.top)
    summary["input_file"] = str(input_path)

    out_dir.mkdir(parents=True, exist_ok=True)
    write_json(out_dir / "recoverable_rank_summary.json", summary)
    write_json(out_dir / "recoverable_patterns.json", pattern_rows)
    write_summary_txt(
        out_dir / "recoverable_rank_summary.txt",
        summary,
        pattern_rows,
        args.top,
    )

    print(f"Wrote {out_dir / 'recoverable_rank_summary.json'}")
    print(f"Wrote {out_dir / 'recoverable_patterns.json'}")
    print(f"Wrote {out_dir / 'recoverable_rank_summary.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
