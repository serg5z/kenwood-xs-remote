import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("xs8_disc_select_sweep",
                                              ROOT / "xs8_disc_select_sweep.py")
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)

CANDIDATES = ROOT / "research" / "disc-select-candidates.json"
RANKED_26 = ("9A CA A8 BA AA FA 0A 1A 2A 3A 4A 5A B8 E8 F8 "
             "8C AC DC 0C 1C 2C 3C 5C 6C 7C 4C").split()


class MemoryLog:
    def __init__(self):
        self.records = []

    def write(self, record):
        self.records.append(json.loads(json.dumps(record)))

    def trials(self):
        return [r for r in self.records if r["event"] == "trial"]


def answers(*items):
    """Scripted operator; running out of answers is closed input (EOFError)."""
    queue = list(items)

    def ask(prompt):
        if not queue:
            raise EOFError
        return queue.pop(0)
    return ask


def full_script(count):
    replies = ["ready"]
    for done in range(1, count + 1):
        replies.append("t4")
        if done % 5 == 0 or done == count:
            replies.append("panel-ok")
        if done != count:
            replies.append("ready")
    return replies


def no_questions(prompt):
    raise AssertionError("unattended mode asked the operator: " + prompt)


def run_unattended(candidates=None, scenario=None, interrupt_on_sleep=False,
                   interrupt_after_chains=None):
    suffix, loaded, _ = sweep.load_candidates(CANDIDATES, "structural-26")
    plan = loaded if candidates is None else [
        c for c in loaded if f"{c['value']:02X}" in candidates]
    service = sweep.MockService(scenario)
    log = MemoryLog()
    sleeps = []
    runner = None

    def sleep(seconds):
        sleeps.append(seconds)
        if interrupt_on_sleep:
            runner.interrupted = True
    if interrupt_after_chains is not None:
        original = service.request

        def counting(command):
            reply = original(command)
            if (command.startswith("RESEARCH_CHAIN") and
                    len(chains(service)) == interrupt_after_chains):
                runner.interrupted = True
            return reply
        service.request = counting
    runner = sweep.Sweep(service, suffix, plan, gap_ms=200, window_ms=3000, delay_s=5.0,
                         log=log, output=lambda line: None, ask=no_questions, sleep=sleep,
                         attended=False)
    with patch.object(sweep.time, "monotonic", side_effect=fake_clock(sleeps)):
        verdict = runner.run()
    return verdict, service, log, sleeps


def fake_clock(sleeps):
    """monotonic() that advances only by the harness's own sleeps."""
    def now():
        return 1000.0 + sum(sleeps)
    return now


def run(candidates, scenario=None, replies=None, delay_s=0.0):
    suffix, loaded, _ = sweep.load_candidates(CANDIDATES, "structural-26")
    plan = [c for c in loaded if f"{c['value']:02X}" in candidates]
    service = sweep.MockService(scenario)
    log = MemoryLog()
    if replies is None:
        replies = full_script(len(plan))
    out = []
    runner = sweep.Sweep(service, suffix, plan, gap_ms=200, window_ms=3000,
                         delay_s=delay_s, log=log, output=out.append,
                         ask=answers(*replies), sleep=lambda s: None)
    return runner.run(), service, log, out


def chains(service):
    return [c for c in service.commands if c.startswith("RESEARCH_CHAIN")]


class CandidateSetTest(unittest.TestCase):
    def test_structural_set_keeps_documented_ranks_and_skips_tested_9a(self):
        suffix, candidates, skipped = sweep.load_candidates(CANDIDATES, "structural-26")
        self.assertEqual(suffix, 0x48)
        self.assertEqual([f"{c['value']:02X}" for c in skipped], ["9A"])
        self.assertEqual([f"{c['value']:02X}" for c in candidates], RANKED_26[1:])
        self.assertEqual([c["rank"] for c in candidates], list(range(2, 27)))

    def test_filtered_set_is_the_whole_union_with_structural_order_first(self):
        _, candidates, skipped = sweep.load_candidates(CANDIDATES, "filtered-169")
        values = [c["value"] for c in skipped + candidates]
        self.assertEqual(set(values), sweep.ALLOWED_PREFIXES)
        self.assertEqual(len(values), 169)
        self.assertEqual([f"{c['value']:02X}" for c in candidates[:25]], RANKED_26[1:])
        self.assertEqual([f"{c['value']:02X}" for c in candidates[-3:]], ["A4", "C4", "D4"])

    def test_bytes_outside_the_union_are_refused_whatever_the_file_says(self):
        for bad in ("0x17", "0x62", "0x8A", "0x48", "0xE9", "0x35", "0xC3"):
            data = json.loads(CANDIDATES.read_text())
            data["sets"]["structural-26"]["candidates"][1]["byte"] = bad
            with tempfile.NamedTemporaryFile("w", suffix=".json") as handle:
                json.dump(data, handle)
                handle.flush()
                with self.assertRaisesRegex(ValueError, "outside the allowed"):
                    sweep.load_candidates(handle.name, "structural-26")

    def test_suffix_other_than_track_4_is_refused(self):
        data = json.loads(CANDIDATES.read_text())
        data["suffix"] = "0x58"
        with tempfile.NamedTemporaryFile("w", suffix=".json") as handle:
            json.dump(data, handle)
            handle.flush()
            with self.assertRaisesRegex(ValueError, "suffix"):
                sweep.load_candidates(handle.name, "structural-26")

    def test_digest_changes_with_order_timing_and_set(self):
        suffix, candidates, _ = sweep.load_candidates(CANDIDATES, "structural-26")
        base = sweep.approval_digest("structural-26", suffix, 200, 3000, candidates)
        self.assertNotEqual(base, sweep.approval_digest("structural-26", suffix, 250, 3000, candidates))
        self.assertNotEqual(base, sweep.approval_digest("structural-26", suffix, 200, 2000, candidates))
        self.assertNotEqual(base, sweep.approval_digest("filtered-169", suffix, 200, 3000, candidates))
        swapped = [candidates[1], candidates[0]] + candidates[2:]
        self.assertNotEqual(base, sweep.approval_digest("structural-26", suffix, 200, 3000, swapped))

    def test_9a_is_recorded_as_a_physical_negative(self):
        tested = json.loads(CANDIDATES.read_text())["tested"]
        self.assertEqual(len(tested), 1)
        entry = tested[0]
        self.assertEqual((entry["byte"], entry["suffix"], entry["chain_id"], entry["gap_ms"]),
                         ("0x9A", "0x48", 9, 214.1))
        self.assertEqual((entry["start"], entry["end"]), ("Disc 1 / Track 1", "Disc 1 / Track 4"))
        self.assertTrue(entry["physical"].startswith("NEGATIVE"))

    def test_approved_plan_and_digest_are_unchanged(self):
        suffix, candidates, _ = sweep.load_candidates(CANDIDATES, "structural-26")
        self.assertEqual(sweep.approval_digest("structural-26", suffix, 200, 3000, candidates),
                         "ebe5699f2ec3b291")

    def test_legacy_labels_match_the_archive(self):
        self.assertEqual(sweep.legacy(0x8A), 0xAE)
        self.assertEqual(sweep.legacy(0x48), 0xED)
        self.assertEqual(sweep.legacy(0x9A), 0xA6)
        self.assertEqual(sweep.legacy(0xD4), 0xD4)


class TrialTest(unittest.TestCase):
    def test_silent_trials_run_in_rank_order_once_each(self):
        verdict, service, log, _ = run(["CA", "A8", "BA"])
        self.assertEqual(verdict, "SET_COMPLETE")
        self.assertEqual(chains(service), [
            "RESEARCH_CHAIN 2 200 3000 0xCA 0x48",
            "RESEARCH_CHAIN 2 200 3000 0xA8 0x48",
            "RESEARCH_CHAIN 2 200 3000 0xBA 0x48"])
        trials = log.trials()
        self.assertEqual([t["verdict"] for t in trials], ["RX_SILENT"] * 3)
        timing = trials[0]["timing"]
        self.assertEqual((timing["tx_ms"], timing["gap_ms"], timing["tx_start_ms"]),
                         ([100.0, 100.0], 214.0, [0.0, 314.0]))
        self.assertEqual([(r["waveform"], r["legacy"], r["relation"]) for r in timing["rx"]],
                         [("0xCA", "0xAC", "TX_WINDOW"), ("0x48", "0xED", "TX_WINDOW")])
        self.assertIn("state=IDLE", trials[0]["pre_status"])
        self.assertIn("state=IDLE", trials[0]["post_status"])
        self.assertEqual(len(trials[0]["rx"]), 2)

    def test_only_chains_and_reads_are_sent(self):
        _, service, _, _ = run(["CA"])
        verbs = {c.split()[0] for c in service.commands}
        self.assertEqual(verbs, {"STATUS", "RESEARCH_CHAIN", "RESEARCH_TX_READ", "RESEARCH_READ"})

    def test_d4_stops_as_possible_disc_change(self):
        verdict, service, log, out = run(["CA", "A8"], {0xCA: {"rx": [(1150, 0xD4)]}})
        self.assertEqual(verdict, "POSSIBLE_DISC_CHANGE")
        self.assertEqual(len(chains(service)), 1)
        self.assertIn("not confirmed success", log.trials()[0]["detail"])
        self.assertTrue(any("STOP POSSIBLE_DISC_CHANGE" in line for line in out))

    def test_any_other_rx_stops(self):
        for label in (0xD6, 0xDE, 0x7D, 0x1D):
            verdict, service, _, _ = run(["CA", "A8"], {0xCA: {"rx": [(400, label)]}})
            self.assertEqual(verdict, "UNEXPECTED_RX", hex(label))
            self.assertEqual(len(chains(service)), 1)

    def test_missing_echo_stops(self):
        verdict, _, _, _ = run(["CA", "A8"], {0xCA: {"echo_missing": True}})
        self.assertEqual(verdict, "UNEXPECTED_RX")

    def test_tx_failure_stops_without_retry(self):
        for index in (0, 1):
            verdict, service, _, _ = run(["CA", "A8"], {0xCA: {"tx_fail": index}})
            self.assertEqual(verdict, "TX_FAILURE")
            self.assertEqual(len(chains(service)), 1)

    def test_aborted_tx_done_stops_even_with_clean_frames(self):
        verdict, service, _, _ = run(["CA", "A8"], {0xCA: {"done_aborted": True}})
        self.assertEqual(verdict, "TX_FAILURE")
        self.assertEqual(len(chains(service)), 1)

    def test_monitor_fault_stops(self):
        verdict, _, _, _ = run(["CA", "A8"], {0xCA: {"monitor_failed": True}})
        self.assertEqual(verdict, "MONITOR_FAULT")

    def test_abnormal_pre_status_stops_before_sending(self):
        bad = sweep.MockService.IDLE.replace("cd_release_owed=0", "cd_release_owed=1")
        verdict, service, log, _ = run(["CA"], {0xCA: {"pre_status": bad}})
        self.assertEqual(verdict, "ANOMALOUS_STATUS")
        self.assertEqual(chains(service), [])
        self.assertFalse(log.trials()[0]["chain_sent"])

    def test_abnormal_post_status_stops(self):
        for change in (("monitor=HEALTHY", "monitor=FAILED"),
                       ("receiver_source=CD", "receiver_source=TUNER"),
                       ("state=IDLE", "state=CD_HOLD")):
            bad = sweep.MockService.IDLE.replace(*change)
            verdict, service, _, _ = run(["CA", "A8"], {0xCA: {"post_status": bad}})
            self.assertEqual(verdict, "ANOMALOUS_STATUS", change)
            self.assertEqual(len(chains(service)), 1)

    def test_unreachable_service_stops_and_counts_as_sent(self):
        suffix, loaded, _ = sweep.load_candidates(CANDIDATES, "structural-26")
        service = sweep.MockService()
        original = service.request

        def flaky(command):
            if command.startswith("RESEARCH_CHAIN"):
                raise ConnectionError("socket closed")
            return original(command)
        service.request = flaky
        log = MemoryLog()
        runner = sweep.Sweep(service, suffix, loaded[:2], gap_ms=200, window_ms=3000,
                             delay_s=0, log=log, output=lambda s: None,
                             ask=answers("ready"), sleep=lambda s: None)
        self.assertEqual(runner.run(), "SERVICE_UNREACHABLE")
        self.assertTrue(log.trials()[0]["chain_sent"])


class CheckpointTest(unittest.TestCase):
    def test_initial_abort_sends_nothing(self):
        verdict, service, _, _ = run(["CA"], replies=["abort"])
        self.assertEqual(verdict, "OPERATOR_ABORT")
        self.assertEqual(service.commands, [])

    def test_every_observation_other_than_track_4_stops(self):
        for word in ("disc", "none", "other", "abort"):
            verdict, service, _, _ = run(["CA", "A8"], replies=["ready", word + " note"])
            expected = "OPERATOR_ABORT" if word == "abort" else "OPERATOR_OBSERVED_" + word.upper()
            self.assertEqual(verdict, expected)
            self.assertEqual(len(chains(service)), 1)

    def test_reset_checkpoint_abort_stops(self):
        verdict, service, _, _ = run(["CA", "A8"], replies=["ready", "t4", "abort"])
        self.assertEqual(verdict, "OPERATOR_ABORT")
        self.assertEqual(len(chains(service)), 1)

    def test_invalid_answers_are_asked_again(self):
        verdict, _, log, _ = run(["CA"], replies=["", "go", "ready d1 t2", "yes", "t4",
                                                  "ok", "panel-ok"])
        self.assertEqual(verdict, "SET_COMPLETE")
        observation = [r for r in log.records if r.get("phase") == "observation"]
        self.assertEqual(observation[0]["answer"], "t4")

    def test_panel_check_after_every_five_completed_trials_and_at_the_end(self):
        plan = RANKED_26[1:8]  # Seven trials: panel after 5 and after 7.
        verdict, service, log, _ = run(plan)
        self.assertEqual(verdict, "SET_COMPLETE")
        self.assertEqual(len(chains(service)), 7)
        panels = [r["completed"] for r in log.records if r.get("phase") == "panel"]
        self.assertEqual(panels, [5, 7])
        phases = [r.get("phase") for r in log.records if r["event"] == "checkpoint"]
        fifth = [i for i, p in enumerate(phases) if p == "observation"][4]
        self.assertEqual(phases[fifth + 1:fifth + 3], ["panel", "reset"])

    def test_panel_check_only_at_the_end_of_a_short_run(self):
        verdict, _, log, _ = run(["CA", "A8"])
        self.assertEqual(verdict, "SET_COMPLETE")
        self.assertEqual([r["completed"] for r in log.records if r.get("phase") == "panel"], [2])

    def test_failed_or_aborted_panel_check_stops(self):
        script = full_script(7)
        at = script.index("panel-ok")
        for word, verdict in (("fail no display", "PANEL_CHECK_FAILED"),
                              ("abort", "OPERATOR_ABORT")):
            replies = script[:at] + [word] + script[at + 1:]
            result, service, log, _ = run(RANKED_26[1:8], replies=replies)
            self.assertEqual(result, verdict)
            self.assertEqual(len(chains(service)), 5)
            self.assertEqual(log.records[-1]["event"], "stop")

    def test_final_panel_check_is_required_for_completion(self):
        for word, verdict in (("fail", "PANEL_CHECK_FAILED"), (None, "CHECKPOINT_SKIPPED")):
            replies = ["ready", "t4"] + ([word] if word else [])
            result, service, _, _ = run(["CA"], replies=replies)
            self.assertEqual(result, verdict)
            self.assertEqual(len(chains(service)), 1)

    def test_skipping_any_checkpoint_stops_without_sending_more(self):
        script = full_script(7)
        # Cut the script before each kind of checkpoint: initial, observation, reset, panel.
        cuts = {"initial": 0, "observation": 1, "reset": 2,
                "panel": script.index("panel-ok")}
        for phase, cut in cuts.items():
            verdict, service, log, _ = run(RANKED_26[1:8], replies=script[:cut])
            self.assertEqual(verdict, "CHECKPOINT_SKIPPED", phase)
            sent = len(chains(service))
            self.assertEqual(sent, {"initial": 0, "observation": 1, "reset": 1,
                                    "panel": 5}[phase], phase)
            self.assertIn("input closed", log.records[-1]["detail"])

    def test_interrupt_during_delay_stops_before_next_trial(self):
        suffix, loaded, _ = sweep.load_candidates(CANDIDATES, "structural-26")
        service = sweep.MockService()
        runner = None

        sleeps = []

        def sleep(seconds):
            sleeps.append(seconds)
            runner.interrupted = True
        runner = sweep.Sweep(service, suffix, loaded[:3], gap_ms=200, window_ms=3000,
                             delay_s=30, log=MemoryLog(), output=lambda s: None,
                             ask=answers("ready", "t4", "ready"), sleep=sleep)
        self.assertEqual(runner.run(), "OPERATOR_ABORT")
        self.assertEqual(len(chains(service)), 1)
        self.assertEqual(len(sleeps), 1)  # Abandoned at the first 0.1 s slice.
        self.assertLessEqual(sleeps[0], 0.1)


class UnattendedTest(unittest.TestCase):
    def test_all_25_pairs_run_in_rank_order_without_questions(self):
        verdict, service, log, sleeps = run_unattended()
        self.assertEqual(verdict, "SET_COMPLETE_RX_SILENT")
        self.assertEqual(chains(service),
                         [f"RESEARCH_CHAIN 2 200 3000 0x{b} 0x48" for b in RANKED_26[1:]])
        self.assertEqual(len(log.trials()), 25)
        self.assertEqual({t["verdict"] for t in log.trials()}, {"RX_SILENT"})
        self.assertFalse([r for r in log.records if r["event"] == "checkpoint"])
        self.assertAlmostEqual(sum(sleeps), 5.0 * 24)  # Between trials, not after the last.
        self.assertNotIn("NEGATIVE", json.dumps(log.records))
        stop = log.records[-1]
        self.assertIn("does NOT rule out DISC SELECT", stop["detail"])
        self.assertIn("no candidate is a verified negative", stop["detail"])

    def test_each_trial_logs_full_tx_and_rx_timing(self):
        verdict, _, log, _ = run_unattended(["CA", "A8"], {0xA8: {"rx": [(1150, 0xD4)]}})
        self.assertEqual(verdict, "POSSIBLE_DISC_CHANGE")
        silent, flagged = log.trials()
        self.assertEqual(len(silent["tx"]), 2)
        self.assertEqual(len(silent["rx"]), 2)
        self.assertIn("RESEARCH_DONE", silent["done"])
        self.assertEqual(flagged["timing"]["rx"][-1],
                         {"start_ms": 1564.0, "end_ms": 1664.0, "waveform": "0xD4",
                          "legacy": "0xD4", "relation": "OTHER_TRAFFIC"})

    def test_every_stop_condition_ends_the_sweep_at_that_trial(self):
        bad = sweep.MockService.IDLE
        cases = {
            "POSSIBLE_DISC_CHANGE": {"rx": [(1150, 0xD4)]},
            "UNEXPECTED_RX": {"rx": [(300, 0xDE)]},
            "TX_FAILURE": {"tx_fail": 1},
            "MONITOR_FAULT": {"monitor_failed": True},
            "ANOMALOUS_STATUS": {"post_status": bad.replace("receiver_integrity=HEALTHY",
                                                            "receiver_integrity=UNCERTAIN")},
        }
        for verdict, scenario in cases.items():
            result, service, log, _ = run_unattended(None, {0xBA: scenario})
            self.assertEqual(result, verdict)
            self.assertEqual(chains(service)[-1], "RESEARCH_CHAIN 2 200 3000 0xBA 0x48")
            self.assertEqual(len(chains(service)), 3, verdict)
            self.assertEqual(log.records[-1]["verdict"], verdict)
        for name, scenario in (("echo", {"echo_missing": True}),
                               ("aborted", {"done_aborted": True}),
                               ("tx0", {"tx_fail": 0})):
            result, service, _, _ = run_unattended(None, {0xBA: scenario})
            self.assertIn(result, ("UNEXPECTED_RX", "TX_FAILURE"), name)
            self.assertEqual(len(chains(service)), 3, name)

    def test_abnormal_status_before_a_trial_sends_nothing_more(self):
        bad = sweep.MockService.IDLE.replace("state=IDLE", "state=VOLUME_HOLD")
        result, service, log, _ = run_unattended(None, {0xBA: {"pre_status": bad}})
        self.assertEqual(result, "ANOMALOUS_STATUS")
        self.assertEqual(len(chains(service)), 2)
        self.assertFalse(log.trials()[-1]["chain_sent"])

    def test_interrupt_stops_during_delay_or_after_the_current_pair(self):
        result, service, _, sleeps = run_unattended(interrupt_on_sleep=True)
        self.assertEqual(result, "OPERATOR_ABORT")
        self.assertEqual(len(chains(service)), 1)
        self.assertEqual(len(sleeps), 1)
        result, service, log, _ = run_unattended(interrupt_after_chains=3)
        self.assertEqual(result, "OPERATOR_ABORT")
        self.assertEqual(len(chains(service)), 3)
        self.assertEqual(log.trials()[-1]["verdict"], "RX_SILENT")  # The pair completed.

    def test_interrupt_before_start_sends_nothing(self):
        suffix, loaded, _ = sweep.load_candidates(CANDIDATES, "structural-26")
        service = sweep.MockService()
        runner = sweep.Sweep(service, suffix, loaded, gap_ms=200, window_ms=3000,
                             delay_s=5.0, log=MemoryLog(), output=lambda line: None,
                             ask=no_questions, sleep=lambda s: None, attended=False)
        runner.interrupted = True
        self.assertEqual(runner.run(), "OPERATOR_ABORT")
        self.assertEqual(chains(service), [])

    def test_interrupt_during_the_last_pair_is_not_reported_as_complete(self):
        result, service, log, _ = run_unattended(interrupt_after_chains=25)
        self.assertEqual(result, "OPERATOR_ABORT")
        self.assertEqual(len(chains(service)), 25)
        self.assertEqual(log.records[-1]["detail"], "interrupt during trial")

    def test_unattended_digest_covers_mode_and_delay(self):
        suffix, candidates, _ = sweep.load_candidates(CANDIDATES, "structural-26")
        attended = sweep.approval_digest("structural-26", suffix, 200, 3000, candidates)
        unattended = sweep.approval_digest("structural-26", suffix, 200, 3000, candidates,
                                           mode=sweep.UNATTENDED, delay_s=5.0)
        self.assertEqual(attended, "ebe5699f2ec3b291")
        self.assertNotEqual(unattended, attended)
        self.assertNotEqual(unattended, sweep.approval_digest(
            "structural-26", suffix, 200, 3000, candidates, mode=sweep.UNATTENDED, delay_s=4.0))
        self.assertNotEqual(unattended, sweep.approval_digest(
            "structural-26", suffix, 250, 3000, candidates, mode=sweep.UNATTENDED, delay_s=5.0))


class CommandLineTest(unittest.TestCase):
    def main(self, *args, stdin=""):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), patch("builtins.input", side_effect=stdin.split("\n")):
            code = sweep.main(list(args))
        return code, out.getvalue()

    def test_list_prints_plan_and_digest(self):
        code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26", "--list")
        self.assertEqual(code, 0)
        self.assertIn("skip 0x9A (rank 1): already tested", text)
        self.assertIn("plan: 2:0xCA 3:0xA8 4:0xBA", text)
        self.assertIn("digest ", text)

    def test_real_run_refused_without_matching_digest(self):
        with patch.object(sweep, "SocketService") as real:
            code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                   "--log", "/dev/null", "--approve", "0000")
            self.assertEqual(code, 2)
            self.assertIn("Refused", text)
            real.assert_not_called()

    def test_real_run_refused_without_log(self):
        _, listing = self.main("--candidates", str(CANDIDATES), "--set", "structural-26", "--list")
        digest = listing.split("digest ")[1].split()[0]
        with patch.object(sweep, "SocketService") as real:
            code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                   "--approve", digest, stdin=sweep.START_PHRASE)
            self.assertEqual(code, 2)
            self.assertIn("need --log", text)
            real.assert_not_called()

    def test_real_run_refused_without_start_phrase(self):
        _, listing = self.main("--candidates", str(CANDIDATES), "--set", "structural-26", "--list")
        digest = listing.split("digest ")[1].split()[0]
        with patch.object(sweep, "SocketService") as real:
            code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                   "--log", "/dev/null", "--approve", digest, stdin="yes")
            self.assertEqual(code, 2)
            real.assert_not_called()

    def test_auto_observe_is_dry_run_only(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                      "--auto-observe", "--log", "/dev/null", "--approve", "x")

    def test_dry_run_never_opens_the_socket_and_resumes_without_resending(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.jsonl"
            with patch.object(sweep.xs8_repl, "request") as real:
                code, _ = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                    "--dry-run", "--auto-observe", "--max-trials", "3",
                                    "--delay-s", "0", "--log", str(log))
                real.assert_not_called()
            self.assertEqual(code, 0)
            code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                   "--log", str(log), "--list", "--dry-run")
            self.assertIn("skip 0xCA (rank 2): sent in", text)
            self.assertIn("plan: 5:0xAA", text)
            records = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual([r["prefix"] for r in records if r["event"] == "trial"],
                             ["0xCA", "0xA8", "0xBA"])
            self.assertTrue(records[0]["dry_run"])

    def test_dry_run_log_is_refused_for_a_real_run(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.jsonl"
            self.main("--candidates", str(CANDIDATES), "--set", "structural-26", "--dry-run",
                      "--auto-observe", "--max-trials", "1", "--delay-s", "0", "--log", str(log))
            with patch.object(sweep, "SocketService") as real, \
                    self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                          "--log", str(log), "--approve", "x")
            real.assert_not_called()

    def unattended_digest(self):
        _, listing = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                               "--unattended-rx-only", "--list")
        self.assertIn("mode unattended-rx-only, delay 5 s", listing)
        return listing.split("digest ")[1].split()[0]

    def test_attended_digest_cannot_start_an_unattended_run(self):
        with patch.object(sweep, "SocketService") as real:
            code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                   "--unattended-rx-only", "--log", "/dev/null",
                                   "--approve", "ebe5699f2ec3b291",
                                   stdin=sweep.START_PHRASE)
            self.assertEqual(code, 2)
            self.assertIn("Refused", text)
            real.assert_not_called()

    def test_unattended_digest_cannot_start_an_attended_run(self):
        digest = self.unattended_digest()
        with patch.object(sweep, "SocketService") as real:
            code, _ = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                "--log", "/dev/null", "--approve", digest,
                                stdin=sweep.START_PHRASE)
            self.assertEqual(code, 2)
            real.assert_not_called()

    def test_unattended_real_run_still_needs_the_start_phrase(self):
        digest = self.unattended_digest()
        with patch.object(sweep, "SocketService") as real:
            code, _ = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                "--unattended-rx-only", "--log", "/dev/null",
                                "--approve", digest, stdin="go")
            self.assertEqual(code, 2)
            real.assert_not_called()

    def test_unattended_dry_run_end_to_end(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.jsonl"
            with patch.object(sweep.xs8_repl, "request") as real, \
                    patch.object(sweep.time, "sleep"):
                code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                                       "--unattended-rx-only", "--dry-run", "--delay-s", "0",
                                       "--log", str(log))
                real.assert_not_called()
            self.assertEqual(code, 0)
            self.assertIn("STOP SET_COMPLETE_RX_SILENT", text)
            records = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual(records[0]["mode"], "unattended-rx-only")
            self.assertEqual(len([r for r in records if r["event"] == "trial"]), 25)
            # An attended run may not reuse (and so skip) unattended trials.
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                          "--dry-run", "--log", str(log), "--list")

    def test_auto_observe_is_refused_with_unattended(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            self.main("--candidates", str(CANDIDATES), "--set", "structural-26", "--dry-run",
                      "--unattended-rx-only", "--auto-observe")

    def test_one_set_per_run(self):
        code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                               "--dry-run", "--auto-observe", "--delay-s", "0")
        self.assertEqual(code, 0)
        self.assertIn("STOP SET_COMPLETE", text)
        self.assertNotIn("0x00 ->", text)  # First byte unique to filtered-169.


if __name__ == "__main__":
    unittest.main()
