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

REAL_CANDIDATES = ROOT / "research" / "disc-select-candidates.json"
# Fixture: the real sets with only chain 9 (0x9A) marked tested, so the harness
# tests keep a full structural-26 plan however far the real research has got.
_fixture = json.loads(REAL_CANDIDATES.read_text())
_fixture["tested"] = [t for t in _fixture["tested"] if t["byte"] == "0x9A"]
_fixture_dir = tempfile.TemporaryDirectory()
CANDIDATES = Path(_fixture_dir.name) / "candidates.json"
CANDIDATES.write_text(json.dumps(_fixture))
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
                   interrupt_after_chains=None, observe_delay=False, delay_s=5.0,
                   interrupt_after_observes=None):
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
    if interrupt_after_observes is not None:
        original_observe = service.request

        def counting_observe(command):
            reply = original_observe(command)
            if (command.startswith("RESEARCH_OBSERVE") and
                    len(service.observed) == interrupt_after_observes):
                runner.interrupted = True
            return reply
        service.request = counting_observe
    runner = sweep.Sweep(service, suffix, plan, gap_ms=200, window_ms=3000, delay_s=delay_s,
                         log=log, output=lambda line: None, ask=no_questions, sleep=sleep,
                         attended=False, observe_delay=observe_delay)
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
        tested = json.loads(REAL_CANDIDATES.read_text())["tested"]
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


class ExtendedPlanTest(unittest.TestCase):
    """The real candidate file after the structural sweep."""

    def test_filtered_169_leaves_142_unattended_candidates_in_documented_order(self):
        data = json.loads(REAL_CANDIDATES.read_text())
        tested = [t["byte"] for t in data["tested"]]
        self.assertEqual(len(tested), 26)
        self.assertEqual(len(set(tested)), 26)
        self.assertEqual(sorted(int(b, 16) for b in tested),
                         sorted(int(b, 16) for b in RANKED_26))
        suffix, candidates, skipped = sweep.load_candidates(REAL_CANDIDATES, "filtered-169",
                                                            unattended=True)
        self.assertEqual(suffix, 0x48)
        self.assertEqual(len(candidates), 142)
        self.assertEqual(len({c["value"] for c in candidates}), 142)
        self.assertEqual({f"0x{c['value']:02X}": c["skip_reason"] for c in skipped},
                         dict({b: "already tested" for b in tested},
                              **{"0xD4": "reserved for an attended experiment"}))
        self.assertFalse({f"0x{c['value']:02X}" for c in candidates} & (set(tested) | {"0xD4"}))
        documented = [e["byte"] for e in data["sets"]["filtered-169"]["candidates"]
                      if e["byte"] not in tested and e["byte"] != "0xD4"]
        self.assertEqual([f"0x{c['value']:02X}" for c in candidates], documented)
        ranks = [c["rank"] for c in candidates]
        self.assertEqual(ranks, sorted(ranks))
        self.assertEqual((candidates[0]["rank"], f"{candidates[0]['value']:02X}"), (27, "00"))
        self.assertEqual([f"{c['value']:02X}" for c in candidates[-2:]], ["A4", "C4"])
        self.assertEqual(candidates[-1]["rank"], 168)
        self.assertTrue({c["value"] for c in candidates} <= sweep.ALLOWED_PREFIXES)
        _, structural, _ = sweep.load_candidates(REAL_CANDIDATES, "structural-26")
        self.assertEqual(structural, [])

    def test_d4_is_reserved_for_attended_runs_only(self):
        reserved = json.loads(REAL_CANDIDATES.read_text())["reserved_attended"]
        self.assertEqual([r["byte"] for r in reserved], ["0xD4"])
        _, attended, _ = sweep.load_candidates(REAL_CANDIDATES, "filtered-169")
        self.assertEqual(len(attended), 143)
        self.assertEqual(f"{attended[-1]['value']:02X}", "D4")
        data = json.loads(CANDIDATES.read_text())
        data["reserved_attended"] = [{"byte": "0xA8"}]
        with tempfile.NamedTemporaryFile("w", suffix=".json") as handle:
            json.dump(data, handle)
            handle.flush()
            _, plan, skipped = sweep.load_candidates(handle.name, "structural-26", unattended=True)
            self.assertNotIn(0xA8, [c["value"] for c in plan])
            self.assertIn(("0xA8", "reserved for an attended experiment"),
                          [(f"0x{c['value']:02X}", c["skip_reason"]) for c in skipped])
            _, plan, _ = sweep.load_candidates(handle.name, "structural-26")
            self.assertIn(0xA8, [c["value"] for c in plan])

    def test_unattended_trials_are_never_recorded_as_negatives(self):
        tested = json.loads(REAL_CANDIDATES.read_text())["tested"]
        for entry in tested[1:]:
            self.assertTrue(entry["rx"].startswith("RX_SILENT"))
            self.assertIn("Not a verified negative", entry["physical"])
            self.assertFalse(entry["physical"].startswith("NEGATIVE"))
        self.assertEqual([t["chain_id"] for t in tested[1:]], list(range(10, 35)))

    def test_extended_digest_covers_list_order_timing_and_observation(self):
        suffix, candidates, _ = sweep.load_candidates(REAL_CANDIDATES, "filtered-169",
                                                      unattended=True)

        def digest(items=candidates, delay=8.0, observe=True, gap=200, window=3000):
            return sweep.approval_digest("filtered-169", suffix, gap, window, items,
                                         mode=sweep.UNATTENDED, delay_s=delay,
                                         observe_delay=observe)
        self.assertEqual(digest(), "fd87fc32fb88cf68")
        self.assertEqual(len({digest(), digest(delay=5.0), digest(observe=False),
                              digest(gap=250), digest(window=2000), digest(candidates[1:]),
                              digest([candidates[1], candidates[0]] + candidates[2:])}), 7)


class ObserveDelayTest(unittest.TestCase):
    def test_each_delay_and_the_tail_are_covered_by_passive_windows(self):
        verdict, service, log, sleeps = run_unattended(["CA", "A8", "BA"], observe_delay=True,
                                                       delay_s=8.0)
        self.assertEqual(verdict, "SET_COMPLETE_RX_SILENT")
        self.assertEqual(service.observed, [5000, 3000] * 3)
        observes = [r for r in log.records if r["event"] == "observe"]
        self.assertEqual([o["final"] for o in observes], [False] * 4 + [True] * 2)
        self.assertEqual({o["verdict"] for o in observes}, {"QUIET"})
        self.assertTrue(all("uncovered_ms" in o for o in observes))
        self.assertTrue(all("uncovered_before_ms" in t for t in log.trials()[1:]))
        self.assertNotIn(5.0, sleeps)  # The delay is observation, not sleep.
        self.assertIn("quiet passive observation", log.records[-1]["detail"])
        # Order: chain, two windows, chain, ...
        verbs = [c.split()[0] for c in service.commands
                 if c.startswith(("RESEARCH_CHAIN", "RESEARCH_OBSERVE"))]
        self.assertEqual(verbs, ["RESEARCH_CHAIN", "RESEARCH_OBSERVE", "RESEARCH_OBSERVE"] * 3)

    def test_frame_between_trials_stops_before_the_next_pair(self):
        for label, verdict in ((0xDE, "UNEXPECTED_RX"), (0x7D, "UNEXPECTED_RX"),
                               (0xD4, "POSSIBLE_DISC_CHANGE")):
            result, service, log, _ = run_unattended(
                ["CA", "A8", "BA"], {0xA8: {"between_rx": [(2700, label)]}},
                observe_delay=True, delay_s=8.0)
            self.assertEqual(result, verdict, hex(label))
            self.assertEqual(len(chains(service)), 2)
            self.assertIn("after 0xA8", log.records[-1]["detail"])
            self.assertIn("2700.0 ms into a passive window", log.records[-1]["detail"])

    def test_frame_after_the_last_trial_prevents_completion(self):
        result, service, _, _ = run_unattended(["CA", "A8"], {0xA8: {"between_rx": [(100, 0xDE)]}},
                                               observe_delay=True, delay_s=8.0)
        self.assertEqual(result, "UNEXPECTED_RX")
        self.assertEqual(len(chains(service)), 2)

    def test_observation_fault_or_refusal_stops(self):
        result, service, _, _ = run_unattended(["CA", "A8"], {0xCA: {"observe_monitor_failed": True}},
                                               observe_delay=True, delay_s=8.0)
        self.assertEqual(result, "MONITOR_FAULT")
        self.assertEqual(len(chains(service)), 1)
        suffix, loaded, _ = sweep.load_candidates(CANDIDATES, "structural-26")
        service = sweep.MockService()
        original = service.request
        service.request = lambda c: ("ERR RESEARCH reason=SESSION_BUSY"
                                     if c.startswith("RESEARCH_OBSERVE") else original(c))
        runner = sweep.Sweep(service, suffix, loaded[:2], gap_ms=200, window_ms=3000,
                             delay_s=8.0, log=MemoryLog(), output=lambda line: None,
                             ask=no_questions, sleep=lambda s: None, attended=False,
                             observe_delay=True)
        self.assertEqual(runner.run(), "OBSERVE_REFUSED")
        self.assertEqual(len(chains(service)), 1)

    def test_interrupt_during_observation_stops_before_the_next_pair(self):
        result, service, _, _ = run_unattended(["CA", "A8", "BA"], observe_delay=True,
                                               delay_s=8.0, interrupt_after_observes=1)
        self.assertEqual(result, "OPERATOR_ABORT")
        self.assertEqual(len(chains(service)), 1)
        self.assertEqual(service.observed, [5000])


class CorrelationTest(unittest.TestCase):
    def logged_run(self, scenario=None):
        _, _, log, _ = run_unattended(["CA", "A8"], scenario, observe_delay=True, delay_s=8.0)
        return log.records

    def journal(self, records, extra=()):
        lines = []
        for record in records:
            for line in record.get("tx", []):
                f = sweep.xs8_repl.fields(line)
                start = int(f["tx_start_ns"]) // 1000
                byte = f["requested"]
                lines.append(f"Oct 16 x xs8-service[1]: mono_us={start} XS8 TX {byte} timing "
                             "result=TX_OK")
                lines.append(f"Oct 16 x xs8-service[1]: mono_us={start + 31} XS8 OBSERVED_BUS "
                             f"source=UNATTRIBUTED status=VALID waveform_lsb={byte} "
                             f"legacy_msb_short1=0x{sweep.legacy(int(byte, 16)):02X}")
        return lines + list(extra)

    def test_echo_only_journal_is_clean(self):
        records = self.logged_run()
        result = sweep.correlate(records, self.journal(records))
        self.assertEqual((result["frames"], result["echoes"], result["non_echo"]), (4, 4, []))
        self.assertEqual((result["journal_tx_missing"], result["unlogged_tx"]), ([], []))

    def test_late_frames_and_unlogged_tx_are_reported(self):
        records = self.logged_run()
        last_tx = max(int(sweep.xs8_repl.fields(line)["tx_start_ns"]) // 1000
                      for record in records for line in record.get("tx", []))
        extra = [f"x: mono_us={last_tx + 6_103_300} XS8 OBSERVED_BUS status=VALID "
                 "waveform_lsb=0x84 legacy_msb_short1=0xDE",
                 f"x: mono_us={last_tx + 4_000_000} XS8 TX 0xF9 timing result=TX_OK",
                 f"x: mono_us={last_tx + 900_000_000} XS8 OBSERVED_BUS status=VALID "
                 "waveform_lsb=0x41 legacy_msb_short1=0x7D"]  # Outside the span: ignored.
        result = sweep.correlate(records, self.journal(records, extra))
        self.assertEqual(len(result["non_echo"]), 1)
        late = result["non_echo"][0]
        self.assertEqual((late["legacy"], late["after_prefix"], late["ms_after_last_tx"]),
                         ("0xDE", "0xA8", 6103.3))
        self.assertEqual(result["unlogged_tx"], [f"{last_tx + 4_000_000}:0xF9"])

    def test_missing_journal_tx_is_reported(self):
        records = self.logged_run()
        lines = [l for l in self.journal(records) if " XS8 TX 0x48" not in l]
        result = sweep.correlate(records, lines)
        self.assertEqual(len(result["journal_tx_missing"]), 2)


def grouped_script(group_count, after="ok d2 t4 playing"):
    script = []
    for number in range(1, group_count + 1):
        script += [f"start-group {number}", after]
    return script


def run_grouped(replies=None, scenario=None, size=4, count=10, done=(), interrupt_after=None):
    suffix, loaded, _ = sweep.load_candidates(CANDIDATES, "structural-26", unattended=True)
    groups = sweep.make_groups(loaded[:count], size)
    for group in groups:
        group["plan"] = [c for c in group["members"] if f"{c['value']:02X}" not in done]
    service = sweep.MockService(scenario)
    log = MemoryLog()
    out = []
    if replies is None:
        replies = grouped_script(len([g for g in groups if g["plan"]]))
    scripted = answers(*replies)
    runner = None

    def operator(prompt):
        service.clock_ns += 20_000_000_000  # Each answer takes the operator time.
        return scripted(prompt)
    if interrupt_after is not None:
        original = service.request

        def request(command):
            reply = original(command)
            if (command.startswith("RESEARCH_CHAIN") and
                    len(chains(service)) == interrupt_after):
                runner.interrupted = True  # The operator's visual abort (SIGTERM).
            return reply
        service.request = request
    runner = sweep.Sweep(service, suffix, [c for g in groups for c in g["plan"]], gap_ms=200,
                         window_ms=3000, delay_s=8.0, log=log, output=out.append,
                         ask=operator, sleep=lambda s: None, attended=True,
                         observe_delay=True, groups=groups)
    return runner.run(), service, log, out


def transmissions(service):
    """Every request that could put a frame on the bus."""
    return [c for c in service.commands
            if c.split()[0] in ("RESEARCH_CHAIN", "RESEARCH_SEND", "QUERY_STATUS")
            or c.startswith(("CD_", "VOLUME", "MUTE", "SELECT_SOURCE", "CONTEXT"))]


GROUP_PREFIXES = [RANKED_26[1:5], RANKED_26[5:9], RANKED_26[9:11]]


class GroupedTest(unittest.TestCase):
    def test_real_plan_splits_into_eleven_twelves_and_a_ten(self):
        _, candidates, _ = sweep.load_candidates(REAL_CANDIDATES, "filtered-169", unattended=True)
        groups = sweep.make_groups(candidates, 12)
        self.assertEqual([len(g["members"]) for g in groups], [12] * 11 + [10])
        self.assertEqual([(g["first_rank"], g["last_rank"]) for g in groups],
                         [(27 + 12 * i, 38 + 12 * i) for i in range(11)] + [(159, 168)])
        self.assertEqual([c for g in groups for c in g["members"]], candidates)
        self.assertNotIn(0xD4, [c["value"] for c in candidates])

    def test_only_the_pairs_are_transmitted_in_original_order(self):
        verdict, service, log, _ = run_grouped()
        self.assertEqual(verdict, "SET_COMPLETE_GROUPED")
        self.assertEqual(transmissions(service),
                         [f"RESEARCH_CHAIN 2 200 3000 0x{b} 0x48"
                          for group in GROUP_PREFIXES for b in group])
        verbs = {c.split()[0] for c in service.commands}
        self.assertEqual(verbs, {"STATUS", "RESEARCH_CHAIN", "RESEARCH_TX_READ",
                                 "RESEARCH_READ", "RESEARCH_OBSERVE"})
        detail = log.records[-1]["detail"]
        self.assertNotIn("group", log.records[-1])  # Completion belongs to no single group.
        self.assertIn("no per-prefix physical verification", detail)
        self.assertIn("no prefix is a verified negative", detail)

    def test_prompts_show_the_exact_prefixes_before_and_after_each_group(self):
        _, _, log, out = run_grouped()
        nexts = [line for line in out if line.startswith("NEXT: group")]
        dones = [line for line in out if line.startswith("DONE: group")]
        self.assertEqual(nexts[0], "NEXT: group 1, ranks 2-5, 4 prefix(es) in order: "
                                   "0xCA 0xA8 0xBA 0xAA")
        self.assertEqual(dones[1], "DONE: group 2, ranks 6-9, sent in order: "
                                   "0xFA 0x0A 0x1A 0x2A")
        self.assertEqual(len(nexts), 3)
        self.assertEqual(len(dones), 3)
        # Strict order: NEXT, then the pairs, then DONE, for every group.
        events = [r["event"] for r in log.records if r["event"] in
                  ("checkpoint", "group_start", "group_end")]
        self.assertEqual(events, ["checkpoint", "group_start", "group_end", "checkpoint"] * 3)
        phases = [r["phase"] for r in log.records if r["event"] == "checkpoint"]
        self.assertEqual(phases, ["before", "after"] * 3)

    def test_nothing_is_sent_before_the_first_start(self):
        verdict, service, _, _ = run_grouped(["abort"])
        self.assertEqual(verdict, "OPERATOR_ABORT")
        self.assertEqual(transmissions(service), [])
        verdict, service, _, _ = run_grouped([])  # Input closed at the first prompt.
        self.assertEqual(verdict, "CHECKPOINT_SKIPPED")
        self.assertEqual(transmissions(service), [])

    def test_next_group_never_starts_without_explicit_start(self):
        for script in (["start-group 1", "ok"],                      # Input ends.
                       ["start-group 1", "ok", "abort"],
                       ["start-group 1", "ok", "start-group 1", "go", "start", "abort"]):
            verdict, service, _, out = run_grouped(script)
            self.assertIn(verdict, ("CHECKPOINT_SKIPPED", "OPERATOR_ABORT"), script)
            self.assertEqual(len(chains(service)), 4, script)
        self.assertEqual(out.count("Type exactly: start-group 2"), 1)  # Wrong number.
        self.assertEqual(out.count("Answer one of: start-group, abort"), 2)  # "go", "start".

    def test_after_group_requires_an_explicit_ok(self):
        script = ["start-group 1", "", "fine", "ok all normal", "start-group 2", "ok",
                  "start-group 3", "ok"]
        verdict, service, log, out = run_grouped(script)
        self.assertEqual(verdict, "SET_COMPLETE_GROUPED")
        self.assertEqual(out.count("Answer one of: ok, anomaly, abort"), 2)

    def test_anomaly_stops_and_leaves_everything_as_it_is(self):
        for at_group, chains_sent in ((1, 4), (2, 8), (3, 10)):
            script = grouped_script(at_group)[:-1] + ["anomaly disc changed to 3"]
            verdict, service, log, _ = run_grouped(script)
            self.assertEqual(verdict, "OPERATOR_ANOMALY")
            self.assertEqual(len(chains(service)), chains_sent)
            self.assertEqual(service.commands[-1], "STATUS")  # Only a read afterwards.
            stop_status, stop = log.records[-2], log.records[-1]
            self.assertEqual(stop_status["event"], "stop_status")
            self.assertEqual(stop["group"], at_group)
            for prefix in GROUP_PREFIXES[at_group - 1]:
                self.assertIn(f"0x{prefix}", stop["detail"])
            self.assertIn("disc changed to 3", stop["detail"])

    def test_abort_after_a_group_stops(self):
        verdict, service, _, _ = run_grouped(["start-group 1", "abort"])
        self.assertEqual(verdict, "OPERATOR_ABORT")
        self.assertEqual(len(chains(service)), 4)

    def test_current_candidate_is_shown_prominently(self):
        _, _, _, out = run_grouped()
        banners = [line for line in out if line.startswith(">>> GROUP")]
        self.assertEqual(len(banners), 10)
        self.assertRegex(banners[0], r"^>>> GROUP 1 \| TRIAL 1/4 \| RANK 2 \| PREFIX 0xCA -> 0x48 "
                                     r"\| \d\d:\d\d:\d\d$")
        self.assertIn(">>> GROUP 3 | TRIAL 2/2 | RANK 11 | PREFIX 0x4A -> 0x48", banners[-1])

    def test_visual_abort_preserves_the_exact_trial_and_changes_nothing(self):
        verdict, service, log, _ = run_grouped(interrupt_after=7)
        self.assertEqual(verdict, "OPERATOR_ABORT")
        self.assertEqual(transmissions(service)[-1], "RESEARCH_CHAIN 2 200 3000 0x1A 0x48")
        trial = log.trials()[-1]
        self.assertEqual((trial["prefix"], trial["rank"], trial["group"], trial["verdict"]),
                         ("0x1A", 8, 2, "RX_SILENT"))
        self.assertTrue(trial["started"] and trial["ended"] and trial["rx"])
        self.assertIn("state=IDLE", trial["post_status"])
        stop_status, stop = log.records[-2], log.records[-1]
        self.assertEqual(stop_status["event"], "stop_status")
        self.assertEqual((stop["verdict"], stop["prefix"], stop["rank"], stop["group"]),
                         ("OPERATOR_ABORT", "0x1A", 8, 2))
        self.assertEqual(service.commands[-1], "STATUS")

    def test_automatic_stops_inside_a_group_keep_working(self):
        cases = {"POSSIBLE_DISC_CHANGE": {0x1A: {"rx": [(1150, 0xD4)]}},
                 "UNEXPECTED_RX": {0x0A: {"between_rx": [(2000, 0x7D)]}},
                 "TX_FAILURE": {0x1A: {"tx_fail": 1}},
                 "MONITOR_FAULT": {0x1A: {"monitor_failed": True}},
                 "ANOMALOUS_STATUS": {0x1A: {"post_status": sweep.MockService.IDLE.replace(
                     "receiver_integrity=HEALTHY", "receiver_integrity=UNCERTAIN")}}}
        for verdict, scenario in cases.items():
            result, service, log, _ = run_grouped(scenario=scenario)
            self.assertEqual(result, verdict)
            self.assertEqual(log.records[-1]["group"], 2)
            self.assertEqual(log.records[-2]["event"], "stop_status")
            self.assertFalse([r for r in log.records if r["event"] == "checkpoint"
                              and r["group"] == 2 and r["phase"] == "after"])

    def test_resume_keeps_group_numbers_and_skips_finished_groups(self):
        done = RANKED_26[1:5] + RANKED_26[5:7]
        script = ["start-group 2", "ok", "start-group 3", "ok"]
        result, service, log, _ = run_grouped(script, done=done)
        self.assertEqual(result, "SET_COMPLETE_GROUPED")
        self.assertEqual([r["group"] for r in log.records if r["event"] == "group_skipped"], [1])
        starts = [r for r in log.records if r["event"] == "group_start"]
        self.assertEqual([(g["group"], g["plan"]) for g in starts],
                         [(2, ["0x1A", "0x2A"]), (3, ["0x3A", "0x4A"])])

    def test_correlation_handles_a_group_split_across_sessions(self):
        """Regression: operator REPL traffic between two parts of one group is not inside it."""
        first_log = run_grouped(["start-group 1", "ok", "start-group 2"], interrupt_after=6)[2]
        part1 = [{"event": "run"}] + first_log.records
        part1_end = max(int(sweep.xs8_repl.fields(r["done"])["deadline_ns"]) // 1000
                        for r in part1 if r.get("group") == 2 and r.get("done"))
        # The resumed session: group 2's remaining prefixes, much later.
        _, service, second_log, _ = run_grouped(["start-group 2", "ok", "start-group 3", "ok"],
                                                done=RANKED_26[1:7])
        offset = part1_end + 300_000_000  # 300 s of operator time, in microseconds.

        def shift(record):
            moved = dict(record)
            for key in ("done",):
                if key in moved:
                    d = sweep.xs8_repl.fields(moved[key])
                    moved[key] = moved[key].replace(
                        f"start_ns={d['start_ns']}", f"start_ns={int(d['start_ns']) + offset * 1000}"
                    ).replace(f"deadline_ns={d['deadline_ns']}",
                              f"deadline_ns={int(d['deadline_ns']) + offset * 1000}")
            if "tx" in moved:
                lines = []
                for line in moved["tx"]:
                    d = sweep.xs8_repl.fields(line)
                    lines.append(line.replace(f"tx_start_ns={d['tx_start_ns']}",
                                              f"tx_start_ns={int(d['tx_start_ns']) + offset * 1000}")
                                 .replace(f"tx_end_ns={d['tx_end_ns']}",
                                          f"tx_end_ns={int(d['tx_end_ns']) + offset * 1000}"))
                moved["tx"] = lines
            return moved
        part2 = [{"event": "run"}] + [shift(r) for r in second_log.records]
        records = part1 + part2
        lines = CorrelationTest().journal(records)
        gap = part1_end + 120_000_000  # Operator REPL traffic between the parts.
        lines += [f"x: mono_us={gap} XS8 TX 0xA7 timing result=TX_OK",
                  f"x: mono_us={gap + 30} XS8 OBSERVED_BUS status=VALID waveform_lsb=0xA7 "
                  "legacy_msb_short1=0x1A",
                  f"x: mono_us={gap + 5_000_000} XS8 OBSERVED_BUS status=VALID "
                  "waveform_lsb=0x41 legacy_msb_short1=0x7D"]
        result = sweep.correlate(records, lines)
        self.assertEqual((result["non_echo"], result["unlogged_tx"], result["journal_tx_missing"]),
                         ([], [], []))
        self.assertEqual(len(result["between_groups"]), 3)
        # A frame inside the resumed part of group 2 is still an anomaly.
        resumed = [r for r in part2 if r.get("event") == "trial" and r.get("group") == 2][0]
        inside = int(sweep.xs8_repl.fields(resumed["tx"][1])["tx_end_ns"]) // 1000 + 1_000_000
        lines.append(f"x: mono_us={inside} XS8 OBSERVED_BUS status=VALID waveform_lsb=0xD4 "
                     "legacy_msb_short1=0xD4")
        result = sweep.correlate(records, lines)
        self.assertEqual([(e["legacy"], e["group"]) for e in result["non_echo"]], [("0xD4", 2)])

    def test_correlation_separates_group_spans_from_operator_time(self):
        _, _, log, _ = run_grouped()
        records = log.records
        lines = CorrelationTest().journal(records)
        group1_end = max(int(sweep.xs8_repl.fields(r["done"])["deadline_ns"]) // 1000
                         for r in records if r.get("group") == 1 and r.get("done"))
        group2 = [t for t in log.trials() if t["group"] == 2][0]
        inside = int(sweep.xs8_repl.fields(group2["tx"][1])["tx_end_ns"]) // 1000 + 1_500_000
        lines += [f"x: mono_us={group1_end + 10_000_000} XS8 OBSERVED_BUS status=VALID "
                  "waveform_lsb=0x84 legacy_msb_short1=0xDE",
                  f"x: mono_us={group1_end + 11_000_000} XS8 TX 0x38 timing result=TX_OK"]
        result = sweep.correlate(records, lines)
        self.assertEqual((result["non_echo"], result["unlogged_tx"]), ([], []))
        self.assertEqual(len(result["between_groups"]), 2)
        self.assertEqual(result["echoes"], len(log.trials()) * 2)
        lines.append(f"x: mono_us={inside} XS8 OBSERVED_BUS status=VALID waveform_lsb=0xD4 "
                     "legacy_msb_short1=0xD4")
        result = sweep.correlate(records, lines)
        self.assertEqual([(e["legacy"], e["group"]) for e in result["non_echo"]], [("0xD4", 2)])


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

    def test_extended_plan_listing_prints_the_approval_digest(self):
        code, text = self.main("--candidates", str(REAL_CANDIDATES), "--set", "filtered-169",
                               "--unattended-rx-only", "--observe-delay", "--delay-s", "8",
                               "--list")
        self.assertEqual(code, 0)
        self.assertIn("142 candidate(s)", text)
        self.assertIn("delay 8 s, passive observation through delays, digest fd87fc32fb88cf68",
                      text)
        self.assertEqual(text.count(": already tested"), 26)
        self.assertIn("skip 0xD4 (rank 169): reserved for an attended experiment", text)

    def grouped_args(self, *extra):
        return ("--candidates", str(REAL_CANDIDATES), "--set", "filtered-169", "--grouped", "12",
                "--observe-delay", "--delay-s", "8") + extra

    def test_grouped_listing_digest_and_groups(self):
        code, text = self.main(*self.grouped_args("--list"))
        self.assertEqual(code, 0)
        self.assertIn("mode grouped-operator, delay 8 s, groups of 12, passive observation "
                      "through delays, digest a98d41d7120e1070", text)
        self.assertIn("group 1: ranks 27-38, 12 prefix(es): 0x00 0x01", text)
        self.assertIn("group 12: ranks 159-168, 10 prefix(es): 0xF3", text)
        self.assertIn("skip 0xD4 (rank 169): reserved for an attended experiment", text)

    def test_grouped_digest_covers_mode_and_group_size(self):
        suffix, candidates, _ = sweep.load_candidates(REAL_CANDIDATES, "filtered-169",
                                                      unattended=True)

        def digest(mode=sweep.GROUPED, size=12):
            return sweep.approval_digest("filtered-169", suffix, 200, 3000, candidates, mode=mode,
                                         delay_s=8.0, observe_delay=True, group_size=size)
        self.assertEqual(digest(), "a98d41d7120e1070")
        self.assertEqual(digest(sweep.UNATTENDED, None), "fd87fc32fb88cf68")
        self.assertEqual(len({digest(), digest(size=11), digest(size=10), digest(sweep.UNATTENDED)}), 4)

    def test_other_digests_cannot_start_a_grouped_run(self):
        for digest in ("fd87fc32fb88cf68", "c2d03a77f3e2396f"):
            with patch.object(sweep, "SocketService") as real:
                code, text = self.main(*self.grouped_args("--log", "/dev/null", "--approve", digest),
                                       stdin=sweep.START_PHRASE)
                self.assertEqual(code, 2)
                self.assertIn("Refused", text)
                real.assert_not_called()

    def test_grouped_option_conflicts(self):
        for extra in (("--unattended-rx-only",), ("--max-trials", "3")):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                self.main(*self.grouped_args("--dry-run", *extra))

    def test_grouped_dry_run_end_to_end(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.jsonl"
            with patch.object(sweep.xs8_repl, "request") as real:
                code, text = self.main(*self.grouped_args("--dry-run", "--auto-observe",
                                                          "--log", str(log)))
                real.assert_not_called()
            self.assertEqual(code, 0)
            self.assertIn("STOP SET_COMPLETE_GROUPED", text)
            records = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual((records[0]["mode"], records[0]["group_size"]),
                             ("grouped-operator", 12))
            self.assertEqual(len([r for r in records if r["event"] == "trial"]), 142)
            self.assertEqual([r["phase"] for r in records if r["event"] == "checkpoint"],
                             ["before", "after"] * 12)
            self.assertFalse([r for r in records if r["event"] in ("probe", "digit_check")])

    def test_observe_delay_needs_unattended(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            self.main("--candidates", str(CANDIDATES), "--set", "structural-26", "--dry-run",
                      "--observe-delay")

    def test_correlation_cli_exit_codes(self):
        records = CorrelationTest().logged_run()
        first = [r for r in records if r["event"] == "trial"][0]
        with tempfile.TemporaryDirectory() as directory:
            log, journal = Path(directory) / "run.jsonl", Path(directory) / "journal.txt"
            log.write_text("\n".join(json.dumps(r) for r in records) + "\n")
            journal.write_text("\n".join(CorrelationTest().journal(records)) + "\n")
            code, text = self.main("--log", str(log), "--correlate-journal", str(journal))
            self.assertEqual(code, 0)
            self.assertIn("CORRELATION CLEAN", text)
            journal.write_text("\n".join(CorrelationTest().journal(records, [
                f"x: mono_us={int(sweep.xs8_repl.fields(first['tx'][1])['tx_end_ns']) // 1000 + 1500000} "
                "XS8 OBSERVED_BUS status=VALID waveform_lsb=0xD4 legacy_msb_short1=0xD4"])) + "\n")
            code, text = self.main("--log", str(log), "--correlate-journal", str(journal))
            self.assertEqual(code, 1)
            self.assertIn("FOUND FRAMES", text)

    def test_one_set_per_run(self):
        code, text = self.main("--candidates", str(CANDIDATES), "--set", "structural-26",
                               "--dry-run", "--auto-observe", "--delay-s", "0")
        self.assertEqual(code, 0)
        self.assertIn("STOP SET_COMPLETE", text)
        self.assertNotIn("0x00 ->", text)  # First byte unique to filtered-169.


if __name__ == "__main__":
    unittest.main()
