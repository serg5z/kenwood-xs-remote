#!/usr/bin/env python3
"""Read-only report for xs8_sniff --event-order CSV; never sorts records."""
import argparse
import csv
import json


class Sequence:
    def __init__(self):
        self.highest = self.last = 0
        self.gaps = self.initially_missing = self.late = self.regressions = 0
        self.unresolved = 0
        self.holes = []
        self.observations = 0
        self.initial_missing_ranges = []
        self.late_numbers = []

    def add(self, number):
        self.last = number
        self.observations += 1
        if number > self.highest:
            if number > self.highest + 1:
                self.holes.append((self.highest + 1, number - 1))
                self.initial_missing_ranges.append((self.highest + 1, number - 1))
                self.gaps += 1
                self.initially_missing += number - self.highest - 1
                self.unresolved += number - self.highest - 1
            self.highest = number
            return
        for index, (first, last) in enumerate(self.holes):
            if first <= number <= last:
                remaining = []
                if first < number:
                    remaining.append((first, number - 1))
                if number < last:
                    remaining.append((number + 1, last))
                self.holes[index:index + 1] = remaining
                self.late += 1
                self.late_numbers.append(number)
                self.unresolved -= 1
                return
        self.regressions += 1

    def report(self):
        return dict(observations=self.observations, highest=self.highest,
                    last=self.last, gaps=self.gaps,
                    initially_missing=self.initially_missing,
                    late_arrivals=self.late, unresolved_at_end=self.unresolved,
                    regressions_or_duplicates=self.regressions,
                    unresolved_ranges=self.holes,
                    initial_missing_ranges=self.initial_missing_ranges,
                    late_numbers=self.late_numbers,
                    continuous=not (self.gaps or self.regressions)
                    if self.observations else None)


def analyze(stream, line, expected):
    selected = {"data": 13, "busy": 26}[line]
    counts = {13: 0, 26: 0}
    edges = {13: {"rising": 0, "falling": 0},
             26: {"rising": 0, "falling": 0}}
    per_line = {13: Sequence(), 26: Sequence()}
    global_sequence = Sequence()
    timestamps = 0
    last_timestamp = None
    records = csv.DictReader(stream)
    required = {"timestamp_ns", "gpio", "edge", "global_seqno", "line_seqno",
                "batch", "batch_index", "batch_size", "read_ns"}
    if not required.issubset(records.fieldnames or []):
        raise ValueError("Expected xs8_sniff --event-order CSV")
    for row in records:
        gpio = int(row["gpio"])
        if gpio not in counts or row["edge"] not in edges[gpio]:
            raise ValueError("Unexpected GPIO or edge type")
        timestamp = int(row["timestamp_ns"])
        global_no, line_no = int(row["global_seqno"]), int(row["line_seqno"])
        if timestamp < 0 or not (0 <= global_no <= 0xffffffff and
                                 0 <= line_no <= 0xffffffff):
            raise ValueError("Invalid timestamp or sequence")
        if last_timestamp is not None and timestamp < last_timestamp:
            timestamps += 1
        last_timestamp = timestamp
        counts[gpio] += 1
        edges[gpio][row["edge"]] += 1
        global_sequence.add(global_no)
        per_line[gpio].add(line_no)
    return {
        "selected_line": line,
        "expectation": "Requested count assumes completed TX and ideal loopback; "
                       "counts include the entire capture and boundary activity.",
        "lines": {str(gpio): {
            "expected_if_completed": expected if gpio == selected else 0,
            "observed": counts[gpio],
            "difference": counts[gpio] - (expected if gpio == selected else 0),
            "edges": edges[gpio],
            "sequence": per_line[gpio].report(),
        } for gpio in counts},
        "global_sequence": global_sequence.report(),
        "unexpected_inactive_line_events": counts[26 if selected == 13 else 13],
        "timestamp_regressions": timestamps,
        "limitations": "No receiver execution or physical edge loss inferred. "
                       "Check TX/RX logs for incomplete capture, failed TX or "
                       "cleanup; reset/wrap is not reconstructed.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--line", required=True, choices=("data", "busy"))
    parser.add_argument("--expected", required=True, type=int,
                        help="Requested number of selected-line transitions")
    parser.add_argument("capture")
    args = parser.parse_args()
    if args.expected < 2 or args.expected > 10000 or args.expected % 2:
        parser.error("--expected must be even, 2..10000")
    try:
        with open(args.capture, newline="", encoding="utf-8") as stream:
            report = analyze(stream, args.line, args.expected)
    except (OSError, ValueError, TypeError, csv.Error) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
