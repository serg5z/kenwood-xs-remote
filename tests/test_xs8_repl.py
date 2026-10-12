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


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        command = self.rfile.readline().decode().strip()
        with self.server.lock:
            self.server.commands.append(command)
            if (command.startswith("RESEARCH_TX_READ ") and
                    self.server.drop_next_tx_read):
                self.server.drop_next_tx_read = False
                return  # Fake a peer closing before its reply.
            if command == "STATUS":
                reply = "OK state=IDLE stop_owed=0 monitor=HEALTHY"
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
            else:
                reply = "ERR UNKNOWN_COMMAND"
        self.wfile.write((reply + "\n").encode())


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True


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
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.output = []
        self.console = repl.Console(self.path, self.output.append,
                                    lambda _: None)

    def tearDown(self):
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
