package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func maskedFrame(op byte, fin bool, p []byte) []byte {
	first := op
	if fin {
		first |= 128
	}
	out := []byte{first}
	if len(p) < 126 {
		out = append(out, 128|byte(len(p)))
	} else {
		out = append(out, 128|126, byte(len(p)>>8), byte(len(p)))
	}
	mask := []byte{1, 2, 3, 4}
	out = append(out, mask...)
	for i, b := range p {
		out = append(out, b^mask[i%4])
	}
	return out
}
func TestWebSocketFragmentAndPing(t *testing.T) {
	c, peer := testSocketPair()
	defer c.Close()
	defer peer.Close()
	done := make(chan error, 1)
	go func() {
		op, p, e := c.readMessage()
		if e == nil && (op != 1 || string(p) != "hello") {
			t.Errorf("%d %q", op, p)
		}
		done <- e
	}()
	peer.Write(maskedFrame(1, false, []byte("hel")))
	peer.Write(maskedFrame(9, true, []byte("ping")))
	pong := make([]byte, 6)
	readExact(t, peer, pong)
	if !bytes.Equal(pong, append([]byte{0x8a, 4}, []byte("ping")...)) {
		t.Fatal(pong)
	}
	peer.Write(maskedFrame(0, true, []byte("lo")))
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
func TestWebSocketRejectsOversizedAndUnmasked(t *testing.T) {
	for _, bad := range [][]byte{{0x81, 0}, {0x81, 0xff, 0, 0, 0, 0, 0, 2, 0, 0}} {
		c, peer := testSocketPair()
		done := make(chan error, 1)
		go func() { _, _, e := c.readMessage(); done <- e }()
		peer.Write(bad)
		h := make([]byte, 2)
		readExact(t, peer, h)
		if h[0] != 0x88 {
			t.Fatal(h)
		}
		p := make([]byte, int(h[1]))
		readExact(t, peer, p)
		if err := <-done; err == nil {
			t.Fatal("bad frame accepted")
		}
		c.Close()
		peer.Close()
	}
}
func TestWebSocketClose(t *testing.T) {
	c, peer := testSocketPair()
	defer c.Close()
	defer peer.Close()
	done := make(chan error, 1)
	go func() { _, _, e := c.readMessage(); done <- e }()
	payload := binary.BigEndian.AppendUint16(nil, 1000)
	peer.Write(maskedFrame(8, true, payload))
	out := make([]byte, 4)
	readExact(t, peer, out)
	if out[0] != 0x88 || !bytes.Equal(out[2:], payload) {
		t.Fatal(out)
	}
	if e := <-done; e != io.EOF {
		t.Fatal(e)
	}
}
func TestDeviceBridgeCloseClosesConnections(t *testing.T) {
	b := testBridge(time.Second)
	c, peer := testSocketPair()
	defer peer.Close()
	b.attach(c)
	b.Close()
	peer.SetReadDeadline(time.Now().Add(time.Second))
	if _, e := peer.Read(make([]byte, 1)); e == nil {
		t.Fatal("connection still open")
	}
	next, other := testSocketPair()
	defer next.Close()
	defer other.Close()
	if b.attach(next) {
		t.Fatal("attached after shutdown")
	}
}

type bufferedTestConn struct {
	net.Conn
	reader *bufio.Reader
}

func (c bufferedTestConn) Read(p []byte) (int, error) { return c.reader.Read(p) }
func dialTestWebSocket(t *testing.T, url, path string) net.Conn {
	t.Helper()
	c, e := net.Dial("tcp", strings.TrimPrefix(url, "http://"))
	if e != nil {
		t.Fatal(e)
	}
	fmt.Fprintf(c, "GET %s HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", path)
	reader := bufio.NewReader(c)
	res, e := http.ReadResponse(reader, nil)
	if e != nil {
		t.Fatal(e)
	}
	if res.StatusCode != 101 || res.Header.Get("Sec-WebSocket-Accept") != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatal(res)
	}
	return bufferedTestConn{c, reader}
}
func TestWebSocketHTTPRelay(t *testing.T) {
	b := testBridge(time.Second)
	defer b.Close()
	mux := http.NewServeMux()
	mux.HandleFunc("/device", b.DeviceWebSocket)
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		b.BrowserWebSocket(w, r, 5, func() map[string]any { return map[string]any{"control": b.Status()} })
	})
	server := httptest.NewServer(mux)
	defer server.Close()
	device := dialTestWebSocket(t, server.URL, "/device")
	defer device.Close()
	device.Write(maskedFrame(1, true, []byte(`{"type":"hello","device_id":"test-cat"}`)))
	browser := dialTestWebSocket(t, server.URL, "/ws")
	defer browser.Close()
	if initial := readServerFrame(t, browser); initial["type"] != "status" {
		t.Fatal(initial)
	}
	browser.Write(maskedFrame(1, true, []byte(`{"command":"RIGHT","step":7}`)))
	cmd := readServerFrame(t, device)
	if cmd["type"] != "servo_cmd" || cmd["cmd"] != "right" || cmd["payload"].(map[string]any)["step"] != float64(7) {
		t.Fatal(cmd)
	}
	ack, _ := json.Marshal(map[string]any{"type": "servo_ack", "seq": cmd["seq"], "ok": true, "payload": map[string]any{"sg90": 97}})
	device.Write(maskedFrame(1, true, ack))
	if result := readServerFrame(t, browser); result["ok"] != true || result["route"] != "device" {
		t.Fatal(result)
	}
	b.Close()
	browser.SetReadDeadline(time.Now().Add(time.Second))
	if _, e := browser.Read(make([]byte, 1)); e == nil {
		t.Fatal("browser not closed on shutdown")
	}
}
