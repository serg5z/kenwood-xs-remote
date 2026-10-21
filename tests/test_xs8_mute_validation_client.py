"""Execute only the documented operator workflow with fake I/O; never sockets/GPIO."""
import contextlib
import io
import pathlib
import re
import types
import unittest


DOCUMENT = pathlib.Path(__file__).resolve().parents[1] / "docs/xs8-h11-mute-validation.md"
SCRIPT = re.search(r"<<'PY'.*?\n(.*?)\nPY\n", DOCUMENT.read_text(), re.S).group(1)
WORKFLOW = SCRIPT[SCRIPT.index("def confirm("):]


def state(initial=False, warnings=0, **changes):
    result = dict(state="IDLE", monitor="HEALTHY", stop_owed="0",
                  context_may_be_open="0", cd_state="IDLE", cd_release_owed="0",
                  cd_lease_expired="0", last_byte="NONE" if initial else "0x07",
                  last_gpio_tx="NONE" if initial else "TX_OK", last_waveform="MATCH",
                  receiver_execution="UNVERIFIED", global_gap_policy="warn",
                  global_gap_warnings=str(warnings),
                  capture_integrity="WARN_GLOBAL_GAP" if warnings else "NO_KNOWN_GAPS",
                  receiver_integrity="UNCERTAIN" if warnings else "HEALTHY")
    result.update(changes)
    return result


class MuteOperatorWorkflowTest(unittest.TestCase):
    def run_workflow(self, statuses=None, confirmations=None, reply="OK MUTE_TX"):
        statuses = iter(statuses or [state(True)] + [state(warnings=3)] * 5)
        confirmations = iter(confirmations or
                             ["ONE_MUTE_ON", "STAGE1_PASS", "ONE_MUTE_OFF", "STAGE2_PASS"])
        commands, sleeps = [], []
        clock = [100.0]

        def sleep(seconds):
            sleeps.append(seconds)
            clock[0] += seconds

        def request(command):
            commands.append((command, clock[0]))
            if isinstance(reply, Exception):
                raise reply
            return reply

        env = dict(input=lambda prompt: next(confirmations), status=lambda: next(statuses),
                   request=request, time=types.SimpleNamespace(monotonic=lambda: clock[0], sleep=sleep))
        error = None
        with contextlib.redirect_stdout(io.StringIO()):
            try:
                exec(compile(WORKFLOW, str(DOCUMENT), "exec"), env)
            except (SystemExit, TimeoutError, EOFError) as exc:
                error = exc
        return commands, sleeps, error

    def test_two_confirmed_stages_preserve_warnings_and_wait(self):
        commands, sleeps, error = self.run_workflow()
        self.assertIsNone(error)
        self.assertEqual([c for c, _ in commands], [b"MUTE\n", b"MUTE\n"])
        self.assertGreaterEqual(commands[1][1] - commands[0][1], 3)
        self.assertEqual(sleeps, [3])

    def test_off_requires_separate_confirmation(self):
        commands, _, error = self.run_workflow(
            confirmations=["ONE_MUTE_ON", "STAGE1_PASS", "CANCEL"])
        self.assertIsInstance(error, SystemExit)
        self.assertEqual(len(commands), 1)

    def test_uncertain_stage1_or_missing_physical_confirmation_blocks_off(self):
        for reply in ("ERR MUTE_UNCERTAIN", TimeoutError("uncertain")):
            commands, _, error = self.run_workflow(reply=reply)
            self.assertIsNotNone(error)
            self.assertEqual(len(commands), 1)
        commands, _, error = self.run_workflow(confirmations=["ONE_MUTE_ON", "NOT_CONFIRMED"])
        self.assertIsInstance(error, SystemExit)
        self.assertEqual(len(commands), 1)

    def test_fault_before_or_after_off_confirmation_blocks_off(self):
        for index in (1, 2, 3):
            statuses = [state(True)] + [state(warnings=3)] * 5
            statuses[index] = state(warnings=3, monitor="FAILED", state="FAULT_LATCHED")
            commands, _, error = self.run_workflow(statuses=statuses)
            self.assertIsInstance(error, SystemExit)
            self.assertEqual(len(commands), 1)

    def test_warning_reset_and_uncertain_integrity_block_off(self):
        for invalid in (state(), state(warnings=3, receiver_integrity="HEALTHY"),
                        state(warnings=3, context_may_be_open="1"),
                        state(warnings=3, last_waveform="INVALID")):
            commands, _, error = self.run_workflow(
                statuses=[state(True), state(warnings=3), invalid])
            self.assertIsInstance(error, SystemExit)
            self.assertEqual(len(commands), 1)

    def test_warning_regression_during_confirmation_blocks_off(self):
        commands, _, error = self.run_workflow(
            statuses=[state(True), state(warnings=3), state(warnings=5), state(warnings=3)])
        self.assertIsInstance(error, SystemExit)
        self.assertEqual(len(commands), 1)

    def test_dirty_baseline_or_no_confirmation_sends_nothing(self):
        commands, _, error = self.run_workflow(statuses=[state(True, warnings=1)])
        self.assertIsInstance(error, SystemExit)
        self.assertEqual(commands, [])
        commands, _, error = self.run_workflow(confirmations=["CANCEL"])
        self.assertIsInstance(error, SystemExit)
        self.assertEqual(commands, [])


if __name__ == "__main__":
    unittest.main()
