#!/usr/bin/env python3
"""
ESP32 camera HTTP frame to HTTP MJPEG bridge, with WebSocket lucky-cat servo
control. XiaoZhi can connect back to /device over WiFi; UART remains as a
debug fallback.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import threading
import time
import zlib
from collections import deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Optional


BOUNDARY = "frame"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
UDP_FRAME_MAGIC_V1 = b"ZMC1"
UDP_FRAME_MAGIC_V2 = b"ZMC2"
UDP_FRAME_MAGICS = (UDP_FRAME_MAGIC_V1, UDP_FRAME_MAGIC_V2)
UDP_FRAME_HEADER_V1 = struct.Struct("!4sIIHHH")
UDP_FRAME_HEADER_V2 = struct.Struct("!4sIIIHHH")
UDP_FRAME_STALE_SECONDS = 5.0
UDP_FRAME_MAX_PENDING = 8
UDP_FRAME_MAX_CHUNKS = 4096
VALID_COMMANDS = {
    "up",
    "down",
    "left",
    "right",
    "stop",
    "center",
    "status",
    "swing_on",
    "swing_off",
    "swing_toggle",
}


class FrameBuffer:
    def __init__(self, buffer_seconds: float = 20.0) -> None:
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._buffer_seconds = max(1.0, float(buffer_seconds))
        self._frames: deque[dict[str, Any]] = deque()
        self._jpeg: Optional[bytes] = None
        self._seq = 0
        self._last_ts = 0.0
        self._source = "none"
        self._camera_id: Optional[str] = None
        self._message = "waiting for frame"

    def update(self, jpeg: bytes, *, source: str = "udp", camera_id: Optional[str] = None, message: str = "frame received") -> None:
        if not jpeg:
            return
        with self._cond:
            self._jpeg = jpeg
            self._seq += 1
            self._last_ts = time.time()
            self._source = source
            self._camera_id = camera_id
            self._message = message
            self._frames.append(
                {
                    "jpeg": jpeg,
                    "seq": self._seq,
                    "ts": self._last_ts,
                    "source": source,
                    "camera_id": camera_id,
                    "message": message,
                }
            )
            self._prune_locked(self._last_ts)
            self._cond.notify_all()

    def get_latest(self) -> tuple[Optional[bytes], int, float]:
        with self._lock:
            return self._jpeg, self._seq, self._last_ts

    def status(self, offline_after: float) -> dict[str, Any]:
        with self._lock:
            age = None if self._last_ts == 0 else time.time() - self._last_ts
            buffered_age = 0.0
            source_fps = 0.0
            if len(self._frames) >= 2:
                buffered_age = float(self._frames[-1]["ts"]) - float(self._frames[0]["ts"])
                if buffered_age > 0:
                    source_fps = (len(self._frames) - 1) / buffered_age
            return {
                "online": age is not None and age <= offline_after,
                "source": self._source,
                "camera_id": self._camera_id,
                "seq": self._seq,
                "last_frame_age": age,
                "buffered_frames": len(self._frames),
                "buffered_seconds": buffered_age,
                "source_fps": source_fps,
                "message": self._message if age is not None else "waiting for frame",
            }

    def wait_next(self, last_seq: int, timeout: float) -> tuple[Optional[bytes], int, float]:
        with self._cond:
            if self._seq == last_seq:
                self._cond.wait(timeout=timeout)
            return self._jpeg, self._seq, self._last_ts

    def get_replay_frame(self, last_seq: int, latest_allowed_ts: float) -> tuple[Optional[bytes], int, float]:
        with self._lock:
            if last_seq < 0:
                for frame in reversed(self._frames):
                    if float(frame["ts"]) <= latest_allowed_ts:
                        return frame["jpeg"], int(frame["seq"]), float(frame["ts"])
                return None, last_seq, 0.0

            for frame in self._frames:
                seq = int(frame["seq"])
                if seq <= last_seq:
                    continue
                if float(frame["ts"]) > latest_allowed_ts:
                    break
                return frame["jpeg"], seq, float(frame["ts"])
            return None, last_seq, 0.0

    def _prune_locked(self, now: float) -> None:
        cutoff = now - self._buffer_seconds
        while len(self._frames) > 1 and float(self._frames[0]["ts"]) < cutoff:
            self._frames.popleft()


def _is_complete_jpeg(buf: bytes) -> bool:
    return len(buf) >= 4 and buf[0:2] == b"\xff\xd8" and buf[-2:] == b"\xff\xd9"


class JpegExtractor:
    """Extract complete JPEG frames from chunked UDP payloads."""

    def __init__(self, max_buffer_bytes: int = 4 * 1024 * 1024) -> None:
        self._buf = bytearray()
        self._max = max_buffer_bytes

    def feed(self, chunk: bytes) -> list[bytes]:
        if not chunk:
            return []

        self._buf.extend(chunk)
        if len(self._buf) > self._max:
            self._buf = self._buf[-(self._max // 2) :]

        out: list[bytes] = []
        while True:
            soi = self._buf.find(b"\xff\xd8")
            if soi < 0:
                if len(self._buf) > 2:
                    self._buf = self._buf[-2:]
                break

            if soi > 0:
                del self._buf[:soi]

            eoi = self._buf.find(b"\xff\xd9", 2)
            if eoi < 0:
                break

            frame = bytes(self._buf[: eoi + 2])
            del self._buf[: eoi + 2]
            if _is_complete_jpeg(frame):
                out.append(frame)
        return out


class FramedUdpJpegAssembler:
    """Reassemble numbered UDP JPEG chunks sent by the ESP32 camera node."""

    def __init__(self, max_frame_bytes: int, stale_seconds: float = UDP_FRAME_STALE_SECONDS) -> None:
        self._max_frame_bytes = max_frame_bytes
        self._stale_seconds = stale_seconds
        self._frames: dict[int, dict[str, Any]] = {}

    def feed(self, packet: bytes) -> list[bytes]:
        if packet.startswith(UDP_FRAME_MAGIC_V2):
            header = UDP_FRAME_HEADER_V2
        elif packet.startswith(UDP_FRAME_MAGIC_V1):
            header = UDP_FRAME_HEADER_V1
        else:
            return []

        if len(packet) < header.size:
            return []

        if header is UDP_FRAME_HEADER_V2:
            _magic, frame_id, frame_len, frame_crc, chunk_index, chunk_count, payload_len = header.unpack_from(packet)
        else:
            _magic, frame_id, frame_len, chunk_index, chunk_count, payload_len = header.unpack_from(packet)
            frame_crc = None

        payload = packet[header.size :]
        if (
            payload_len != len(payload)
            or payload_len == 0
            or frame_len == 0
            or frame_len > self._max_frame_bytes
            or chunk_count == 0
            or chunk_count > UDP_FRAME_MAX_CHUNKS
            or chunk_index >= chunk_count
        ):
            return []

        now = time.time()
        self._prune(now)

        state = self._frames.get(frame_id)
        if (
            state is None
            or state["frame_len"] != frame_len
            or state.get("frame_crc") != frame_crc
            or len(state["chunks"]) != chunk_count
        ):
            state = {
                "ts": now,
                "frame_len": frame_len,
                "frame_crc": frame_crc,
                "chunks": [None] * chunk_count,
                "received": 0,
                "received_bytes": 0,
            }
            self._frames[frame_id] = state
            self._trim_pending()

        chunks = state["chunks"]
        if chunks[chunk_index] is None:
            chunks[chunk_index] = payload
            state["received"] += 1
            state["received_bytes"] += payload_len
            state["ts"] = now

        if state["received"] != chunk_count:
            return []

        frame = b"".join(chunks)
        self._frames.pop(frame_id, None)
        if len(frame) != frame_len or not _is_complete_jpeg(frame):
            return []
        if frame_crc is not None and zlib.crc32(frame) & 0xFFFFFFFF != frame_crc:
            return []

        return [frame]

    def _prune(self, now: float) -> None:
        stale = [
            frame_id
            for frame_id, state in self._frames.items()
            if now - float(state["ts"]) > self._stale_seconds
        ]
        for frame_id in stale:
            self._frames.pop(frame_id, None)

    def _trim_pending(self) -> None:
        while len(self._frames) > UDP_FRAME_MAX_PENDING:
            oldest_id = min(self._frames, key=lambda frame_id: float(self._frames[frame_id]["ts"]))
            self._frames.pop(oldest_id, None)


class SerialControlBridge:
    """Line-delimited JSON bridge to the XiaoZhi pan/tilt UART controller."""

    def __init__(self, port: Optional[str], baud: int, timeout: float) -> None:
        self._port = port
        self._baud = baud
        self._timeout = timeout
        self._serial: Any = None
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._seq = 0
        self._pending: dict[int, dict[str, Any]] = {}
        self._last_status: dict[str, Any] = {
            "online": False,
            "message": "serial disabled",
            "pan": None,
            "tilt": None,
        }

    def start(self) -> None:
        if not self._port:
            return
        try:
            import serial  # type: ignore
        except ImportError:
            self._last_status = {
                "online": False,
                "message": "pyserial is not installed",
                "pan": None,
                "tilt": None,
            }
            return

        try:
            self._serial = serial.Serial(self._port, self._baud, timeout=0.2)
        except Exception as exc:
            self._last_status = {
                "online": False,
                "message": f"serial open failed: {exc}",
                "pan": None,
                "tilt": None,
            }
            return

        self._last_status = {
            "online": True,
            "message": f"serial connected: {self._port}",
            "pan": None,
            "tilt": None,
        }
        threading.Thread(target=self._reader_loop, daemon=True).start()

    @property
    def status(self) -> dict[str, Any]:
        with self._lock:
            return dict(self._last_status)

    @property
    def available(self) -> bool:
        return self._serial is not None

    def send_command(self, cmd: str, payload: Optional[dict[str, Any]] = None) -> dict[str, Any]:
        if cmd not in VALID_COMMANDS:
            return {"ok": False, "error": f"invalid command: {cmd}"}
        if self._serial is None:
            return {"ok": False, "error": self.status["message"], "status": self.status}

        frame = self._make_frame(cmd, payload or {})
        line = (json.dumps(frame, separators=(",", ":")) + "\n").encode("utf-8")
        with self._cond:
            self._pending[frame["seq"]] = {}
            try:
                self._serial.write(line)
                self._serial.flush()
            except Exception as exc:
                self._pending.pop(frame["seq"], None)
                self._last_status.update({"online": False, "message": f"serial write failed: {exc}"})
                return {"ok": False, "error": str(exc), "status": self.status}

            deadline = time.time() + self._timeout
            while time.time() < deadline:
                ack = self._pending.get(frame["seq"])
                if ack:
                    self._pending.pop(frame["seq"], None)
                    return ack
                self._cond.wait(timeout=max(0.01, deadline - time.time()))

            self._pending.pop(frame["seq"], None)
            return {"ok": False, "error": "serial ack timeout", "seq": frame["seq"], "status": self.status}

    def _make_frame(self, cmd: str, payload: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            self._seq = (self._seq + 1) & 0x7FFFFFFF
            seq = self._seq
        return {"type": "cmd", "seq": seq, "cmd": cmd, "payload": payload}

    def _reader_loop(self) -> None:
        assert self._serial is not None
        while True:
            try:
                raw = self._serial.readline()
            except Exception as exc:
                with self._cond:
                    self._last_status.update({"online": False, "message": f"serial read failed: {exc}"})
                    self._cond.notify_all()
                return
            if not raw:
                continue
            try:
                message = json.loads(raw.decode("utf-8", errors="replace").strip())
            except json.JSONDecodeError:
                continue
            self._handle_serial_message(message)

    def _handle_serial_message(self, message: dict[str, Any]) -> None:
        payload = message.get("payload")
        if not isinstance(payload, dict):
            payload = {}
        with self._cond:
            if message.get("type") in ("ack", "status"):
                self._last_status.update(
                    {
                        "online": True,
                        "message": "ok",
                        "pan": payload.get("pan", self._last_status.get("pan")),
                        "tilt": payload.get("tilt", self._last_status.get("tilt")),
                    }
                )
            seq = message.get("seq")
            if isinstance(seq, int) and seq in self._pending:
                self._pending[seq] = {
                    "ok": bool(message.get("ok", True)),
                    "seq": seq,
                    "type": message.get("type", "ack"),
                    "payload": payload,
                    "error": message.get("error"),
                }
            self._cond.notify_all()


def udp_receiver(
    fb: FrameBuffer,
    host: str,
    port: int,
    *,
    packet_is_frame: bool,
    udp_buf_bytes: int,
    extractor_max_bytes: int,
) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))

    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, udp_buf_bytes)
    except OSError:
        pass

    extractor = JpegExtractor(max_buffer_bytes=extractor_max_bytes)
    assembler = FramedUdpJpegAssembler(max_frame_bytes=extractor_max_bytes)

    while True:
        data, _addr = sock.recvfrom(65535)
        if not data:
            continue

        if data.startswith(UDP_FRAME_MAGICS):
            for frame in assembler.feed(data):
                fb.update(frame)
            continue

        if packet_is_frame:
            if _is_complete_jpeg(data):
                fb.update(data)
            else:
                for frame in extractor.feed(data):
                    fb.update(frame)
            continue

        for frame in extractor.feed(data):
            fb.update(frame)


def _ws_accept_key(key: str) -> str:
    digest = hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def _read_ws_frame(rfile) -> tuple[int, bytes]:
    header = rfile.read(2)
    if len(header) < 2:
        raise ConnectionError("short websocket header")
    b1, b2 = header
    opcode = b1 & 0x0F
    masked = bool(b2 & 0x80)
    length = b2 & 0x7F
    if length == 126:
        length = struct.unpack("!H", rfile.read(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", rfile.read(8))[0]
    mask = rfile.read(4) if masked else b""
    payload = bytearray(rfile.read(length))
    if masked:
        for i in range(length):
            payload[i] ^= mask[i % 4]
    return opcode, bytes(payload)


def _ws_frame(payload: bytes, opcode: int = 1) -> bytes:
    first = 0x80 | (opcode & 0x0F)
    length = len(payload)
    if length < 126:
        return bytes([first, length]) + payload
    if length <= 0xFFFF:
        return bytes([first, 126]) + struct.pack("!H", length) + payload
    return bytes([first, 127]) + struct.pack("!Q", length) + payload


class DeviceControlBridge:
    """Preferred WebSocket bridge to a WiFi-connected XiaoZhi device, with UART fallback."""

    def __init__(self, fallback: SerialControlBridge, timeout: float) -> None:
        self._fallback = fallback
        self._timeout = timeout
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._send_lock = threading.Lock()
        self._device_conn: Optional[socket.socket] = None
        self._seq = 0
        self._pending: dict[int, dict[str, Any]] = {}
        self._last_device_status: dict[str, Any] = {
            "online": False,
            "message": "device websocket disconnected",
            "device_id": None,
            "mg996r": None,
            "sg90": None,
            "swing": None,
        }

    @property
    def status(self) -> dict[str, Any]:
        with self._lock:
            device = dict(self._last_device_status)
        uart = self._fallback.status
        if device.get("online"):
            route = "device"
        elif uart.get("online"):
            route = "uart"
        else:
            route = "offline"
        return {"route": route, "device": device, "uart": uart}

    def attach_device(self, conn: socket.socket) -> None:
        old_conn: Optional[socket.socket]
        with self._cond:
            old_conn = self._device_conn if self._device_conn is not conn else None
            self._device_conn = conn
            self._last_device_status.update({"online": True, "message": "device websocket connected"})
            self._cond.notify_all()
        if old_conn is not None:
            self._close_socket(old_conn)

    def detach_device(self, conn: socket.socket) -> None:
        with self._cond:
            if self._device_conn is not conn:
                return
            self._device_conn = None
            self._last_device_status.update({"online": False, "message": "device websocket disconnected"})
            for seq in list(self._pending):
                self._pending[seq] = {"ok": False, "seq": seq, "error": "device websocket disconnected"}
            self._cond.notify_all()

    def handle_device_message(self, message: dict[str, Any]) -> None:
        msg_type = message.get("type")
        payload = message.get("payload")
        if not isinstance(payload, dict):
            payload = {}

        with self._cond:
            if msg_type == "hello":
                self._last_device_status.update(
                    {
                        "online": True,
                        "message": "device ready",
                        "device_id": message.get("device_id"),
                    }
                )
            elif msg_type in ("servo_ack", "servo_status", "status"):
                self._last_device_status.update(
                    {
                        "online": True,
                        "message": "ok" if message.get("ok", True) else str(message.get("error", "error")),
                        "mg996r": payload.get("mg996r", self._last_device_status.get("mg996r")),
                        "sg90": payload.get("sg90", self._last_device_status.get("sg90")),
                        "swing": payload.get("swing", self._last_device_status.get("swing")),
                    }
                )
                seq = message.get("seq")
                if isinstance(seq, int) and seq in self._pending:
                    self._pending[seq] = {
                        "ok": bool(message.get("ok", True)),
                        "seq": seq,
                        "type": msg_type,
                        "payload": payload,
                        "error": message.get("error"),
                        "route": "device",
                    }
            self._cond.notify_all()

    def send_command(self, cmd: str, payload: Optional[dict[str, Any]] = None) -> dict[str, Any]:
        if cmd not in VALID_COMMANDS:
            return {"ok": False, "error": f"invalid command: {cmd}"}
        with self._lock:
            conn = self._device_conn
        if conn is not None:
            return self._send_device_command(conn, cmd, payload or {})
        if self._fallback.available:
            result = self._fallback.send_command(cmd, payload)
            result.setdefault("route", "uart")
            return result
        return {"ok": False, "error": "device websocket disconnected", "route": "offline", "status": self.status}

    def _send_device_command(self, conn: socket.socket, cmd: str, payload: dict[str, Any]) -> dict[str, Any]:
        with self._cond:
            self._seq = (self._seq + 1) & 0x7FFFFFFF
            seq = self._seq
            self._pending[seq] = {}

        frame = {"type": "servo_cmd", "seq": seq, "cmd": cmd, "payload": payload}
        data = _ws_frame(json.dumps(frame, separators=(",", ":")).encode("utf-8"))
        try:
            with self._send_lock:
                conn.sendall(data)
        except OSError as exc:
            with self._cond:
                self._pending.pop(seq, None)
                if self._device_conn is conn:
                    self._device_conn = None
                    self._last_device_status.update({"online": False, "message": f"device write failed: {exc}"})
                self._cond.notify_all()
            fallback = self._fallback.send_command(cmd, payload)
            fallback.setdefault("route", "uart")
            return fallback

        deadline = time.time() + self._timeout
        with self._cond:
            while time.time() < deadline:
                ack = self._pending.get(seq)
                if ack:
                    self._pending.pop(seq, None)
                    return ack
                self._cond.wait(timeout=max(0.01, deadline - time.time()))
            self._pending.pop(seq, None)

        return {"ok": False, "error": "device ack timeout", "seq": seq, "route": "device", "status": self.status}

    @staticmethod
    def _close_socket(conn: socket.socket) -> None:
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            conn.close()
        except OSError:
            pass


class MjpegHandler(BaseHTTPRequestHandler):
    server_version = "ESP32MJPEG/2.0"
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        fb: FrameBuffer = self.server.frame_buffer  # type: ignore[attr-defined]

        if self.path.startswith("/ws"):
            self._handle_websocket()
            return
        if self.path.startswith("/device"):
            self._handle_device_websocket()
            return
        if self.path in ("/", "/index.html"):
            self._send_index()
            return
        if self.path == "/snapshot.jpg":
            self._send_snapshot(fb)
            return
        if self.path == "/status.json":
            self._send_json(self._status_payload())
            return
        if self.path.startswith("/mjpeg-buffered") or self.path.startswith("/mjpeg-replay"):
            self._stream_mjpeg_replay(fb)
            return
        if self.path.startswith("/mjpeg"):
            self._stream_mjpeg_live(fb)
            return

        self.send_error(HTTPStatus.NOT_FOUND, "Not Found")

    def do_POST(self) -> None:
        if self.path == "/camera/frame":
            self._handle_camera_frame_upload()
            return
        if self.path != "/control":
            self.send_error(HTTPStatus.NOT_FOUND, "Not Found")
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            message = json.loads(raw.decode("utf-8"))
            cmd, payload = _normalize_control_message(message, self.server.default_step)  # type: ignore[attr-defined]
            result = self.server.control_bridge.send_command(cmd, payload)  # type: ignore[attr-defined]
            self._send_json(result)
        except ValueError as exc:
            self._send_json({"ok": False, "error": str(exc)}, HTTPStatus.BAD_REQUEST)

    def log_message(self, fmt: str, *args) -> None:
        return

    def _handle_camera_frame_upload(self) -> None:
        fb: FrameBuffer = self.server.frame_buffer  # type: ignore[attr-defined]
        expected_token = self.server.camera_token  # type: ignore[attr-defined]
        max_frame_bytes = self.server.camera_max_frame_bytes  # type: ignore[attr-defined]

        token = self.headers.get("X-Camera-Token", "")
        if expected_token and token != expected_token:
            self._send_json({"ok": False, "error": "invalid camera token"}, HTTPStatus.UNAUTHORIZED)
            return

        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "image/jpeg":
            self._send_json({"ok": False, "error": "Content-Type must be image/jpeg"}, HTTPStatus.BAD_REQUEST)
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send_json({"ok": False, "error": "invalid Content-Length"}, HTTPStatus.BAD_REQUEST)
            return

        if length <= 0:
            self._send_json({"ok": False, "error": "empty frame"}, HTTPStatus.BAD_REQUEST)
            return
        if length > max_frame_bytes:
            self._send_json({"ok": False, "error": "frame too large"}, HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
            return

        frame = self.rfile.read(length)
        if len(frame) != length:
            self._send_json({"ok": False, "error": "short frame body"}, HTTPStatus.BAD_REQUEST)
            return
        if not _is_complete_jpeg(frame):
            self._send_json({"ok": False, "error": "invalid jpeg frame"}, HTTPStatus.BAD_REQUEST)
            return

        camera_id = self.headers.get("X-Camera-Id") or "esp32s3-cam"
        frame_seq = self.headers.get("X-Frame-Seq", "")
        fb.update(frame, source="esp32-http", camera_id=camera_id, message=f"http frame {frame_seq}".strip())
        _jpeg, seq, _ts = fb.get_latest()
        self._send_json({"ok": True, "seq": seq})

    def _send_index(self) -> None:
        host = self.headers.get("Host", "127.0.0.1")
        html = self._build_index_html(host)
        body = html.encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _build_index_html(self, host: str) -> str:
        safe_host = (
            host.replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
            .replace('"', "&quot;")
        )
        return """<!doctype html>
<html lang="zh-CN">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>喵掌柜 AIoT 看店中控</title>
    <style>
      :root {
        color-scheme: light;
        --ink: #172026;
        --muted: #64707d;
        --line: #d9dee5;
        --paper: #fbfaf7;
        --panel: #ffffff;
        --jade: #127c72;
        --gold: #c78522;
        --coral: #b64734;
        --steel: #30445a;
        --shadow: 0 18px 48px rgba(29, 42, 54, 0.12);
      }
      * { box-sizing: border-box; }
      body {
        margin: 0;
        background:
          linear-gradient(180deg, rgba(255,255,255,0.78), rgba(251,250,247,0.94)),
          repeating-linear-gradient(90deg, rgba(18,124,114,0.055) 0 1px, transparent 1px 44px),
          var(--paper);
        color: var(--ink);
        font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft YaHei", Arial, sans-serif;
      }
      .wrap {
        width: min(1440px, calc(100vw - 32px));
        margin: 0 auto;
        padding: 24px 0 32px;
        display: grid;
        gap: 16px;
      }
      .topbar {
        display: grid;
        grid-template-columns: 1fr auto;
        gap: 16px;
        align-items: end;
        padding: 10px 0 6px;
      }
      .brand { display: grid; gap: 8px; }
      .eyebrow {
        color: var(--gold);
        font-size: 13px;
        font-weight: 800;
        letter-spacing: 0;
      }
      h1 {
        margin: 0;
        font-size: clamp(30px, 5vw, 54px);
        line-height: 1;
        letter-spacing: 0;
      }
      .tagline {
        max-width: 780px;
        margin: 0;
        color: #3d4952;
        font-size: 15px;
        line-height: 1.7;
      }
      .status-stack {
        display: flex;
        flex-wrap: wrap;
        justify-content: flex-end;
        gap: 8px;
      }
      .chip {
        min-height: 34px;
        display: inline-flex;
        align-items: center;
        gap: 8px;
        border: 1px solid var(--line);
        border-radius: 999px;
        background: rgba(255,255,255,0.86);
        padding: 7px 12px;
        color: #36434d;
        font-size: 13px;
        font-weight: 700;
        box-shadow: 0 6px 18px rgba(23,32,38,0.06);
      }
      .dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #9aa4ad;
      }
      .chip.good .dot { background: #169b72; }
      .chip.warn .dot { background: var(--gold); }
      .chip.bad .dot { background: var(--coral); }
      .main-grid {
        display: grid;
        grid-template-columns: minmax(0, 1.9fr) minmax(320px, 0.75fr);
        gap: 16px;
      }
      .panel {
        background: rgba(255,255,255,0.92);
        border: 1px solid var(--line);
        border-radius: 8px;
        box-shadow: var(--shadow);
        overflow: hidden;
      }
      .panel-head {
        min-height: 58px;
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        border-bottom: 1px solid var(--line);
        padding: 14px 16px;
      }
      .panel-title {
        margin: 0;
        font-size: 17px;
        line-height: 1.25;
        letter-spacing: 0;
      }
      .panel-kicker {
        color: var(--muted);
        font-size: 12px;
        font-weight: 700;
      }
      .video-stage {
        position: relative;
        background: #0d1419;
        padding: 12px;
      }
      .video-stage img {
        width: 100%;
        min-height: 420px;
        aspect-ratio: 16 / 9;
        object-fit: contain;
        background: #0a0d10;
        border-radius: 6px;
        display: block;
      }
      .video-placeholder {
        position: absolute;
        inset: 12px;
        display: grid;
        place-items: center;
        border-radius: 6px;
        background: linear-gradient(180deg, rgba(13,20,25,0.88), rgba(13,20,25,0.72));
      }
      .video-placeholder.hidden {
        display: none;
      }
      .play-panel {
        display: grid;
        justify-items: center;
        gap: 10px;
        color: #e8f1f0;
        text-align: center;
      }
      .loading-spinner {
        width: 34px;
        height: 34px;
        border-radius: 999px;
        border: 3px solid rgba(232,241,240,0.25);
        border-top-color: #e8f1f0;
        display: none;
        animation: spin 0.9s linear infinite;
      }
      .video-placeholder.loading .loading-spinner {
        display: block;
      }
      .video-placeholder.loading .play-panel button {
        display: none;
      }
      .play-panel button {
        border: 0;
        border-radius: 8px;
        background: var(--jade);
        color: #fff;
        min-height: 44px;
        padding: 0 18px;
        font-size: 15px;
        font-weight: 800;
        cursor: pointer;
      }
      .play-panel p {
        margin: 0;
        color: rgba(232,241,240,0.76);
        font-size: 12px;
        line-height: 1.5;
      }
      @keyframes spin {
        to { transform: rotate(360deg); }
      }
      .control-body {
        display: grid;
        gap: 14px;
        padding: 16px;
      }
      .servo-readout {
        display: grid;
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 10px;
      }
      .metric {
        border: 1px solid var(--line);
        border-radius: 8px;
        background: #fbfcfb;
        padding: 12px;
      }
      .metric span {
        display: block;
        color: var(--muted);
        font-size: 12px;
        font-weight: 800;
      }
      .metric strong {
        display: block;
        margin-top: 5px;
        color: var(--ink);
        font-size: 22px;
        line-height: 1.1;
      }
      .dpad {
        display: grid;
        grid-template-columns: repeat(3, minmax(72px, 1fr));
        gap: 10px;
        align-items: center;
      }
      .dpad button {
        height: 58px;
        border: 1px solid #cbd4d8;
        border-radius: 8px;
        background: linear-gradient(180deg, #ffffff, #f1f5f4);
        color: var(--steel);
        font-size: 25px;
        font-weight: 900;
        cursor: pointer;
        transition: transform 0.12s ease, border-color 0.12s ease, background 0.12s ease;
      }
      .dpad button:hover {
        border-color: var(--jade);
        background: #edf7f4;
        transform: translateY(-1px);
      }
      .dpad button:active { transform: translateY(1px); }
      .dpad .center {
        background: #172026;
        color: #fff;
      }
      .swing-row {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 10px;
      }
      .command {
        height: 48px;
        border: 1px solid #c8d2d3;
        border-radius: 8px;
        background: #fff;
        color: var(--ink);
        font-weight: 800;
        cursor: pointer;
      }
      .command.primary {
        border-color: #a86519;
        background: #fff5df;
        color: #7d4611;
      }
      .status-line {
        min-height: 42px;
        display: flex;
        align-items: center;
        border: 1px solid var(--line);
        border-radius: 8px;
        background: #f7f8f5;
        padding: 10px 12px;
        color: #3f4a53;
        font-size: 13px;
        line-height: 1.45;
      }
      .device-info {
        display: grid;
        gap: 8px;
        border: 1px solid var(--line);
        border-radius: 8px;
        background: #fbfcfb;
        padding: 12px;
      }
      .device-info-title {
        color: var(--ink);
        font-size: 14px;
        font-weight: 900;
      }
      .info-row {
        display: grid;
        grid-template-columns: 74px 1fr;
        gap: 10px;
        align-items: center;
        min-height: 28px;
      }
      .info-row span {
        color: var(--muted);
        font-size: 12px;
        font-weight: 800;
      }
      .info-row strong {
        min-width: 0;
        color: #26323a;
        font-size: 13px;
        line-height: 1.45;
        font-weight: 800;
      }
      .feature-grid {
        display: grid;
        grid-template-columns: repeat(3, minmax(0, 1fr));
        gap: 12px;
      }
      .feature {
        border: 1px solid var(--line);
        border-radius: 8px;
        background: rgba(255,255,255,0.88);
        padding: 14px;
      }
      .feature b {
        display: block;
        color: var(--ink);
        font-size: 15px;
        margin-bottom: 7px;
      }
      .feature p {
        margin: 0;
        color: var(--muted);
        font-size: 13px;
        line-height: 1.6;
      }
      @media (max-width: 920px) {
        .topbar, .main-grid, .feature-grid { grid-template-columns: 1fr; }
        .status-stack { justify-content: flex-start; }
        .video-stage img { min-height: 260px; }
      }
      @media (max-width: 480px) {
        .wrap {
          width: min(100vw - 20px, 1440px);
          padding-top: 14px;
        }
        .servo-readout, .swing-row { grid-template-columns: 1fr; }
        .dpad { grid-template-columns: repeat(3, minmax(0, 1fr)); }
        .dpad button {
          height: 52px;
          font-size: 22px;
        }
      }
    </style>
  </head>
  <body>
    <main class="wrap">
      <header class="topbar">
        <section class="brand">
          <div class="eyebrow">AI 看店机器人</div>
          <h1>喵掌柜</h1>
          <p class="tagline">把招财猫、远程看店和智能迎宾收进一台小小的店铺助手里，替小店守住每一个需要被看见的瞬间。</p>
        </section>
        <aside class="status-stack">
          <div id="routeChip" class="chip bad"><i class="dot"></i><span id="routeText">设备离线</span></div>
          <div id="cameraChip" class="chip warn"><i class="dot"></i><span id="cameraText">画面等待</span></div>
        </aside>
      </header>

      <section class="main-grid">
        <section class="panel">
          <div class="panel-head">
            <div>
              <p class="panel-title">远程看店</p>
              <div class="panel-kicker">门店画面</div>
            </div>
            <div class="chip"><i class="dot"></i><span id="cameraAgeText">--</span></div>
          </div>
          <div class="video-stage">
            <img id="cameraStream" alt="喵掌柜远程看店画面" />
            <div id="cameraOverlay" class="video-placeholder">
              <div class="play-panel">
                <div class="loading-spinner" aria-hidden="true"></div>
                <button id="cameraPlayButton" type="button">开启播放</button>
                <p id="cameraLoadText">点击后查看当前店内画面。</p>
              </div>
            </div>
          </div>
        </section>

        <section class="panel">
          <div class="panel-head">
            <div>
              <p class="panel-title">招财猫遥控</p>
              <div class="panel-kicker">视角与右臂</div>
            </div>
            <div class="chip"><i class="dot"></i><span id="deviceIdText">--</span></div>
          </div>
          <div class="control-body">
            <div class="servo-readout">
              <div class="metric"><span>视角</span><strong id="mgText">--</strong></div>
              <div class="metric"><span>右臂</span><strong id="sgText">--</strong></div>
              <div class="metric"><span>招手</span><strong id="swingText">--</strong></div>
              <div class="metric"><span>连接</span><strong id="routeMetric">--</strong></div>
            </div>

            <div class="dpad">
              <span></span><button data-cmd="up" aria-label="上">↑</button><span></span>
              <button data-cmd="left" aria-label="左">←</button><button class="center" data-cmd="center" aria-label="居中">●</button><button data-cmd="right" aria-label="右">→</button>
              <span></span><button data-cmd="down" aria-label="下">↓</button><span></span>
            </div>

            <div class="swing-row">
              <button class="command primary" data-cmd="swing_toggle">摇臂切换</button>
              <button class="command" data-cmd="status">刷新状态</button>
            </div>
            <div id="status" class="status-line">正在连接喵掌柜...</div>
            <div class="device-info">
              <div class="device-info-title">设备信息</div>
              <div class="info-row"><span>身份</span><strong id="identityText">喵掌柜</strong></div>
              <div class="info-row"><span>看店</span><strong id="cameraInfoText">等待画面</strong></div>
              <div class="info-row"><span>能力</span><strong>迎宾互动 · 环境感知 · 招手招财</strong></div>
            </div>
          </div>
        </section>
      </section>

      <section class="feature-grid">
        <article class="feature"><b>会迎宾</b><p>顾客靠近时主动打招呼，让小店多一点温度和记忆点。</p></article>
        <article class="feature"><b>会守店</b><p>远程查看店内画面，随时确认环境与客流状态。</p></article>
        <article class="feature"><b>会招财</b><p>招财猫右臂摇摆互动，让传统招幌多一点智能趣味。</p></article>
      </section>
    </main>

    <script>
      const statusEl = document.getElementById("status");
      const routeChip = document.getElementById("routeChip");
      const cameraChip = document.getElementById("cameraChip");
      const routeText = document.getElementById("routeText");
      const cameraText = document.getElementById("cameraText");
      const cameraAgeText = document.getElementById("cameraAgeText");
      const cameraStream = document.getElementById("cameraStream");
      const cameraOverlay = document.getElementById("cameraOverlay");
      const cameraPlayButton = document.getElementById("cameraPlayButton");
      const cameraLoadText = document.getElementById("cameraLoadText");
      const deviceIdText = document.getElementById("deviceIdText");
      const identityText = document.getElementById("identityText");
      const cameraInfoText = document.getElementById("cameraInfoText");
      const mgText = document.getElementById("mgText");
      const sgText = document.getElementById("sgText");
      const swingText = document.getElementById("swingText");
      const routeMetric = document.getElementById("routeMetric");
      const ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
      let cameraPlayRequested = false;
      let cameraPlaying = false;
      let lastCameraStatus = null;

      function setChip(el, state) {
        el.classList.remove("good", "warn", "bad");
        el.classList.add(state);
      }

      function panLabel(value) {
        if (value === null || value === undefined) return "--";
        if (value < 75) return "偏左";
        if (value > 105) return "偏右";
        return "居中";
      }

      function armLabel(value) {
        if (value === null || value === undefined) return "--";
        if (value < 75) return "放下";
        if (value > 105) return "抬起";
        return "中位";
      }

      function showCameraLoading(text) {
        cameraOverlay.classList.remove("hidden");
        cameraOverlay.classList.add("loading");
        cameraPlayButton.disabled = true;
        cameraLoadText.textContent = text;
      }

      function startCameraPlayback() {
        if (cameraPlaying) return;
        cameraPlaying = true;
        cameraOverlay.classList.add("hidden");
        cameraOverlay.classList.remove("loading");
        cameraStream.src = `/mjpeg-buffered?start=${Date.now()}`;
        cameraText.textContent = "正在观看";
        cameraInfoText.textContent = "正在看店";
      }

      function renderStatus(data) {
        const camera = data?.camera || data?.payload?.camera || null;
        const control = data?.control || data?.payload?.control || data;
        const route = control?.route || "unknown";
        const device = control?.device || {};
        const uart = control?.uart || {};

        if (camera) {
          lastCameraStatus = camera;
          if (camera.online) {
            setChip(cameraChip, "good");
            cameraText.textContent = cameraPlaying ? "正在观看" : cameraPlayRequested ? "正在准备" : "画面在线";
            cameraAgeText.textContent = "在线";
            cameraInfoText.textContent = cameraPlaying ? "正在看店" : cameraPlayRequested ? "正在准备顺滑画面" : "可开启观看";
          } else {
            setChip(cameraChip, "warn");
            cameraText.textContent = camera.last_frame_age === null ? "画面等待" : "正在恢复";
            cameraAgeText.textContent = camera.last_frame_age === null ? "等待画面" : "恢复中";
            cameraInfoText.textContent = camera.last_frame_age === null ? "等待画面" : "正在恢复";
          }
          if (cameraPlayRequested && !cameraPlaying) {
            if (camera.playback_ready === true) {
              startCameraPlayback();
            } else {
              showCameraLoading(camera.online ? "正在准备顺滑画面..." : "等待店内画面上线...");
            }
          }
        }

        if (route === "device") {
          setChip(routeChip, "good");
          routeText.textContent = "设备在线";
          routeMetric.textContent = "在线";
          deviceIdText.textContent = "已连接";
          identityText.textContent = "喵掌柜在线";
          statusEl.textContent = "喵掌柜在线，可以远程看店和遥控招手。";
        } else if (route === "uart") {
          setChip(routeChip, "warn");
          routeText.textContent = "本地连接";
          routeMetric.textContent = "本地";
          deviceIdText.textContent = "本地";
          identityText.textContent = "本地调试";
          statusEl.textContent = "本地调试连接已接入。";
        } else {
          setChip(routeChip, "bad");
          routeText.textContent = "设备离线";
          routeMetric.textContent = "--";
          deviceIdText.textContent = "未连接";
          identityText.textContent = "等待上线";
          statusEl.textContent = "等待喵掌柜上线。";
        }

        mgText.textContent = panLabel(device.mg996r);
        sgText.textContent = armLabel(device.sg90);
        swingText.textContent = device.swing === true ? "开启" : device.swing === false ? "关闭" : "--";
      }

      async function refreshStatus() {
        try {
          const response = await fetch("/status.json", { cache: "no-store" });
          renderStatus(await response.json());
        } catch (_err) {
          setChip(routeChip, "bad");
          statusEl.textContent = "暂时无法获取设备状态。";
        }
      }

      function sendCommand(cmd) {
        if (ws.readyState !== WebSocket.OPEN) {
          statusEl.textContent = "设备暂未连接，稍后再试。";
          return;
        }
        statusEl.textContent = "正在执行...";
        ws.send(JSON.stringify({ type: "cmd", cmd }));
      }

      function renderCommandResult(data) {
        const payload = data.payload || {};
        if (payload.mg996r !== undefined) mgText.textContent = panLabel(payload.mg996r);
        if (payload.sg90 !== undefined) sgText.textContent = armLabel(payload.sg90);
        if (payload.swing !== undefined) {
          swingText.textContent = payload.swing === true ? "开启" : payload.swing === false ? "关闭" : "--";
        }
        if (data.route) routeMetric.textContent = data.route === "device" ? "在线" : data.route === "uart" ? "本地" : "--";
        statusEl.textContent = data.ok === false ? "指令失败，设备暂未响应。" : "指令已执行。";
        setTimeout(refreshStatus, 200);
      }

      ws.onopen = () => refreshStatus();
      ws.onclose = () => statusEl.textContent = "喵掌柜暂时离线。";
      ws.onerror = () => statusEl.textContent = "连接暂时异常。";
      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          if (data.payload?.control || data.control || data.camera) {
            renderStatus(data.payload || data);
          } else if (data.payload || data.route || data.ok !== undefined) {
            renderCommandResult(data);
          } else {
            statusEl.textContent = "收到设备更新。";
          }
        } catch (_err) {
          statusEl.textContent = "收到设备消息。";
        }
      };

      for (const button of document.querySelectorAll("button[data-cmd]")) {
        button.addEventListener("click", () => sendCommand(button.dataset.cmd));
      }
      cameraPlayButton.addEventListener("click", () => {
        cameraPlayRequested = true;
        cameraText.textContent = "正在打开";
        showCameraLoading("正在准备顺滑画面...");
        if (lastCameraStatus?.playback_ready === true) {
          startCameraPlayback();
        } else {
          refreshStatus();
        }
      });
      refreshStatus();
      setInterval(refreshStatus, 2000);
    </script>
  </body>
</html>
""".replace("__HOST__", safe_host)

    def _send_snapshot(self, fb: FrameBuffer) -> None:
        jpeg, _seq, _ts = fb.get_latest()
        if not jpeg:
            self.send_error(HTTPStatus.SERVICE_UNAVAILABLE, "No frame yet")
            return
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(jpeg)))
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.end_headers()
        self.wfile.write(jpeg)

    def _send_mjpeg_headers(self) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Age", "0")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Type", f"multipart/x-mixed-replace; boundary={BOUNDARY}")
        self.end_headers()

    def _write_mjpeg_frame(self, jpeg: bytes) -> None:
        headers = (
            f"--{BOUNDARY}\r\n"
            "Content-Type: image/jpeg\r\n"
            f"Content-Length: {len(jpeg)}\r\n"
            "\r\n"
        ).encode("utf-8")
        self.wfile.write(headers)
        self.wfile.write(jpeg)
        self.wfile.write(b"\r\n")
        self.wfile.flush()

    def _stream_mjpeg_live(self, fb: FrameBuffer) -> None:
        self._send_mjpeg_headers()

        last_seq = -1
        try:
            while True:
                jpeg, seq, _ts = fb.wait_next(last_seq, timeout=5.0)
                if not jpeg or seq == last_seq:
                    continue
                last_seq = seq
                self._write_mjpeg_frame(jpeg)
        except (ConnectionError, BrokenPipeError, ConnectionResetError):
            return

    def _stream_mjpeg_replay(self, fb: FrameBuffer) -> None:
        self._send_mjpeg_headers()

        replay_delay = self.server.camera_playback_delay  # type: ignore[attr-defined]
        replay_interval = 1.0 / max(0.1, float(self.server.camera_playback_fps))  # type: ignore[attr-defined]
        last_seq = -1
        last_jpeg: Optional[bytes] = None

        try:
            while True:
                allowed_ts = time.time() - float(replay_delay)
                jpeg, seq, _ts = fb.get_replay_frame(last_seq, allowed_ts)
                if jpeg is not None and seq != last_seq:
                    last_seq = seq
                    last_jpeg = jpeg
                    self._write_mjpeg_frame(jpeg)
                elif last_jpeg is not None:
                    self._write_mjpeg_frame(last_jpeg)

                time.sleep(replay_interval)
        except (ConnectionError, BrokenPipeError, ConnectionResetError):
            return

    def _handle_websocket(self) -> None:
        if self.headers.get("Upgrade", "").lower() != "websocket":
            print(f"[WS  ] bad browser upgrade from {self.client_address}: {self.headers.get('Upgrade')!r}")
            self.send_error(HTTPStatus.BAD_REQUEST, "Expected WebSocket upgrade")
            return
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            print(f"[WS  ] missing browser Sec-WebSocket-Key from {self.client_address}")
            self.send_error(HTTPStatus.BAD_REQUEST, "Missing Sec-WebSocket-Key")
            return

        print(f"[WS  ] browser connected from {self.client_address}")
        self.send_response(HTTPStatus.SWITCHING_PROTOCOLS)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", _ws_accept_key(key))
        self.end_headers()
        self.close_connection = True

        self._ws_send_json({"type": "status", "payload": self._status_payload()})
        while True:
            try:
                opcode, payload = _read_ws_frame(self.rfile)
            except ConnectionError:
                return
            if opcode == 0x8:
                try:
                    self.connection.sendall(_ws_frame(b"", opcode=0x8))
                finally:
                    return
            if opcode == 0x9:
                self.connection.sendall(_ws_frame(payload, opcode=0xA))
                continue
            if opcode != 0x1:
                self._ws_send_json({"ok": False, "error": "only text websocket frames are supported"})
                continue
            try:
                message = payload.decode("utf-8")
                parsed: Any = json.loads(message) if message.strip().startswith("{") else message.strip()
                cmd, cmd_payload = _normalize_control_message(parsed, self.server.default_step)  # type: ignore[attr-defined]
                result = self.server.control_bridge.send_command(cmd, cmd_payload)  # type: ignore[attr-defined]
                self._ws_send_json(result)
            except ValueError as exc:
                self._ws_send_json({"ok": False, "error": str(exc)})

    def _handle_device_websocket(self) -> None:
        if self.headers.get("Upgrade", "").lower() != "websocket":
            print(f"[DEV ] bad device upgrade from {self.client_address}: {self.headers.get('Upgrade')!r}")
            self.send_error(HTTPStatus.BAD_REQUEST, "Expected WebSocket upgrade")
            return
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            print(f"[DEV ] missing device Sec-WebSocket-Key from {self.client_address}")
            self.send_error(HTTPStatus.BAD_REQUEST, "Missing Sec-WebSocket-Key")
            return

        print(f"[DEV ] device websocket connected from {self.client_address}")
        self.send_response(HTTPStatus.SWITCHING_PROTOCOLS)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", _ws_accept_key(key))
        self.end_headers()
        self.close_connection = True

        self.server.control_bridge.attach_device(self.connection)  # type: ignore[attr-defined]
        try:
            while True:
                try:
                    opcode, payload = _read_ws_frame(self.rfile)
                except ConnectionError:
                    return
                if opcode == 0x8:
                    try:
                        self.connection.sendall(_ws_frame(b"", opcode=0x8))
                    finally:
                        return
                if opcode == 0x9:
                    self.connection.sendall(_ws_frame(payload, opcode=0xA))
                    continue
                if opcode != 0x1:
                    continue
                try:
                    message = json.loads(payload.decode("utf-8", errors="replace"))
                except json.JSONDecodeError:
                    continue
                if isinstance(message, dict):
                    self.server.control_bridge.handle_device_message(message)  # type: ignore[attr-defined]
        finally:
            self.server.control_bridge.detach_device(self.connection)  # type: ignore[attr-defined]

    def _ws_send_json(self, payload: dict[str, Any]) -> None:
        self.connection.sendall(_ws_frame(json.dumps(payload, separators=(",", ":")).encode("utf-8")))

    def _send_json(self, payload: dict[str, Any], status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _status_payload(self) -> dict[str, Any]:
        camera = self.server.frame_buffer.status(self.server.camera_offline_after)  # type: ignore[attr-defined]
        playback_delay = self.server.camera_playback_delay  # type: ignore[attr-defined]
        min_remaining = self.server.camera_min_playback_remaining  # type: ignore[attr-defined]
        last_age = camera.get("last_frame_age")
        remaining = 0.0
        if isinstance(last_age, (int, float)):
            remaining = max(0.0, float(playback_delay) - float(last_age))
        camera["playback_delay"] = playback_delay
        camera["playback_fps"] = self.server.camera_playback_fps  # type: ignore[attr-defined]
        camera["start_buffer_seconds"] = self.server.camera_start_buffer_seconds  # type: ignore[attr-defined]
        camera["start_buffer_frames"] = self.server.camera_start_buffer_frames  # type: ignore[attr-defined]
        camera["min_playback_remaining"] = min_remaining
        camera["playback_buffer_remaining"] = remaining
        camera["playback_ready"] = (
            isinstance(last_age, (int, float))
            and float(camera.get("buffered_seconds") or 0.0) >= float(camera["start_buffer_seconds"])
            and int(camera.get("buffered_frames") or 0) >= int(camera["start_buffer_frames"])
            and remaining >= float(min_remaining)
        )
        return {
            "camera": camera,
            "control": self.server.control_bridge.status,  # type: ignore[attr-defined]
        }


def _normalize_control_message(message: Any, default_step: int) -> tuple[str, dict[str, Any]]:
    if isinstance(message, str):
        cmd = message
        payload: dict[str, Any] = {}
    elif isinstance(message, dict):
        cmd_value = message.get("cmd") or message.get("command")
        if not isinstance(cmd_value, str):
            raise ValueError("missing command")
        cmd = cmd_value
        payload_value = message.get("payload", {})
        payload = payload_value if isinstance(payload_value, dict) else {}
        if "step" in message and "step" not in payload:
            payload["step"] = message["step"]
    else:
        raise ValueError("message must be a JSON object or command string")

    cmd = cmd.lower()
    if cmd not in VALID_COMMANDS:
        raise ValueError(f"invalid command: {cmd}")
    if cmd in {"up", "down", "left", "right"} and "step" not in payload:
        payload["step"] = default_step
    return cmd, payload


class MjpegServer(ThreadingHTTPServer):
    def __init__(
        self,
        server_address,
        request_handler,
        frame_buffer: FrameBuffer,
        control_bridge: DeviceControlBridge,
        default_step: int,
        camera_token: str,
        camera_max_frame_bytes: int,
        camera_offline_after: float,
        camera_playback_delay: float,
        camera_playback_fps: float,
        camera_start_buffer_seconds: float,
        camera_start_buffer_frames: int,
        camera_min_playback_remaining: float,
    ):
        super().__init__(server_address, request_handler)
        self.frame_buffer = frame_buffer
        self.control_bridge = control_bridge
        self.default_step = default_step
        self.camera_token = camera_token
        self.camera_max_frame_bytes = camera_max_frame_bytes
        self.camera_offline_after = camera_offline_after
        self.camera_playback_delay = camera_playback_delay
        self.camera_playback_fps = camera_playback_fps
        self.camera_start_buffer_seconds = camera_start_buffer_seconds
        self.camera_start_buffer_frames = camera_start_buffer_frames
        self.camera_min_playback_remaining = camera_min_playback_remaining


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="ESP32 camera HTTP frame bridge -> MJPEG + WebSocket lucky-cat servo control")
    p.add_argument("--udp-host", default="0.0.0.0", help="UDP bind host (default: 0.0.0.0)")
    p.add_argument("--udp-port", type=int, default=5000, help="UDP bind port (default: 5000)")
    p.add_argument("--http-host", default="0.0.0.0", help="HTTP/WebSocket bind host (default: 0.0.0.0)")
    p.add_argument("--http-port", type=int, default=8080, help="HTTP/WebSocket bind port (default: 8080)")
    p.add_argument("--serial-port", default=None, help="UART port connected to XiaoZhi controller, e.g. COM7 or /dev/ttyUSB0")
    p.add_argument("--serial-baud", type=int, default=115200, help="UART baud rate (default: 115200)")
    p.add_argument("--control-step", type=int, default=5, help="default servo step per arrow command (default: 5)")
    p.add_argument("--control-timeout", type=float, default=1.0, help="seconds to wait for UART ACK (default: 1.0)")
    p.add_argument("--packet-is-frame", action="store_true", help="treat each UDP packet as a complete JPEG when possible")
    p.add_argument("--udp-rcvbuf", type=int, default=8 * 1024 * 1024, help="UDP receive buffer bytes")
    p.add_argument("--extractor-max-bytes", type=int, default=8 * 1024 * 1024, help="JPEG extractor buffer limit")
    p.add_argument("--enable-esp32-cam-udp", action="store_true", help="deprecated: UDP camera receiver is enabled by default")
    p.add_argument("--disable-esp32-cam-udp", action="store_true", help="disable ESP32-CAM UDP receiver")
    p.add_argument(
        "--camera-token",
        default=os.getenv("ZHIMU_CAMERA_TOKEN", ""),
        help="token required by POST /camera/frame; defaults to ZHIMU_CAMERA_TOKEN, empty disables token checking",
    )
    p.add_argument("--camera-max-frame-bytes", type=int, default=1048576, help="maximum accepted JPEG frame bytes")
    p.add_argument("--camera-offline-after", type=float, default=10.0, help="seconds before camera status is treated as offline")
    p.add_argument("--camera-buffer-seconds", type=float, default=120.0, help="seconds of accepted camera frames kept for delayed playback")
    p.add_argument("--camera-playback-delay", type=float, default=30.0, help="seconds of delay used by /mjpeg buffered playback")
    p.add_argument("--camera-playback-fps", type=float, default=24.0, help="target FPS for /mjpeg buffered playback")
    p.add_argument("--camera-start-buffer-seconds", type=float, default=30.0, help="minimum buffered camera seconds before the web page starts playback")
    p.add_argument("--camera-start-buffer-frames", type=int, default=120, help="minimum buffered camera frames before the web page starts playback")
    p.add_argument("--camera-min-playback-remaining", type=float, default=10.0, help="minimum seconds between playback point and latest frame before pausing the web stream")
    return p.parse_args()


def main() -> None:
    args = parse_args()
    fb = FrameBuffer(float(args.camera_buffer_seconds))
    serial_bridge = SerialControlBridge(args.serial_port, int(args.serial_baud), float(args.control_timeout))
    serial_bridge.start()
    control_bridge = DeviceControlBridge(serial_bridge, float(args.control_timeout))

    if not args.disable_esp32_cam_udp:
        threading.Thread(
            target=udp_receiver,
            args=(fb, args.udp_host, args.udp_port),
            kwargs=dict(
                packet_is_frame=bool(args.packet_is_frame),
                udp_buf_bytes=int(args.udp_rcvbuf),
                extractor_max_bytes=int(args.extractor_max_bytes),
            ),
            daemon=True,
        ).start()

    httpd = MjpegServer(
        (args.http_host, args.http_port),
        MjpegHandler,
        fb,
        control_bridge,
        int(args.control_step),
        str(args.camera_token),
        int(args.camera_max_frame_bytes),
        float(args.camera_offline_after),
        float(args.camera_playback_delay),
        float(args.camera_playback_fps),
        float(args.camera_start_buffer_seconds),
        int(args.camera_start_buffer_frames),
        float(args.camera_min_playback_remaining),
    )
    host, port = httpd.socket.getsockname()[:2]

    print(f"[HTTP] http://{host}:{port}/  (Live MJPEG: /mjpeg, Buffered MJPEG: /mjpeg-buffered, Snapshot: /snapshot.jpg, Browser WS: /ws, Device WS: /device)")
    print(f"[CAM ] buffered playback delay={float(args.camera_playback_delay):.1f}s fps={float(args.camera_playback_fps):.1f} buffer={float(args.camera_buffer_seconds):.1f}s start_after={float(args.camera_start_buffer_seconds):.1f}s/{int(args.camera_start_buffer_frames)}f min_remaining={float(args.camera_min_playback_remaining):.1f}s")
    if not args.disable_esp32_cam_udp:
        print(f"[UDP ] listening on {args.udp_host}:{args.udp_port} (packet_is_frame={bool(args.packet_is_frame)})")
    else:
        print("[UDP ] ESP32-CAM UDP receiver disabled")
    print(f"[UART] {serial_bridge.status['message']}")
    print("Press Ctrl+C to stop.")

    try:
        httpd.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    try:
        os.environ["PYTHONUNBUFFERED"] = "1"
    except Exception:
        pass
    main()
