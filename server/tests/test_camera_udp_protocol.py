"""Feed the firmware C encoder fixture's packets into the compiled Go server.

These are host protocol tests, not an ESP-IDF build or hardware FPS benchmark.
From server/: python -m unittest discover -s tests -v
Requires a C compiler and Go, or CAT_MONEY_SERVER_BINARY pointing to a built
server. No Python server implementation or third-party Python packages are used.
The verbatim firmware fixture and its provenance are in tests/fixtures/.
"""

import ctypes
import json
import os
import random
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
V2_HEADER = struct.Struct("!4sIIIHHH")


class FirmwareEncoderMixin:
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            raise unittest.SkipTest("a C compiler is required for firmware encoder tests")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / "cam_udp_protocol.so"
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Wconversion", "-Werror",
            "-pedantic", "-fPIC", "-shared", "-O2",
            str(ROOT / "tests/fixtures/cam_udp_protocol.c"), "-o", str(library),
        ], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.cam_udp_crc32.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.cam_udp_crc32.restype = ctypes.c_uint32
        cls.lib.cam_udp_chunk_count.argtypes = [ctypes.c_size_t]
        cls.lib.cam_udp_chunk_count.restype = ctypes.c_uint16
        cls.lib.cam_udp_encode_chunk.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint16,
        ]
        cls.lib.cam_udp_encode_chunk.restype = ctypes.c_size_t

    @staticmethod
    def jpeg(length):
        # Synthetic JPEG boundary bytes exercise transport, not image decoding.
        return b"\xff\xd8" + bytes((i % 251 for i in range(length - 4))) + b"\xff\xd9"

    def packets(self, frame, frame_id=0x1234ABCD):
        count = self.lib.cam_udp_chunk_count(len(frame))
        crc = self.lib.cam_udp_crc32(frame, len(frame))
        packets = []
        for index in range(count):
            output = ctypes.create_string_buffer(1404)
            ctypes.memset(output, 0xA5, len(output))
            size = self.lib.cam_udp_encode_chunk(output, 1400, frame, len(frame), frame_id, crc, index)
            self.assertGreater(size, 22)
            self.assertLessEqual(size, 1400)
            self.assertEqual(output.raw[1400:], b"\xa5" * 4)
            packet = output.raw[:size]
            self.assertEqual(V2_HEADER.unpack_from(packet),
                             (b"ZMC2", frame_id, len(frame), zlib.crc32(frame), index, count, size - 22))
            packets.append(packet)
        return packets


class FirmwareProtocolTests(FirmwareEncoderMixin, unittest.TestCase):
    def test_crc_known_vector_and_empty(self):
        self.assertEqual(self.lib.cam_udp_crc32(b"123456789", 9), 0xCBF43926)
        self.assertEqual(self.lib.cam_udp_crc32(b"", 0), 0)

    def test_crc_binary_random_and_max_frame(self):
        rng = random.Random(2026)
        for size in (1, 4, 255, 1378, 8192, 1024 * 1024):
            with self.subTest(size=size):
                data = bytes(rng.randrange(256) for _ in range(size))
                self.assertEqual(self.lib.cam_udp_crc32(data, size), zlib.crc32(data))

    def test_chunk_count_boundaries(self):
        for length, count in ((0, 0), (1, 1), (1378, 1), (1379, 2), (2756, 2),
                              (1024 * 1024, 761), (1024 * 1024 + 1, 0)):
            with self.subTest(length=length):
                self.assertEqual(self.lib.cam_udp_chunk_count(length), count)

    def test_invalid_arguments_leave_output_untouched(self):
        frame = self.jpeg(1379)
        output = ctypes.create_string_buffer(b"\xa5" * 1400, 1400)
        crc = zlib.crc32(frame)
        for length, index, capacity in ((len(frame), 0, 1399), (len(frame), 2, 1400),
                                         (0, 0, 1400), (1024 * 1024 + 1, 0, 1400)):
            self.assertEqual(self.lib.cam_udp_encode_chunk(output, capacity, frame, length, 1, crc, index), 0)
            self.assertEqual(output.raw, b"\xa5" * 1400)
        self.assertEqual(self.lib.cam_udp_encode_chunk(None, 1400, frame, len(frame), 1, crc, 0), 0)
        self.assertEqual(self.lib.cam_udp_encode_chunk(output, 1400, None, len(frame), 1, crc, 0), 0)


class FirmwareGoReceiverTests(FirmwareEncoderMixin, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        supplied = os.environ.get("CAT_MONEY_SERVER_BINARY")
        if supplied:
            cls.binary = str(Path(supplied).resolve())
            if not Path(cls.binary).is_file():
                raise RuntimeError(f"CAT_MONEY_SERVER_BINARY does not exist: {cls.binary}")
        else:
            go = shutil.which("go")
            if go is None:
                raise unittest.SkipTest("Go or CAT_MONEY_SERVER_BINARY is required for server integration tests")
            cls.binary = str(Path(cls.temp.name) / "cat-money-server")
            subprocess.run([go, "build", "-o", cls.binary, "."], cwd=ROOT, check=True, timeout=120)

    @staticmethod
    def free_port(kind):
        with socket.socket(socket.AF_INET, kind) as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def setUp(self):
        self.http_port = self.free_port(socket.SOCK_STREAM)
        self.udp_port = self.free_port(socket.SOCK_DGRAM)
        self.base = f"http://127.0.0.1:{self.http_port}"
        # Explicit no-proxy opener keeps local tests independent of user proxies.
        self.http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.log = tempfile.TemporaryFile()
        self.addCleanup(self.log.close)
        # Run outside the source tree so external web files cannot mask a broken embed.
        self.process = subprocess.Popen([
            self.binary, "--http-host", "127.0.0.1", "--http-port", str(self.http_port),
            "--udp-host", "127.0.0.1", "--udp-port", str(self.udp_port),
        ], cwd=self.temp.name, stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop_server)
        deadline = time.monotonic() + 5
        while True:
            if self.process.poll() is not None:
                self.log.seek(0)
                self.fail(f"Go server exited during startup: {self.log.read().decode(errors='replace')}")
            try:
                self.status()
                break
            except (OSError, urllib.error.URLError):
                if time.monotonic() >= deadline:
                    self.fail("Go server did not become ready within five seconds")
                time.sleep(0.02)
        self.sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(self.sender.close)
        # A stable source port is essential for sender-isolation/sequence tests.
        self.sender.bind(("127.0.0.1", 0))

    def stop_server(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)

    def status(self):
        with self.http.open(self.base + "/status.json", timeout=2) as response:
            return json.load(response)["camera"]

    def snapshot(self):
        with self.http.open(self.base + "/snapshot.jpg", timeout=2) as response:
            self.assertEqual(response.headers.get_content_type(), "image/jpeg")
            return response.read()

    def send_packets(self, packets):
        for index, packet in enumerate(packets):
            self.sender.sendto(packet, ("127.0.0.1", self.udp_port))
            # Keep this a protocol test rather than a kernel receive-buffer test.
            if index % 16 == 15:
                time.sleep(0.001)

    def wait_for_seq(self, seq):
        deadline = time.monotonic() + 3
        while True:
            status = self.status()
            if status["seq"] >= seq:
                self.assertEqual(status["seq"], seq)
                return status
            if time.monotonic() >= deadline:
                self.fail(f"expected camera sequence {seq}, got {status}")
            time.sleep(0.01)

    def test_embedded_viewer_without_external_web_directory(self):
        self.assertFalse((Path(self.temp.name) / "web").exists())
        expected = (ROOT / "web/index.html").read_text(encoding="utf-8")
        expected = expected.replace("__HOST__", f"127.0.0.1:{self.http_port}")
        for route in ("/", "/index.html"):
            with self.subTest(route=route):
                with self.http.open(self.base + route, timeout=2) as response:
                    self.assertEqual(response.headers.get_content_type(), "text/html")
                    self.assertEqual(response.read().decode("utf-8"), expected)

    def test_encode_reassemble_boundaries(self):
        for seq, length in enumerate((4, 1377, 1378, 1379, 2756, 30720, 1024 * 1024), 1):
            with self.subTest(length=length):
                frame = self.jpeg(length)
                self.send_packets(self.packets(frame, frame_id=seq))
                status = self.wait_for_seq(seq)
                self.assertTrue(status["online"])
                self.assertEqual(status["source"], "udp")
                self.assertEqual(self.snapshot(), frame)

    def test_reordered_chunks_and_duplicates(self):
        frame = self.jpeg(30720)
        packets = self.packets(frame)
        random.Random(7).shuffle(packets)
        self.send_packets([packet for packet in packets for _ in range(2)])
        self.wait_for_seq(1)
        self.assertEqual(self.snapshot(), frame)
        # Repeat a completed frame, then a newer sentinel. Only two publish.
        self.send_packets(packets)
        sentinel = self.jpeg(60)
        self.send_packets(self.packets(sentinel, frame_id=0x1234ABCE))
        self.wait_for_seq(2)
        self.assertEqual(self.snapshot(), sentinel)

    def test_missing_chunk_cannot_publish_partial_frame(self):
        frame = self.jpeg(10000)
        packets = self.packets(frame, frame_id=1)
        self.send_packets([packet for index, packet in enumerate(packets) if index != 2])
        sentinel = self.jpeg(60)
        self.send_packets(self.packets(sentinel, frame_id=2))
        self.wait_for_seq(1)
        self.assertEqual(self.snapshot(), sentinel)
        # Late completion of the old partial frame must not rewind playback.
        self.send_packets([packets[2]])
        self.send_packets(self.packets(self.jpeg(80), frame_id=3))
        self.wait_for_seq(2)
        self.assertEqual(self.snapshot(), self.jpeg(80))

    def test_payload_corruption_rejected_by_crc(self):
        packets = self.packets(self.jpeg(10000), frame_id=1)
        corrupted = bytearray(packets[2])
        corrupted[-10] ^= 1
        packets[2] = bytes(corrupted)
        self.send_packets(packets)
        sentinel = self.jpeg(60)
        self.send_packets(self.packets(sentinel, frame_id=2))
        self.wait_for_seq(1)
        self.assertEqual(self.snapshot(), sentinel)


if __name__ == "__main__":
    unittest.main()
