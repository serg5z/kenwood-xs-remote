import importlib.util
import os
import socketserver
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "xs8_repl", Path(__file__).resolve().parents[1] / "xs8_repl.py")
repl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repl)


TOKEN = "777777"


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        # Serve every line on the connection: CONTEXT sessions keep one open.
        with self.server.lock:
            self.server.next_connection += 1
            self.connection_id = self.server.next_connection
        for raw in self.rfile:
            if not self.answer(raw.decode().strip()):
                return

    def answer(self, command):
        with self.server.lock:
            self.server.commands.append(command)
            self.server.connections.append((self.connection_id, command))
            if (command.startswith("RESEARCH_TX_READ ") and
                    self.server.drop_next_tx_read):
                self.server.drop_next_tx_read = False
                return False  # Fake a peer closing before its reply.
            if command == "STATUS":
                reply = "OK state=IDLE stop_owed=0 monitor=HEALTHY"
            elif command == "RESEARCH_CONTEXT_PREPARE MUTE CONFIRM_NO_TX":
                self.server.context_active = True
                reply = ("OK RESEARCH_CONTEXT_PREPARED token=99 profile=MUTE "
                         "mode=PREPARATION_ONLY context_entered=0 recovery_ready=0")
            elif command == "RESEARCH_CONTEXT_STATUS":
                reply = ("OK RESEARCH_CONTEXT_STATUS state=" +
                         ("PREPARED" if self.server.context_active else "NONE") +
                         " context_entered=0 panel_check=NOT_OBSERVED")
            elif command.startswith("RESEARCH_CONTEXT_OBSERVE 99 "):
                reply = "OK RESEARCH_OBSERVE id=1 tx_attempted=0"
            elif command == "RESEARCH_CONTEXT_HEARTBEAT 99":
                reply = "OK RESEARCH_CONTEXT_LEASE_RENEWED context_entered=0"
            elif command == "RESEARCH_CONTEXT_RUN 99":
                reply = "ERR RESEARCH_CONTEXT reason=RECOVERY_EVIDENCE_MISSING"
            elif command == "RESEARCH_CONTEXT_CANCEL 99":
                self.server.context_active = False
                reply = "OK RESEARCH_CONTEXT_CANCELLED context_entered=0 tx_attempted=0"
            elif command == "QUERY_STATUS":
                reply = "OK QUERY_STATUS source_inferred=CD"
            elif command.startswith("RESEARCH_SEND "):
                reply = ("OK RESEARCH_TX gpio_tx=TX_OK waveform=MATCH "
                         "receiver_execution=UNVERIFIED id=1 "
                         "tx_start_ns=10 tx_end_ns=100 deadline_ns=200")
            elif command.startswith("RESEARCH_CHAIN "):
                reply = "OK RESEARCH_CHAIN id=1 count=2 gap_ms=200 window_ms=1500"
            elif command == "RESEARCH_TX_READ 1 0":
                if self.server.tx_wait_once:
                    self.server.tx_wait_once = False
                    reply = "OK RESEARCH_TX_WAIT id=1 count=0 planned=2"
                else:
                    reply = ("OK RESEARCH_TX_RECORD id=1 index=0 requested=0xFC "
                             "tx_start_ns=10 tx_end_ns=100 gpio_tx=TX_OK waveform=MATCH")
            elif command == "RESEARCH_TX_READ 1 1":
                reply = ("OK RESEARCH_TX_RECORD id=1 index=1 requested=0xEC "
                         "tx_start_ns=200000100 tx_end_ns=200000200 "
                         "gpio_tx=TX_OK waveform=MATCH")
            elif command == "RESEARCH_TX_READ 1 2":
                reply = "OK RESEARCH_TX_DONE id=1 count=2 planned=2 aborted=0"
            elif command.startswith("RESEARCH_OBSERVE "):
                reply = "OK RESEARCH_OBSERVE id=1 start_ns=10 deadline_ns=200"
            elif command == "RESEARCH_READ 1 0":
                if self.server.busy_next_read:
                    self.server.busy_next_read = False
                    reply = "ERR BUSY"
                else:
                    self.server.first_reads += 1
                    reply = ("OK RESEARCH_WAIT id=1 count=0" if
                             self.server.first_reads == 1 else
                             "OK RESEARCH_RECORD id=1 index=0 kind=FRAME "
                             "start_ns=20 end_ns=100 waveform_lsb=0x9C "
                             "legacy_msb_short1=0xC6 valid=1")
            elif command == "RESEARCH_READ 1 1":
                reply = "OK RESEARCH_DONE id=1 count=1 overflow=0"
            elif command == "RESEARCH_CANCEL 1":
                reply = "OK RESEARCH_CANCEL id=1"
            elif command == "CONTEXT_SESSION_ENTER CONFIRM_ATTENDED 1500":
                reply = ("OK CONTEXT_SESSION_ENTERED token=" + TOKEN + " byte=0x17 "
                         "gpio_tx=TX_OK waveform=MATCH context_may_be_open=1 "
                         "lease_ms=3000 max_ms=300000 receiver_execution=UNVERIFIED "
                         "capture_id=1 frames=0x17:TX_OK:MATCH:1:2")
            elif command.startswith(f"CONTEXT_SESSION_SEND {TOKEN} "):
                reply = self.server.send_reply or (
                    "OK CONTEXT_SESSION_SENT byte=" + command.split()[2] +
                    " gpio_tx=TX_OK waveform=MATCH context_may_be_open=1 "
                    "receiver_execution=UNVERIFIED capture_id=1 frames=x")
            elif command == f"CONTEXT_SESSION_HEARTBEAT {TOKEN}":
                reply = self.server.heartbeat_reply
            elif command == f"CONTEXT_SESSION_EXIT {TOKEN} 1500":
                reply = ("OK CONTEXT_SESSION_EXITED byte=0x07 root_attempts=1 "
                         "root_gpio_tx=TX_OK root_waveform=MATCH context_may_be_open=0 "
                         "state=IDLE root_verified=NOT_OBSERVED "
                         "receiver_execution=UNVERIFIED capture_id=1 frames=x")
            elif command == "CONTEXT_SESSION_STATUS":
                reply = self.server.context_status_reply
            else:
                reply = "ERR UNKNOWN_COMMAND"
        self.wfile.write((reply + "\n").encode())
        self.wfile.flush()
        return True


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True


def repl_token():
    return TOKEN


class ReplTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.temp.name, "service.sock")
        self.server = Server(self.path, Handler)
        self.server.lock = threading.Lock()
        self.server.commands = []
        self.server.first_reads = 0
        self.server.busy_next_read = False
        self.server.tx_wait_once = False
        self.server.drop_next_tx_read = False
        self.server.context_active = False
        self.server.connections = []
        self.server.next_connection = 0
        self.server.send_reply = None
        self.server.heartbeat_reply = "OK CONTEXT_SESSION_LEASE_RENEWED"
        self.server.context_status_reply = (
            "OK CONTEXT_SESSION_STATUS open=1 sends=1 state=RESEARCH_CONTEXT "
            "context_may_be_open=1")
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.output = []
        self.console = repl.Console(self.path, self.output.append,
                                    lambda _: None, context_heartbeat_s=0.05)

    def tearDown(self):
        self.console.close_context()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def test_status_query_and_one_send(self):
        self.console.command("status")
        self.console.command("query")
        self.console.command("send 0x9C")
        self.assertEqual(self.server.commands.count(
            "RESEARCH_SEND 0x9C 1500"), 1)
        self.assertIn("RESEARCH_READ 1 1", self.server.commands)
        self.assertTrue(any("waveform_lsb=0x9C" in x for x in self.output))
        self.assertEqual(len(self.console.history), 1)

    def test_context_preparation_records_only_passive_rx_and_run_is_blocked(self):
        self.console.confirm = lambda _: "PREPARE MUTE NO TX"
        self.console.command("context prepare MUTE")
        self.console.command("context run")
        self.console.command("context status")
        self.assertIn("RESEARCH_CONTEXT_OBSERVE 99 1500", self.server.commands)
        self.assertEqual(self.server.commands.count("RESEARCH_CONTEXT_RUN 99"), 1)
        self.assertTrue(any("RECOVERY_EVIDENCE_MISSING" in entry for entry in self.output))
        self.assertTrue(any("waveform_lsb=0x9C" in entry for entry in self.output))
        self.assertFalse(any(command.startswith(("RESEARCH_SEND", "RESEARCH_CHAIN"))
                             or command in ("MUTE", "QUERY_STATUS")
                             for command in self.server.commands))
        self.assertFalse(any("token=99" in entry or "CONTEXT_OBSERVE 99" in entry
                             for entry in self.output + self.console.history))
        self.console.command("quit")
        self.assertIn("RESEARCH_CONTEXT_CANCEL 99", self.server.commands)

    def test_context_requires_explicit_confirmation_and_one_named_profile(self):
        self.console.confirm = lambda _: "no"
        self.console.command("context prepare MUTE")
        self.console.command("context enter")  # Declined phrase: nothing sent.
        for command in ("context prepare PHONO", "context send 0xD8", "context send"):
            with self.assertRaises(ValueError):
                self.console.command(command)
        self.assertEqual(self.server.commands, [])
        self.assertTrue(any("CONTEXT entry declined" in x for x in self.output))

    # ---- interactive CONTEXT session ----

    def enter(self):
        self.console.confirm = lambda _: repl.CONTEXT_PHRASE
        self.console.command("context enter")
        self.assertIsNotNone(self.console.session)

    def owner_commands(self):
        owner = next(c for c, cmd in self.server.connections
                     if cmd.startswith("CONTEXT_SESSION_ENTER"))
        return [cmd for c, cmd in self.server.connections if c == owner]

    def test_context_session_uses_one_owner_connection_and_masks_token(self):
        self.enter()
        self.console.command("send 0x78")
        time.sleep(0.2)  # Heartbeats run on the owner connection meanwhile.
        self.console.command("context status")
        with self.assertRaises(ValueError):
            self.console.command("context verify")  # ROOT checks only after exit.
        self.console.command("context exit")
        self.assertIsNone(self.console.session)
        owned = self.owner_commands()
        self.assertEqual(owned[0], "CONTEXT_SESSION_ENTER CONFIRM_ATTENDED 1500")
        self.assertIn(f"CONTEXT_SESSION_SEND {repl_token()} 0x78 1500", owned)
        self.assertIn(f"CONTEXT_SESSION_HEARTBEAT {repl_token()}", owned)
        self.assertEqual(owned[-1], f"CONTEXT_SESSION_EXIT {repl_token()} 1500")
        self.assertEqual(self.server.commands.count(
            f"CONTEXT_SESSION_SEND {repl_token()} 0x78 1500"), 1)
        self.assertIn("RESEARCH_READ 1 1", self.server.commands)
        self.assertFalse(any(repl_token() in x for x in self.output + self.console.history))
        self.assertTrue(any("ROOT verification" in x for x in self.output))
        self.console.command("context verify")
        self.assertIn("QUERY_STATUS", self.server.commands)
        # After a clean exit, send is ROOT research TX again.
        self.console.command("send 0x9C")
        self.assertEqual(self.server.commands.count("RESEARCH_SEND 0x9C 1500"), 1)

    def test_context_send_validation_and_uncertain_result_is_never_retried(self):
        self.enter()
        for command in ("send 0x78 0x88", "send 0x78 --gap-ms 200",
                        "send 0x78 0x88 --gap-ms 200", "send 0x78 750", "send 0x123",
                        "send 78", "send 0x78 --observe-ms 0", "send", "send --observe-ms 10"):
            with self.subTest(command=command), self.assertRaises(ValueError):
                self.console.command(command)
        for byte, text in (("0x17", "nested entry"), ("0x07", "context exit"),
                           ("0xE9", "volume motor"), ("0xf9", "volume motor")):
            with self.subTest(byte=byte):
                with self.assertRaisesRegex(ValueError, text):
                    self.console.command("send " + byte)
        # Nothing reached the service: no CONTEXT send and never a ROOT fallback.
        self.assertFalse(any(c.startswith(("CONTEXT_SESSION_SEND", "RESEARCH_SEND",
                                           "RESEARCH_CHAIN")) for c in self.server.commands))
        self.console.command("send 0x78 --observe-ms 250")
        self.assertIn(f"CONTEXT_SESSION_SEND {repl_token()} 0x78 250", self.server.commands)
        self.server.send_reply = "ERR CONTEXT_SESSION reason=NESTED_ENTRY_FORBIDDEN"
        self.console.command("send 0x55")  # A service-side denial is explained too.
        self.assertTrue(any("nested entry" in x for x in self.output))
        self.server.send_reply = ("ERR CONTEXT_SESSION_SEND_UNCERTAIN byte=0x88 "
                                  "gpio_tx=TX_OK waveform=MISSING root_attempts=1 "
                                  "context_may_be_open=0 state=FAULT_LATCHED capture_id=1")
        self.console.command("send 0x88")
        self.assertIsNone(self.console.session)
        # Uncertain end: neither a CONTEXT nor a ROOT send, enter or verify goes out.
        for command in ("send 0x88", "send 0x9C", "context enter", "context verify"):
            with self.subTest(command=command), self.assertRaises(ValueError):
                self.console.command(command)
        sends = [c for c in self.server.commands
                 if c.startswith(("CONTEXT_SESSION_SEND", "RESEARCH_SEND", "RESEARCH_CHAIN"))]
        self.assertEqual(sends, [f"CONTEXT_SESSION_SEND {repl_token()} 0x78 250",
                                 f"CONTEXT_SESSION_SEND {repl_token()} 0x55 1500",
                                 f"CONTEXT_SESSION_SEND {repl_token()} 0x88 1500"])
        self.assertNotIn(f"CONTEXT_SESSION_EXIT {repl_token()} 1500", self.server.commands)
        self.assertNotIn("QUERY_STATUS", self.server.commands)
        # Only the service's own report of a closed CONTEXT re-enables ROOT send.
        self.console.command("context status")
        with self.assertRaises(ValueError):
            self.console.command("send 0x9C")
        self.server.context_status_reply = ("OK CONTEXT_SESSION_STATUS open=0 sends=2 "
                                            "state=FAULT_LATCHED context_may_be_open=0")
        self.console.command("context status")
        self.console.command("send 0x9C")
        self.assertEqual(self.server.commands.count("RESEARCH_SEND 0x9C 1500"), 1)

    def test_quit_exits_context_and_lost_lease_blocks_sends(self):
        self.enter()
        self.console.command("quit")
        self.assertIn(f"CONTEXT_SESSION_EXIT {repl_token()} 1500", self.server.commands)
        self.assertIsNone(self.console.session)
        self.server.heartbeat_reply = "ERR CONTEXT_SESSION reason=NOT_ACTIVE"
        self.enter()
        deadline = time.monotonic() + 2
        while not self.console.session.lost:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)
        with self.assertRaises(ValueError):
            self.console.command("send 0x78")
        self.assertFalse(any(c.startswith(("CONTEXT_SESSION_SEND", "RESEARCH_SEND"))
                             for c in self.server.commands))

    def test_context_interrupt_releases_preparation_without_generic_cancel_or_tx(self):
        self.console.confirm = lambda _: "PREPARE MUTE NO TX"
        self.console.pause = lambda _: (_ for _ in ()).throw(KeyboardInterrupt)
        self.console.command("context prepare MUTE")
        self.assertIn("RESEARCH_CONTEXT_CANCEL 99", self.server.commands)
        self.assertNotIn("RESEARCH_CANCEL 1", self.server.commands)
        self.assertIsNone(self.console.context_token)
        self.assertTrue(any("no XS8 TX was attempted" in entry for entry in self.output))

    def test_window_and_other_client_between_polls(self):
        self.console.command("set observe 750")
        self.server.busy_next_read = True
        self.console.pause = lambda _: self.assertIn(
            "state=IDLE", repl.request(self.path, "STATUS"))
        self.console.command("observe 750")
        self.assertEqual(self.server.commands.count("RESEARCH_OBSERVE 750"), 1)
        self.assertIn("STATUS", self.server.commands)
        self.assertGreaterEqual(self.server.commands.count("RESEARCH_READ 1 0"), 3)
        self.assertLess(self.server.commands.index("RESEARCH_READ 1 0"),
                        self.server.commands.index("STATUS"))
        self.assertTrue(any("RESEARCH_DONE" in x for x in self.output))

    def test_invalid_input_never_dispatches(self):
        for command in ("send 0x123", "send 9C", "send 0x9C 5001",
                        "observe 0", "set observe -1"):
            with self.assertRaises(ValueError):
                self.console.command(command)
        self.assertEqual(self.server.commands, [])

    def test_interrupt_cancels_observation_without_retransmission(self):
        self.console.pause = lambda _: (_ for _ in ()).throw(KeyboardInterrupt)
        self.console.command("send 0x9C")
        self.assertIn("RESEARCH_CANCEL 1", self.server.commands)
        self.assertEqual(self.server.commands.count("RESEARCH_SEND 0x9C 1500"), 1)

    def test_chain_is_one_request_with_individual_tx_reports(self):
        self.server.tx_wait_once = True
        self.console.pause = lambda _: self.assertIn(
            "state=IDLE", repl.request(self.path, "STATUS"))
        self.console.command("send 0xFC 0xEC --gap-ms 200")
        self.assertEqual(self.server.commands.count(
            "RESEARCH_CHAIN 2 200 1500 0xFC 0xEC"), 1)
        self.assertIn("RESEARCH_TX_READ 1 2", self.server.commands)
        self.assertIn("RESEARCH_READ 1 1", self.server.commands)
        self.assertIn("STATUS", self.server.commands)
        self.assertTrue(any("requested=0xFC" in x for x in self.output))
        self.assertTrue(any("requested=0xEC" in x for x in self.output))

    def test_chain_observation_survives_closed_read_socket(self):
        self.server.drop_next_tx_read = True
        self.console.command("send 0xFC 0xEC --gap-ms 200")
        self.assertEqual(self.server.commands.count(
            "RESEARCH_CHAIN 2 200 1500 0xFC 0xEC"), 1)
        self.assertGreaterEqual(self.server.commands.count(
            "RESEARCH_TX_READ 1 0"), 2)
        self.assertTrue(any("RESEARCH_TX_DONE" in x for x in self.output))
        self.assertTrue(any("RESEARCH_DONE" in x for x in self.output))

    def test_broken_pipe_on_read_retries_without_resending_chain(self):
        original = repl.request
        attempts = [0]

        def transport(path, command):
            if command == "RESEARCH_READ 1 0" and attempts[0] == 0:
                attempts[0] += 1
                raise BrokenPipeError("fake single-client handoff race")
            return original(path, command)

        with patch.object(repl, "request", side_effect=transport):
            self.console.command("send 0xFC 0xEC --gap-ms 200")
        self.assertEqual(attempts[0], 1)
        self.assertEqual(self.server.commands.count(
            "RESEARCH_CHAIN 2 200 1500 0xFC 0xEC"), 1)
        self.assertTrue(any("RESEARCH_DONE" in x for x in self.output))

    def test_observation_retry_expires_without_resending_tx(self):
        with patch.object(repl, "request", side_effect=BrokenPipeError):
            with self.assertRaisesRegex(ConnectionError,
                                        "observation read unavailable"):
                self.console._read_session("RESEARCH_READ 1 0",
                                           time.monotonic() - 1)
        self.assertEqual(self.server.commands, [])

    def test_parse_chain_limits_and_legacy_send(self):
        self.assertEqual(repl.parse_send(["send", "0x9C", "750"], 1500),
                         "RESEARCH_SEND 0x9C 750")
        self.assertEqual(repl.parse_send(
            ["send", "0xFC", "0xEC", "--observe-ms", "750"], 1500),
            "RESEARCH_CHAIN 2 200 750 0xFC 0xEC")
        for args in (["send"], ["send", "0xFC", "0xEC", "0xAA", "0xBB", "0xCC"],
                     ["send", "0xFC", "0xEC", "--gap-ms", "0"],
                     ["send", "0xFC", "0xEC", "--gap-ms", "1001"],
                     ["send", "0xFC", "0xEC", "--observe-ms", "5001"],
                     ["send", "0xFC", "0xEC", "--gap-ms", "200", "--gap-ms", "200"]):
            with self.subTest(args=args), self.assertRaises(ValueError):
                repl.parse_send(args, 1500)


if __name__ == "__main__":
    unittest.main()
