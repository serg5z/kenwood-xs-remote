import importlib.util
import os
import socketserver
import tempfile
import threading
import unittest
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
            if command == "STATUS":
                reply = "OK state=IDLE stop_owed=0 monitor=HEALTHY"
            elif command == "QUERY_STATUS":
                reply = "OK QUERY_STATUS source_inferred=CD"
            elif command.startswith("RESEARCH_SEND "):
                reply = ("OK RESEARCH_TX gpio_tx=TX_OK waveform=MATCH "
                         "receiver_execution=UNVERIFIED id=1 "
                         "tx_start_ns=10 tx_end_ns=100 deadline_ns=200")
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


if __name__ == "__main__":
    unittest.main()
