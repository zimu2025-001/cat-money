# Camera protocol interoperability fixtures

`cam_udp_protocol.c` and `cam_udp_protocol.h` are verbatim copies of the camera
firmware encoder from this repository's `dev/cc-fps` snapshot:

- Source commit: `4423c5c216bee9cb4d626a1508c23686f772fdb6`.
- Original paths: `esp32s3-cam/main/cam_udp_protocol.c` and
  `esp32s3-cam/main/cam_udp_protocol.h`.

The source files, including their comments and headers, are unchanged. Neither
source file included an SPDX identifier or a license header at that revision;
this copy does not add or imply a new license grant. The header's historical
reference to the Python receiver is retained intentionally for an exact copy.

These files are test inputs, not a second camera firmware project. The Go server
does not build or link them. `tests/test_camera_udp_protocol.py` compiles the C
encoder temporarily, generates its actual ZMC2 packets, sends them to the Go
server, and verifies the resulting frames through HTTP. No checkout of the
camera branch or ESP-IDF installation is needed.

When intentionally changing the wire protocol, update both fixtures together
from the camera branch, record the new source commit here, and rerun:

```sh
python -m unittest discover -s tests -p test_camera_udp_protocol.py -v
```
