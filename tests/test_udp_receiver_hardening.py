"""Deterministic receiver regression tests, requiring only the standard library."""
import importlib.util
from pathlib import Path
import threading
import socket
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

spec = importlib.util.spec_from_file_location("receiver_hardening_proxy", Path(__file__).resolve().parents[1] / "esp32-proxy.py")
proxy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proxy)
JPEG = b"\xff\xd8abcdef\xff\xd9"


def packets(frame_id, jpeg=JPEG, version=2):
    chunks = [jpeg[:5], jpeg[5:]]
    return [packet(frame_id, i, chunk, len(jpeg), 2, version, zlib.crc32(jpeg)) for i, chunk in enumerate(chunks)]


def packet(frame_id, index, payload, length=10, count=2, version=2, crc=0):
    if version == 2:
        header = proxy.UDP_FRAME_HEADER_V2.pack(b"ZMC2", frame_id, length, crc, index, count, len(payload))
    else:
        header = proxy.UDP_FRAME_HEADER_V1.pack(b"ZMC1", frame_id, length, index, count, len(payload))
    return header + payload


class ReceiverHardeningTests(unittest.TestCase):
    def setUp(self):
        self.clock = patch.object(proxy.time, "monotonic", return_value=100.0)
        self.now = self.clock.start()
        self.addCleanup(self.clock.stop)
        self.asm = proxy.FramedUdpJpegAssembler(1024)

    def complete(self, frame_id, sender=None, jpeg=JPEG, version=2):
        out = []
        for part in packets(frame_id, jpeg, version):
            out += self.asm.feed(part, sender)
        return out

    def test_duplicate_complete_and_late_completion_cannot_rewind(self):
        old = packets(10)
        self.assertEqual([], self.asm.feed(old[0]))
        self.assertEqual([JPEG], self.complete(11))
        self.assertEqual([], self.asm.feed(old[1]))
        self.assertEqual([], self.complete(11))
        self.assertEqual([], self.complete(9))
        self.assertEqual([JPEG], self.complete(12))

    def test_wraparound_is_forward_but_half_range_is_ambiguous(self):
        for frame_id in (0xFFFFFFFE, 0xFFFFFFFF, 0, 1):
            self.assertEqual([JPEG], self.complete(frame_id))
        self.assertEqual([], self.complete(0xFFFFFFFF))
        self.assertEqual([], self.complete(0x80000001))

    def test_reboot_recovers_without_stale_traffic_extending_timeout(self):
        self.assertEqual([JPEG], self.complete(200))
        self.now.return_value = 104.9
        self.assertEqual([], self.complete(1))
        self.now.return_value = 105.0
        self.assertEqual([JPEG], self.complete(2))
        self.assertEqual([], self.complete(1))

    def test_senders_do_not_mix_chunks_or_sequence_state(self):
        a, b = ("127.0.0.1", 1000), ("127.0.0.1", 1001)
        parts = packets(1)
        self.assertEqual([], self.asm.feed(parts[0], a))
        self.assertEqual([], self.asm.feed(parts[1], b))
        self.assertEqual([JPEG], self.asm.feed(parts[1], a))
        self.assertEqual([JPEG], self.asm.feed(parts[0], b))
        self.assertEqual([], self.complete(1, a))

    def test_oversized_payload_and_impossible_chunk_counts_are_rejected(self):
        for part in (packet(1, 0, b"x" * 11), packet(2, 0, b"x", count=11), packet(3, 0, b"x", length=3)):
            self.assertEqual([], self.asm.feed(part))
        self.assertEqual({}, self.asm._frames)

    def test_accumulated_payload_cannot_exceed_declared_length(self):
        self.asm.feed(packet(1, 0, b"x" * 6))
        self.assertEqual([], self.asm.feed(packet(1, 1, b"x" * 6)))
        state = self.asm._frames[(None, 1)]
        self.assertTrue(state["invalid"])
        self.assertEqual(0, state["received_bytes"])
        self.assertEqual([], state["chunks"])

    def test_identical_duplicate_is_safe_conflicting_duplicate_is_poisoned(self):
        parts = packets(1)
        self.asm.feed(parts[0])
        self.asm.feed(parts[0])
        self.assertEqual([JPEG], self.asm.feed(parts[1]))
        parts = packets(2)
        self.asm.feed(parts[0])
        bad = parts[0][:-1] + b"!"
        self.assertEqual([], self.asm.feed(bad))
        self.assertEqual([], self.asm.feed(parts[1]))
        self.assertEqual([], self.complete(2))

    def test_conflicting_metadata_is_poisoned(self):
        self.asm.feed(packets(1)[0])
        self.assertEqual([], self.asm.feed(packet(1, 1, b"abcde", length=11)))
        self.assertEqual([], self.complete(1))

    def test_pending_and_sender_history_are_bounded(self):
        for i in range(100):
            self.asm.feed(packets(i)[0], sender=i)
        self.assertLessEqual(len(self.asm._frames), proxy.UDP_FRAME_MAX_PENDING)
        for i in range(100):
            self.complete(i, sender=i)
        self.assertLessEqual(len(self.asm._latest), proxy.UDP_FRAME_MAX_PENDING)
        self.assertLessEqual(len(self.asm._frames), proxy.UDP_FRAME_MAX_PENDING)

    def test_incomplete_frame_expires_despite_duplicates(self):
        parts = packets(1)
        self.asm.feed(parts[0])
        self.now.return_value = 104.0
        self.asm.feed(parts[0])
        self.now.return_value = 105.0
        self.assertEqual([], self.asm.feed(parts[1]))

    def test_zmc1_still_works(self):
        self.assertEqual([JPEG], self.complete(1, version=1))


class FrameBufferWaitingTests(unittest.TestCase):
    def test_no_frame_wait_has_a_valid_frame_predicate(self):
        fb = proxy.FrameBuffer()
        def check_wait(predicate, timeout):
            self.assertEqual(5.0, timeout)
            self.assertFalse(predicate())
            fb._jpeg, fb._seq = JPEG, 1
            self.assertTrue(predicate())
        with patch.object(fb._cond, "wait_for", side_effect=check_wait) as wait:
            self.assertEqual(JPEG, fb.wait_next(-1, 5.0)[0])
            wait.assert_called_once()

    def test_first_frame_wakes_waiter_and_latest_is_immediate(self):
        fb = proxy.FrameBuffer()
        entered = threading.Event()
        original = fb._cond.wait
        def wait(timeout=None):
            entered.set()
            return original(timeout)
        results = []
        with patch.object(fb._cond, "wait", side_effect=wait):
            worker = threading.Thread(target=lambda: results.append(fb.wait_next(-1, 2.0)))
            worker.start()
            self.assertTrue(entered.wait(1.0))
            self.assertTrue(worker.is_alive())
            fb.update(JPEG)
            worker.join(1.0)
            self.assertFalse(worker.is_alive())
        self.assertEqual(JPEG, results[0][0])
        self.assertEqual(JPEG, fb.wait_next(-1, 0.0)[0])

    def test_empty_timeout_returns_without_a_frame(self):
        self.assertEqual((None, 0, 0.0), proxy.FrameBuffer().wait_next(-1, 0.0))


class MjpegDisconnectTests(unittest.TestCase):
    def run_idle_disconnect(self, method):
        connection, client = socket.socketpair()
        self.addCleanup(connection.close)
        self.addCleanup(client.close)
        handler = object.__new__(proxy.MjpegHandler)
        handler.connection = connection
        handler.server = SimpleNamespace(camera_playback_delay=30.0, camera_playback_fps=0.1)
        ready = threading.Event()
        handler._send_mjpeg_headers = ready.set
        errors = []
        def run():
            try:
                getattr(handler, method)(proxy.FrameBuffer())
            except Exception as exc:
                errors.append(exc)
        worker = threading.Thread(target=run, daemon=True)
        worker.start()
        self.assertTrue(ready.wait(1.0))
        self.assertTrue(worker.is_alive())
        client.close()
        worker.join(2.0)
        self.assertFalse(worker.is_alive(), "closed idle MJPEG client leaked its handler")
        self.assertEqual([], errors)
        self.assertIsNone(connection.gettimeout(), "stream timeout must not leak to other handlers")

    def test_live_disconnect_before_first_frame(self):
        self.run_idle_disconnect("_stream_mjpeg_live")

    def test_buffered_disconnect_before_first_frame_at_low_fps(self):
        self.run_idle_disconnect("_stream_mjpeg_replay")

    def test_header_write_timeout_is_handled_and_socket_timeout_restored(self):
        for method in ("_stream_mjpeg_live", "_stream_mjpeg_replay"):
            with self.subTest(method=method):
                connection, client = socket.socketpair()
                with connection, client:
                    connection.settimeout(3.0)
                    handler = object.__new__(proxy.MjpegHandler)
                    handler.connection = connection
                    handler.server = SimpleNamespace(camera_playback_delay=30.0, camera_playback_fps=24)
                    with patch.object(handler, "_send_mjpeg_headers", side_effect=TimeoutError):
                        getattr(handler, method)(proxy.FrameBuffer())
                    self.assertEqual(3.0, connection.gettimeout())


if __name__ == "__main__":
    unittest.main()
