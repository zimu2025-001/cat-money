package main

import (
	"flag"
	"io"
	"testing"
)

func TestConfigDefaultsEnvironmentAndFlags(t *testing.T) {
	t.Setenv("ZHIMU_CAMERA_TOKEN", "env-token")
	c, err := ParseConfig(nil, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if c.HTTPHost != "0.0.0.0" || c.HTTPPort != 8080 || c.UDPPort != 5000 || c.CameraToken != "env-token" || c.DisableUDP || c.CameraMaxFrameBytes != 1048576 || c.ExtractorMaxBytes != 8388608 || c.CameraBufferSeconds != 120 || c.CameraPlaybackDelay != 30 || c.CameraPlaybackFPS != 24 || c.CameraStartBufferSeconds != 30 || c.CameraStartBufferFrames != 120 || c.CameraMinPlaybackRemaining != 10 {
		t.Fatalf("defaults %+v", c)
	}
	c, err = ParseConfig([]string{"--http-host", "127.0.0.1", "--http-port", "0", "--udp-port", "1234", "--camera-token", "override", "--packet-is-frame", "--enable-esp32-cam-udp", "--disable-esp32-cam-udp", "--serial-port", "COM7", "--serial-baud", "9600", "--control-step", "7", "--control-timeout", "0.5"}, io.Discard)
	if err != nil || c.CameraToken != "override" || !c.DisableUDP || !c.EnableUDP || !c.PacketIsFrame || c.SerialPort != "COM7" || c.SerialBaud != 9600 || c.ControlTimeout != 0.5 {
		t.Fatalf("flags %+v %v", c, err)
	}
}
func TestConfigValidation(t *testing.T) {
	for _, args := range [][]string{{"--http-port", "65536"}, {"--udp-port", "-1"}, {"--camera-playback-delay", "NaN"}, {"--camera-playback-fps", "+Inf"}, {"--control-timeout", "0"}, {"--camera-max-frame-bytes", "3"}, {"--extractor-max-bytes", "-1"}, {"--camera-start-buffer-frames", "-1"}, {"extra"}, {"--unknown"}} {
		if _, err := ParseConfig(args, io.Discard); err == nil {
			t.Fatal("accepted", args)
		}
	}
	if _, err := ParseConfig([]string{"--help"}, io.Discard); err != flag.ErrHelp {
		t.Fatal("help", err)
	}
}
