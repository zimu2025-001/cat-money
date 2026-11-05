"""Compile the real firmware packet encoder and feed it to the Python receiver.

This is a host protocol test, not an ESP-IDF build or a hardware FPS benchmark.
Run from the repository root: python -m unittest discover -s tests -v
"""

import ctypes
import importlib.util
import random
import shutil
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("camera_proxy_protocol_test", ROOT / "esp32-proxy.py")
proxy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(proxy)


class FirmwareProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            raise unittest.SkipTest("a C compiler is required for firmware encoder tests")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / "cam_udp_protocol.so"
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Wconversion", "-Werror",
            "-pedantic", "-fPIC", "-shared", "-O2",
            str(ROOT / "esp32s3-cam/main/cam_udp_protocol.c"), "-o", str(library),
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
            self.assertEqual(proxy.UDP_FRAME_HEADER_V2.unpack_from(packet),
                             (b"ZMC2", frame_id, len(frame), zlib.crc32(frame), index, count, size - 22))
            packets.append(packet)
        return packets

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

    def test_encode_reassemble_boundaries(self):
        for length in (4, 1377, 1378, 1379, 2756, 30720, 1024 * 1024):
            with self.subTest(length=length):
                frame = self.jpeg(length)
                assembler = proxy.FramedUdpJpegAssembler(1024 * 1024)
                result = []
                for packet in self.packets(frame):
                    result.extend(assembler.feed(packet))
                self.assertEqual(result, [frame])

    def test_reordered_chunks_and_duplicates(self):
        frame = self.jpeg(30720)
        packets = self.packets(frame)
        random.Random(7).shuffle(packets)
        assembler = proxy.FramedUdpJpegAssembler(1024 * 1024)
        result = []
        for packet in packets:
            result.extend(assembler.feed(packet))
            result.extend(assembler.feed(packet))
        self.assertEqual(result, [frame])

    def test_missing_chunk_cannot_publish_partial_frame(self):
        frame = self.jpeg(10000)
        packets = self.packets(frame)
        assembler = proxy.FramedUdpJpegAssembler(1024 * 1024)
        for index, packet in enumerate(packets):
            if index != 2:
                self.assertEqual(assembler.feed(packet), [])

    def test_payload_corruption_rejected_by_crc(self):
        packets = self.packets(self.jpeg(10000))
        corrupted = bytearray(packets[2])
        corrupted[-10] ^= 1
        packets[2] = bytes(corrupted)
        assembler = proxy.FramedUdpJpegAssembler(1024 * 1024)
        for packet in packets:
            self.assertEqual(assembler.feed(packet), [])

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


if __name__ == "__main__":
    unittest.main()
