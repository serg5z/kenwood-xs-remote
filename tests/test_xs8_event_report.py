import io
import unittest

from xs8_event_report import analyze


def capture(events):
    header = ("timestamp_ns,gpio,edge,global_seqno,line_seqno,batch,"
              "batch_index,batch_size,read_ns,sequence_status\n")
    return io.StringIO(header + "".join(
        f"{time},{gpio},{edge},{glob},{line},{i+1},0,1,{time+100},IGNORED\n"
        for i, (time, gpio, edge, glob, line) in enumerate(events)))


class EventReportTests(unittest.TestCase):
    def test_each_line(self):
        for name, gpio in (("data", 13), ("busy", 26)):
            with self.subTest(name=name):
                report = analyze(capture([
                    (100, gpio, "rising", 1, 1),
                    (200, gpio, "falling", 2, 2)]), name, 2)
                self.assertEqual(report["lines"][str(gpio)]["observed"], 2)
                self.assertEqual(report["lines"][str(gpio)]["difference"], 0)
                self.assertTrue(report["global_sequence"]["continuous"])
                self.assertTrue(report["lines"][str(gpio)]["sequence"]["continuous"])
                self.assertEqual(report["unexpected_inactive_line_events"], 0)

    def test_global_gap_with_contiguous_line_counter(self):
        report = analyze(capture([
            (100, 13, "rising", 1, 1),
            (200, 13, "falling", 3, 2)]), "data", 2)
        self.assertEqual(report["global_sequence"]["unresolved_ranges"], [(2, 2)])
        self.assertEqual(report["global_sequence"]["unresolved_at_end"], 1)
        self.assertTrue(report["lines"]["13"]["sequence"]["continuous"])
        self.assertEqual(report["lines"]["13"]["difference"], 0)

    def test_late_numbers_across_batches_and_timestamp_regression(self):
        report = analyze(capture([
            (100, 26, "rising", 1, 1),
            (300, 26, "falling", 3, 3),
            (200, 26, "rising", 2, 2)]), "busy", 4)
        for sequence in (report["global_sequence"],
                         report["lines"]["26"]["sequence"]):
            self.assertEqual(sequence["late_arrivals"], 1)
            self.assertEqual(sequence["late_numbers"], [2])
            self.assertEqual(sequence["unresolved_at_end"], 0)
            self.assertFalse(sequence["continuous"])
        self.assertEqual(report["timestamp_regressions"], 1)

    def test_inactive_activity_and_line_gap(self):
        report = analyze(capture([
            (100, 13, "rising", 1, 1),
            (200, 26, "rising", 2, 2),
            (300, 13, "falling", 3, 2)]), "data", 2)
        self.assertEqual(report["unexpected_inactive_line_events"], 1)
        self.assertEqual(report["lines"]["26"]["expected_if_completed"], 0)
        self.assertEqual(report["lines"]["26"]["sequence"]["unresolved_at_end"], 1)

    def test_duplicate(self):
        report = analyze(capture([
            (100, 13, "rising", 1, 1),
            (200, 13, "falling", 1, 1)]), "data", 2)
        self.assertEqual(report["global_sequence"]["regressions_or_duplicates"], 1)

    def test_empty_and_malformed(self):
        report = analyze(capture([]), "data", 2)
        self.assertEqual(report["lines"]["13"]["observed"], 0)
        self.assertEqual(report["lines"]["13"]["difference"], -2)
        self.assertIsNone(report["global_sequence"]["continuous"])
        for stream in (io.StringIO("time_us,gpio,edge\n"), capture([
                (100, 6, "rising", 1, 1)]), capture([
                (100, 13, "invalid", 1, 1)])):
            with self.assertRaises(ValueError):
                analyze(stream, "data", 2)


if __name__ == "__main__":
    unittest.main()
