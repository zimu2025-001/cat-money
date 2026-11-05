package main

import (
	"flag"
	"fmt"
	"io"
	"math"
	"os"
)

// Config retains the Python server's command-line names and defaults.
type Config struct {
	UDPHost, HTTPHost, SerialPort, CameraToken              string
	UDPPort, HTTPPort, SerialBaud, ControlStep              int
	UDPRcvbuf, ExtractorMaxBytes, CameraMaxFrameBytes       int
	CameraStartBufferFrames                                 int
	ControlTimeout, CameraOfflineAfter, CameraBufferSeconds float64
	CameraPlaybackDelay, CameraPlaybackFPS                  float64
	CameraStartBufferSeconds, CameraMinPlaybackRemaining    float64
	PacketIsFrame, DisableUDP, EnableUDP                    bool
}

func DefaultConfig() Config {
	return Config{UDPHost: "0.0.0.0", HTTPHost: "0.0.0.0", UDPPort: 5000, HTTPPort: 8080,
		SerialBaud: 115200, ControlStep: 5, ControlTimeout: 1, UDPRcvbuf: 8 * 1024 * 1024,
		ExtractorMaxBytes: 8 * 1024 * 1024, CameraToken: os.Getenv("ZHIMU_CAMERA_TOKEN"),
		CameraMaxFrameBytes: 1048576, CameraOfflineAfter: 10, CameraBufferSeconds: 120,
		CameraPlaybackDelay: 30, CameraPlaybackFPS: 24, CameraStartBufferSeconds: 30,
		CameraStartBufferFrames: 120, CameraMinPlaybackRemaining: 10}
}

func ParseConfig(args []string, output io.Writer) (Config, error) {
	c := DefaultConfig()
	f := flag.NewFlagSet("esp32-proxy", flag.ContinueOnError)
	f.SetOutput(output)
	f.StringVar(&c.UDPHost, "udp-host", c.UDPHost, "UDP bind host")
	f.IntVar(&c.UDPPort, "udp-port", c.UDPPort, "UDP bind port")
	f.StringVar(&c.HTTPHost, "http-host", c.HTTPHost, "HTTP/WebSocket bind host")
	f.IntVar(&c.HTTPPort, "http-port", c.HTTPPort, "HTTP/WebSocket bind port")
	f.StringVar(&c.SerialPort, "serial-port", "", "UART device, for example COM7 or /dev/ttyUSB0")
	f.IntVar(&c.SerialBaud, "serial-baud", c.SerialBaud, "UART baud rate")
	f.IntVar(&c.ControlStep, "control-step", c.ControlStep, "default servo step per arrow command")
	f.Float64Var(&c.ControlTimeout, "control-timeout", c.ControlTimeout, "seconds to wait for device/UART ACK")
	f.BoolVar(&c.PacketIsFrame, "packet-is-frame", false, "treat each UDP packet as a JPEG when possible")
	f.IntVar(&c.UDPRcvbuf, "udp-rcvbuf", c.UDPRcvbuf, "UDP receive buffer bytes")
	f.IntVar(&c.ExtractorMaxBytes, "extractor-max-bytes", c.ExtractorMaxBytes, "JPEG extractor/frame byte limit")
	f.BoolVar(&c.EnableUDP, "enable-esp32-cam-udp", false, "deprecated: UDP is enabled by default")
	f.BoolVar(&c.DisableUDP, "disable-esp32-cam-udp", false, "disable UDP camera receiver")
	f.StringVar(&c.CameraToken, "camera-token", c.CameraToken, "POST /camera/frame token; defaults to ZHIMU_CAMERA_TOKEN")
	f.IntVar(&c.CameraMaxFrameBytes, "camera-max-frame-bytes", c.CameraMaxFrameBytes, "maximum HTTP JPEG frame bytes")
	f.Float64Var(&c.CameraOfflineAfter, "camera-offline-after", c.CameraOfflineAfter, "seconds before camera becomes offline")
	f.Float64Var(&c.CameraBufferSeconds, "camera-buffer-seconds", c.CameraBufferSeconds, "seconds of frames retained")
	f.Float64Var(&c.CameraPlaybackDelay, "camera-playback-delay", c.CameraPlaybackDelay, "delay for /mjpeg-buffered and /mjpeg-replay")
	f.Float64Var(&c.CameraPlaybackFPS, "camera-playback-fps", c.CameraPlaybackFPS, "target buffered-playback FPS")
	f.Float64Var(&c.CameraStartBufferSeconds, "camera-start-buffer-seconds", c.CameraStartBufferSeconds, "minimum seconds before buffered playback")
	f.IntVar(&c.CameraStartBufferFrames, "camera-start-buffer-frames", c.CameraStartBufferFrames, "minimum frames before buffered playback")
	f.Float64Var(&c.CameraMinPlaybackRemaining, "camera-min-playback-remaining", c.CameraMinPlaybackRemaining, "minimum buffered-playback reserve in seconds")
	if err := f.Parse(args); err != nil {
		return c, err
	}
	if f.NArg() != 0 {
		return c, fmt.Errorf("unexpected arguments: %v", f.Args())
	}
	if c.HTTPPort < 0 || c.HTTPPort > 65535 || c.UDPPort < 0 || c.UDPPort > 65535 {
		return c, fmt.Errorf("ports must be between 0 and 65535")
	}
	if c.SerialBaud <= 0 || c.UDPRcvbuf <= 0 || c.ExtractorMaxBytes < 4 || c.CameraMaxFrameBytes < 4 {
		return c, fmt.Errorf("baud and receive buffer must be positive; frame limits must be at least 4 bytes")
	}
	for name, v := range map[string]float64{"control-timeout": c.ControlTimeout, "camera-offline-after": c.CameraOfflineAfter, "camera-buffer-seconds": c.CameraBufferSeconds, "camera-playback-delay": c.CameraPlaybackDelay, "camera-playback-fps": c.CameraPlaybackFPS, "camera-start-buffer-seconds": c.CameraStartBufferSeconds, "camera-min-playback-remaining": c.CameraMinPlaybackRemaining} {
		if math.IsNaN(v) || math.IsInf(v, 0) || v < 0 || v > 365*24*3600 {
			return c, fmt.Errorf("%s must be finite, nonnegative and at most one year", name)
		}
	}
	if c.ControlTimeout == 0 {
		return c, fmt.Errorf("control-timeout must be positive")
	}
	if c.CameraStartBufferFrames < 0 {
		return c, fmt.Errorf("camera-start-buffer-frames must be nonnegative")
	}
	return c, nil
}
