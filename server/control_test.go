package main

import (
	"bufio"
	"encoding/json"
	"net"
	"strings"
	"testing"
	"time"
)

func TestNormalizeControlMessage(t *testing.T) {
	for cmd := range validCommands {
		got, p, err := NormalizeControlMessage(strings.ToUpper(cmd), 5)
		if err != nil || got != cmd {
			t.Fatalf("%s: %v %s", cmd, err, got)
		}
		_, has := p["step"]
		if has != (cmd == "up" || cmd == "down" || cmd == "left" || cmd == "right") {
			t.Fatalf("step for %s: %v", cmd, p)
		}
	}
	original := map[string]any{"step": 3}
	_, p, e := NormalizeControlMessage(map[string]any{"command": "left", "step": 9, "payload": original}, 5)
	if e != nil || p["step"] != 3 {
		t.Fatal(p, e)
	}
	p["step"] = 4
	if original["step"] != 3 {
		t.Fatal("input payload mutated")
	}
	_, p, e = NormalizeControlMessage(map[string]any{"cmd": "up", "step": 7}, 5)
	if e != nil || p["step"] != 7 {
		t.Fatal(p, e)
	}
	for _, v := range []any{nil, 42, map[string]any{}, "bad", map[string]any{"cmd": map[string]any{}}} {
		if _, _, err := NormalizeControlMessage(v, 5); err == nil {
			t.Fatalf("accepted %v", v)
		}
	}
}
func testBridge(timeout time.Duration) *DeviceBridge {
	return NewDeviceBridge(NewSerialBridge("", 115200, timeout), timeout)
}
func testSocketPair() (*webSocket, net.Conn) {
	a, b := net.Pipe()
	return &webSocket{conn: a, reader: bufio.NewReader(a)}, b
}
func readServerFrame(t *testing.T, c net.Conn) map[string]any {
	t.Helper()
	var h [2]byte
	readExact(t, c, h[:])
	n := int(h[1])
	if n == 126 {
		var ext [2]byte
		readExact(t, c, ext[:])
		n = int(ext[0])<<8 | int(ext[1])
	}
	p := make([]byte, n)
	readExact(t, c, p)
	var m map[string]any
	if err := json.Unmarshal(p, &m); err != nil {
		t.Fatal(err)
	}
	return m
}
func readExact(t *testing.T, c net.Conn, p []byte) {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(time.Second))
	for len(p) > 0 {
		n, e := c.Read(p)
		if e != nil {
			t.Fatal(e)
		}
		p = p[n:]
	}
}
func TestDeviceCommandCorrelationAndTakeover(t *testing.T) {
	b := testBridge(time.Second)
	defer b.Close()
	first, peer := testSocketPair()
	defer peer.Close()
	b.attach(first)
	result := make(chan map[string]any, 1)
	go func() { result <- b.SendCommand("left", map[string]any{"step": 5}) }()
	frame := readServerFrame(t, peer)
	if frame["type"] != "servo_cmd" {
		t.Fatal(frame)
	}
	b.handleMessage(first, map[string]any{"type": "servo_ack", "seq": frame["seq"], "ok": true, "payload": map[string]any{"mg996r": 95}})
	if r := <-result; r["ok"] != true || r["route"] != "device" {
		t.Fatal(r)
	}
	go func() { result <- b.SendCommand("center", nil) }()
	_ = readServerFrame(t, peer)
	second, peer2 := testSocketPair()
	defer peer2.Close()
	b.attach(second)
	if r := <-result; r["error"] != "device websocket replaced" {
		t.Fatal(r)
	}
	b.handleMessage(first, map[string]any{"type": "hello", "device_id": "stale"})
	b.detach(first)
	if d := b.Status()["device"].(map[string]any); d["online"] != true || d["device_id"] != nil {
		t.Fatal(d)
	}
	b.handleMessage(second, map[string]any{"type": "hello", "device_id": "new"})
	if b.Status()["device"].(map[string]any)["device_id"] != "new" {
		t.Fatal(b.Status())
	}
}
func TestDeviceTimeoutAndOffline(t *testing.T) {
	b := testBridge(20 * time.Millisecond)
	defer b.Close()
	if b.SendCommand("status", nil)["route"] != "offline" {
		t.Fatal(b.Status())
	}
	c, peer := testSocketPair()
	defer peer.Close()
	b.attach(c)
	result := make(chan map[string]any, 1)
	go func() { result <- b.SendCommand("status", nil) }()
	readServerFrame(t, peer)
	if r := <-result; r["error"] != "device ack timeout" {
		t.Fatal(r)
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if len(b.pending) != 0 {
		t.Fatal("pending command leak")
	}
}
func TestUARTFallback(t *testing.T) {
	b := testBridge(time.Second)
	a, p := net.Pipe()
	defer p.Close()
	b.fallback.mu.Lock()
	b.fallback.port = a
	b.fallback.status["online"] = true
	b.fallback.mu.Unlock()
	go b.fallback.readLoop(a)
	defer b.Close()
	result := make(chan map[string]any, 1)
	go func() { result <- b.SendCommand("up", map[string]any{"step": 5}) }()
	line, err := bufio.NewReader(p).ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var frame map[string]any
	json.Unmarshal(line, &frame)
	if frame["type"] != "cmd" {
		t.Fatal(frame)
	}
	ack, _ := json.Marshal(map[string]any{"type": "ack", "seq": frame["seq"], "ok": true, "payload": map[string]any{"pan": 90, "tilt": 85}})
	p.Write(append(ack, '\n'))
	if r := <-result; r["ok"] != true || r["route"] != "uart" {
		t.Fatal(r)
	}
}
func TestSerialTimeout(t *testing.T) {
	b := NewSerialBridge("", 115200, 20*time.Millisecond)
	a, p := net.Pipe()
	defer p.Close()
	b.port = a
	b.status["online"] = true
	defer b.Close()
	result := make(chan map[string]any, 1)
	go func() { result <- b.SendCommand("status", nil) }()
	bufio.NewReader(p).ReadBytes('\n')
	if r := <-result; r["error"] != "serial ack timeout" {
		t.Fatal(r)
	}
}
