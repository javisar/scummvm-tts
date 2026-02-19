#!/usr/bin/env python3
"""Classify Indy3 rejected print candidates into actionable buckets.

This script reads `rejected_candidates_with_reasons.json` produced by
`indy3_dialogue_extract_v4_reasons` and adds a triage category for each row.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple


CATEGORY_RECOVERABLE = "recoverable_parser_gap"
CATEGORY_TEXTLESS = "textless_valid"
CATEGORY_FALSE_POSITIVE = "likely_false_positive"
CATEGORY_MISALIGNED_OR_TRUNCATED = "likely_misaligned_or_truncated"
CATEGORY_UNKNOWN = "unknown_needs_review"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Classify rejected print candidates from "
            "rejected_candidates_with_reasons.json"
        )
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Path to rejected_candidates_with_reasons.json",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        help="Output directory for triage_summary.* and triage_candidates.json",
    )
    parser.add_argument(
        "--tail-threshold",
        type=int,
        default=2,
        help=(
            "Treat *_oob near end-of-source as likely truncation if "
            "source_len - reject_cursor <= threshold (default: 2)"
        ),
    )
    return parser.parse_args()


def _to_int(value: Any, default: int = 0) -> int:
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 10)
        except ValueError:
            return default
    return default


def classify_candidate(
    candidate: Dict[str, Any], tail_threshold: int
) -> Dict[str, Any]:
    reason = str(candidate.get("reason", ""))
    source_len = _to_int(candidate.get("source_len"), default=0)
    reject_cursor = _to_int(candidate.get("reject_cursor"), default=0)
    tail_bytes = max(source_len - reject_cursor, 0)

    category = CATEGORY_UNKNOWN
    confidence = "low"
    rationale = "No classification rule matched"

    if reason in {"unsupported_cmd_nibble", "diagnostic_ok_unexpected"}:
        category = CATEGORY_RECOVERABLE
        confidence = "high" if reason == "unsupported_cmd_nibble" else "medium"
        rationale = "Parser does not currently model this print command path"
    elif reason == "terminator_before_text":
        category = CATEGORY_TEXTLESS
        confidence = "high"
        rationale = "Print command sequence terminates before SO_TEXTSTRING"
    elif reason == "reached_end_without_text":
        category = CATEGORY_FALSE_POSITIVE
        confidence = "medium"
        rationale = "Candidate scan hit source end without encountering text"
    elif reason.startswith("text_len_"):
        category = CATEGORY_MISALIGNED_OR_TRUNCATED
        confidence = "medium"
        rationale = "String length decode failed (either malformed string or misaligned parse start)"
    elif reason.endswith("_arg_oob") or reason == "opcode_offset_oob":
        if tail_bytes <= tail_threshold:
            category = CATEGORY_MISALIGNED_OR_TRUNCATED
            confidence = "medium"
            rationale = "Argument decode ran out of bytes near source boundary"
        else:
            category = CATEGORY_FALSE_POSITIVE
            confidence = "medium"
            rationale = "Argument decode overflowed away from boundary; likely misaligned candidate"

    result = dict(candidate)
    result["triage_category"] = category
    result["triage_confidence"] = confidence
    result["triage_rationale"] = rationale
    result["tail_bytes_after_reject_cursor"] = tail_bytes
    result["recoverable"] = category == CATEGORY_RECOVERABLE
    return result


def load_rejected_candidates(path: Path) -> List[Dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, list):
        raise ValueError("Input JSON must be an array")
    out: List[Dict[str, Any]] = []
    for idx, row in enumerate(data):
        if not isinstance(row, dict):
            raise ValueError(f"Input row {idx} is not an object")
        out.append(row)
    return out


def _sorted_counter(counter: Counter[str]) -> List[Tuple[str, int]]:
    return sorted(counter.items(), key=lambda item: (-item[1], item[0]))


def build_summary(
    candidates: Iterable[Dict[str, Any]], tail_threshold: int
) -> Dict[str, Any]:
    category_counts: Counter[str] = Counter()
    reason_counts: Counter[str] = Counter()
    category_reason_counts: Counter[str] = Counter()
    opcode_counts: Counter[str] = Counter()
    source_kind_counts: Counter[str] = Counter()

    total = 0
    for cand in candidates:
        total += 1
        category = str(cand.get("triage_category", CATEGORY_UNKNOWN))
        reason = str(cand.get("reason", ""))
        opcode = str(cand.get("opcode", ""))
        source_kind = str(cand.get("source_kind", ""))

        category_counts[category] += 1
        reason_counts[reason] += 1
        category_reason_counts[f"{category}|{reason}"] += 1
        opcode_counts[opcode] += 1
        source_kind_counts[source_kind] += 1

    recoverable = category_counts[CATEGORY_RECOVERABLE]
    summary: Dict[str, Any] = {
        "total_candidates": total,
        "tail_threshold": tail_threshold,
        "recoverable_candidates": recoverable,
        "recoverable_ratio": (float(recoverable) / float(total)) if total else 0.0,
        "counts_by_category": {k: v for k, v in _sorted_counter(category_counts)},
        "counts_by_reason": {k: v for k, v in _sorted_counter(reason_counts)},
        "counts_by_opcode": {k: v for k, v in _sorted_counter(opcode_counts)},
        "counts_by_source_kind": {k: v for k, v in _sorted_counter(source_kind_counts)},
        "counts_by_category_and_reason": {
            k: v for k, v in _sorted_counter(category_reason_counts)
        },
    }
    return summary


def write_json(path: Path, payload: Any) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=True, indent=2)
        handle.write("\n")


def write_summary_txt(path: Path, summary: Dict[str, Any]) -> None:
    lines: List[str] = []
    lines.append(f"total_candidates\t{summary['total_candidates']}")
    lines.append(f"tail_threshold\t{summary['tail_threshold']}")
    lines.append(f"recoverable_candidates\t{summary['recoverable_candidates']}")
    lines.append(f"recoverable_ratio\t{summary['recoverable_ratio']:.6f}")
    lines.append("")

    lines.append("category\tcount")
    for name, count in summary["counts_by_category"].items():
        lines.append(f"{name}\t{count}")
    lines.append("")

    lines.append("reason\tcount")
    for name, count in summary["counts_by_reason"].items():
        lines.append(f"{name}\t{count}")
    lines.append("")

    lines.append("category|reason\tcount")
    for name, count in summary["counts_by_category_and_reason"].items():
        lines.append(f"{name}\t{count}")

    with path.open("w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)
    out_dir = Path(args.out_dir)

    rejected = load_rejected_candidates(input_path)
    triaged = [classify_candidate(row, args.tail_threshold) for row in rejected]

    summary = build_summary(triaged, args.tail_threshold)
    summary["input_file"] = str(input_path)

    out_dir.mkdir(parents=True, exist_ok=True)
    write_json(out_dir / "triage_candidates.json", triaged)
    write_json(out_dir / "triage_summary.json", summary)
    write_summary_txt(out_dir / "triage_summary.txt", summary)

    print(f"Wrote {out_dir / 'triage_candidates.json'}")
    print(f"Wrote {out_dir / 'triage_summary.json'}")
    print(f"Wrote {out_dir / 'triage_summary.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
