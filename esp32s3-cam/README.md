# ESP32-S3 Camera UDP Stream

An ESP-IDF camera node that sends OV2640 JPEG frames to `../esp32-proxy.py`
using the ZMC2 UDP protocol. The default image settings remain VGA (640 × 480),
JPEG quality 12, two PSRAM frame buffers, and `CAMERA_GRAB_LATEST`.

## Build and flash

Use ESP-IDF 5.1 or newer with the ESP32-S3 toolchain. The managed camera component
is pinned to `espressif/esp32-camera` 2.1.6. Check your board's camera pins and
Octal PSRAM compatibility against `main/main.c` and `sdkconfig.defaults` first.

1. Copy `main/camera_private_config.example.h` to `main/camera_private_config.h`.
2. Fill in Wi-Fi credentials and the proxy computer's LAN IPv4 address. The
   private file is ignored by Git. Do not commit credentials.
3. From an activated ESP-IDF terminal:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the actual camera serial port. Start the proxy before testing.
If an old build targets a different chip, run `idf.py fullclean` before selecting
`esp32s3`; do not erase the device's Flash just to change build targets.

## Frame pacing and congestion

Optional settings in the private header:

```c
#define ZHIMU_CAM_TARGET_FPS 15
#define ZHIMU_CAM_SEND_BUDGET_MS 100
```

- Target FPS is a pacing limit (1–60), not measured or promised hardware FPS.
  The sender waits once per frame, rounded to RTOS ticks, and never catches up
  by bursting several old frames. Slow frames still yield at least one tick.
- The old unconditional 20 ms delay after every packet is removed. Packets send
  consecutively; temporary transmit-buffer pressure (`ENOMEM`, `ENOBUFS`,
  `EAGAIN`/`EWOULDBLOCK`, `EINTR`) yields one tick before retrying only that packet.
- The socket is nonblocking. CRC calculation and packet sending share a nominal
  100 ms whole-frame budget (configurable 1–1000 ms). Expiry, Wi-Fi loss or a
  non-retryable send error abandons the frame. Deadline checks happen before each
  send/retry; scheduler delays can make the observed duration exceed the budget.
- Every captured buffer is returned before the pacing wait. No frame-level
  retransmit or growing backlog is added. UDP has no delivery acknowledgement:
  a locally successful send can still be lost on the network.
- Lower the target to 10 or 5 if sustained send drops/retries occur. Improve
  Wi-Fi/supply quality before increasing rates; keep image settings constant when
  comparing throughput.

## Wire format

Each datagram is at most **1400 bytes**, including a **22-byte header**, leaving
up to **1378 JPEG bytes** per packet. Integers are serialized in network byte order
without C struct padding, matching Python `struct.Struct("!4sIIIHHH")`:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `ZMC2` |
| 4 | 4 | Frame ID, unsigned 32-bit, wraps naturally |
| 8 | 4 | Complete JPEG length |
| 12 | 4 | CRC-32/ISO-HDLC of the complete JPEG, matching `zlib.crc32` |
| 16 | 2 | Zero-based chunk index |
| 18 | 2 | Total chunk count |
| 20 | 2 | This packet's payload length |
| 22 | variable | JPEG bytes |

The sender rejects empty, incomplete or larger-than-1-MiB frames. `main/cam_udp_protocol.c`
is independent of ESP-IDF so the exact firmware encoder can be tested on a host.
The proxy continues accepting legacy raw JPEG chunks and ZMC1, but only ZMC2 has
whole-frame CRC validation. Lost/corrupt ZMC2 frames are dropped instead of being
published as damaged images. Reordering within a complete frame is supported.

The updated proxy isolates ZMC1/ZMC2 assembly by sender IP/port, bounds pending
frames and bytes, and suppresses older/duplicate completed frame IDs, including
across uint32 wrap. Because ZMC2 has no boot/session ID, it forgets the last ID
when no newer frame completes for 5 seconds. A reboot reusing the same sender
port can therefore take up to 5 seconds to resume; very delayed replay after this
window cannot be distinguished from a new boot. This is not an authenticated
protocol. Use a trusted LAN and do not expose the UDP/HTTP ports publicly.

## Measuring results

Every roughly 5 seconds, the serial log prints:

- `sent_fps`: complete frames queued locally per second during that window.
- `sent`, `dropped`, `capture_errors`, `invalid`: window counts; capture failures
  are separate from captured frames that were dropped.
- `avg_jpeg`: mean JPEG bytes for successful local sends.
- `capture`, `send`, `max_send`: milliseconds spent waiting for capture and in
  CRC/packet transmission (including failed sends), excluding frame pacing.
- `retries`, `last_errno`: transient queue pressure and the most recent socket
  error in that window; a nonzero error does not imply every frame failed.

Compare `sent_fps` with `camera.source_fps` in `/status.json` and the actual live
view. Receiver source FPS counts accepted frames, not monitor refreshes. It uses
its retained frame history, so allow the history window to settle. Test a fixed
scene and a moving scene for at least a minute, and repeat with real Wi-Fi load.

A 30-KiB JPEG previously incurred 22 × 20 ms = 440 ms of packet sleeps alone.
Removing that artificial delay is a code-level improvement, not evidence of a
particular hardware frame rate. Exposure, capture, JPEG size, RTOS scheduling,
Wi-Fi loss and browser decoding still determine observed performance.

## Host regression tests

From the repository root (Python 3.10+; C compiler for the firmware protocol tests):

```sh
python -m unittest discover -s tests -v
```

These tests compile the real packet encoder with strict host compiler warnings
and check CRC, byte order, packet boundaries and receiver compatibility. Other
proxy tests cover malformed, lost, reordered, duplicate and late packets. Host
tests are **not** an ESP-IDF build, flash test or hardware FPS benchmark; run the
build and hardware checks above before relying on the firmware.

## Layout

- `main/main.c`: camera, Wi-Fi, bounded UDP sending, pacing and statistics.
- `main/cam_udp_protocol.{c,h}`: portable ZMC2 encoding and CRC.
- `main/CMakeLists.txt`: component sources and dependencies.
- `main/idf_component.yml`: managed component dependencies.
- `sdkconfig.defaults`: ESP32-S3 PSRAM defaults.
