package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

const maxWSMessage = 64 * 1024

// A connection has exactly one reader. All writes, including control frames,
// share a lock so acknowledgments and ping replies cannot interleave.
type webSocket struct {
	conn      net.Conn
	reader    *bufio.Reader
	writeMu   sync.Mutex
	closeOnce sync.Once
}

func (c *webSocket) Close() { c.closeOnce.Do(func() { c.conn.Close() }) }
func headerToken(value, token string) bool {
	for _, part := range strings.Split(value, ",") {
		if strings.EqualFold(strings.TrimSpace(part), token) {
			return true
		}
	}
	return false
}
func upgradeWebSocket(w http.ResponseWriter, r *http.Request) (*webSocket, error) {
	key := r.Header.Get("Sec-WebSocket-Key")
	decoded, err := base64.StdEncoding.DecodeString(key)
	if r.Method != "GET" || !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") || !headerToken(r.Header.Get("Connection"), "upgrade") || r.Header.Get("Sec-WebSocket-Version") != "13" || err != nil || len(decoded) != 16 {
		http.Error(w, "Expected WebSocket version 13 upgrade with valid key", http.StatusBadRequest)
		return nil, errors.New("invalid websocket handshake")
	}
	h, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "WebSocket unavailable", http.StatusInternalServerError)
		return nil, errors.New("hijacking unsupported")
	}
	conn, rw, err := h.Hijack()
	if err != nil {
		return nil, err
	}
	digest := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	_, err = fmt.Fprintf(rw, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", base64.StdEncoding.EncodeToString(digest[:]))
	if err == nil {
		err = rw.Flush()
	}
	conn.SetDeadline(time.Time{})
	if err != nil {
		conn.Close()
		return nil, err
	}
	return &webSocket{conn: conn, reader: rw.Reader}, nil
}
func (c *webSocket) WriteJSON(v any) error {
	p, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return c.writeFrame(1, p)
}
func (c *webSocket) writeFrame(op byte, p []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	defer c.conn.SetWriteDeadline(time.Time{})
	header := []byte{0x80 | op}
	switch {
	case len(p) < 126:
		header = append(header, byte(len(p)))
	case len(p) <= 65535:
		header = append(header, 126, byte(len(p)>>8), byte(len(p)))
	default:
		header = append(header, 127)
		header = binary.BigEndian.AppendUint64(header, uint64(len(p)))
	}
	bufs := net.Buffers{header, p}
	_, err := bufs.WriteTo(c.conn)
	return err
}
func (c *webSocket) protocolError(code uint16, msg string) error {
	p := binary.BigEndian.AppendUint16(nil, code)
	p = append(p, []byte(msg)...)
	_ = c.writeFrame(8, p)
	return errors.New(msg)
}
func (c *webSocket) readMessage() (byte, []byte, error) {
	var message []byte
	var messageOp byte
	for {
		var h [2]byte
		if _, err := io.ReadFull(c.reader, h[:]); err != nil {
			return 0, nil, err
		}
		fin := h[0]&0x80 != 0
		op := h[0] & 15
		masked := h[1]&0x80 != 0
		if h[0]&0x70 != 0 || !masked {
			return 0, nil, c.protocolError(1002, "invalid frame flags")
		}
		size := uint64(h[1] & 127)
		if size == 126 {
			var x [2]byte
			if _, err := io.ReadFull(c.reader, x[:]); err != nil {
				return 0, nil, err
			}
			size = uint64(binary.BigEndian.Uint16(x[:]))
			if size < 126 {
				return 0, nil, c.protocolError(1002, "noncanonical length")
			}
		} else if size == 127 {
			var x [8]byte
			if _, err := io.ReadFull(c.reader, x[:]); err != nil {
				return 0, nil, err
			}
			size = binary.BigEndian.Uint64(x[:])
			if size < 65536 || size>>63 != 0 {
				return 0, nil, c.protocolError(1002, "invalid length")
			}
		}
		control := op >= 8
		if control && (!fin || size > 125) {
			return 0, nil, c.protocolError(1002, "invalid control frame")
		}
		if size > maxWSMessage || (!control && uint64(len(message))+size > maxWSMessage) {
			return 0, nil, c.protocolError(1009, "message too large")
		}
		var mask [4]byte
		if _, err := io.ReadFull(c.reader, mask[:]); err != nil {
			return 0, nil, err
		}
		p := make([]byte, int(size))
		if _, err := io.ReadFull(c.reader, p); err != nil {
			return 0, nil, err
		}
		for i := range p {
			p[i] ^= mask[i%4]
		}
		switch op {
		case 8:
			if len(p) == 1 {
				return 0, nil, c.protocolError(1002, "invalid close payload")
			}
			if len(p) >= 2 {
				code := binary.BigEndian.Uint16(p)
				if code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 || code == 1015 || (code >= 1016 && code < 3000) {
					return 0, nil, c.protocolError(1002, "invalid close code")
				}
				if !utf8.Valid(p[2:]) {
					return 0, nil, c.protocolError(1007, "invalid close text")
				}
			}
			_ = c.writeFrame(8, p)
			return 0, nil, io.EOF
		case 9:
			if err := c.writeFrame(10, p); err != nil {
				return 0, nil, err
			}
			continue
		case 10:
			continue
		case 0:
			if messageOp == 0 {
				return 0, nil, c.protocolError(1002, "unexpected continuation")
			}
		case 1, 2:
			if messageOp != 0 {
				return 0, nil, c.protocolError(1002, "interleaved data message")
			}
			messageOp = op
		default:
			return 0, nil, c.protocolError(1002, "unknown opcode")
		}
		message = append(message, p...)
		if fin {
			if messageOp == 1 && !utf8.Valid(message) {
				return 0, nil, c.protocolError(1007, "invalid UTF-8")
			}
			return messageOp, message, nil
		}
	}
}
func (b *DeviceBridge) BrowserWebSocket(w http.ResponseWriter, r *http.Request, defaultStep int, status func() map[string]any) {
	c, err := upgradeWebSocket(w, r)
	if err != nil {
		return
	}
	defer c.Close()
	b.mu.Lock()
	if b.closed {
		b.mu.Unlock()
		return
	}
	b.browsers[c] = struct{}{}
	b.mu.Unlock()
	defer func() { b.mu.Lock(); delete(b.browsers, c); b.mu.Unlock() }()
	if err = c.WriteJSON(map[string]any{"type": "status", "payload": status()}); err != nil {
		return
	}
	for {
		op, p, err := c.readMessage()
		if err != nil {
			return
		}
		if op != 1 {
			if c.WriteJSON(map[string]any{"ok": false, "error": "only text websocket frames are supported"}) != nil {
				return
			}
			continue
		}
		var parsed any = strings.TrimSpace(string(p))
		if strings.HasPrefix(parsed.(string), "{") {
			if err = json.Unmarshal(p, &parsed); err != nil {
				if c.WriteJSON(map[string]any{"ok": false, "error": err.Error()}) != nil {
					return
				}
				continue
			}
		}
		cmd, payload, err := NormalizeControlMessage(parsed, defaultStep)
		var result map[string]any
		if err != nil {
			result = map[string]any{"ok": false, "error": err.Error()}
		} else {
			result = b.SendCommand(cmd, payload)
		}
		if c.WriteJSON(result) != nil {
			return
		}
	}
}
func (b *DeviceBridge) DeviceWebSocket(w http.ResponseWriter, r *http.Request) {
	c, err := upgradeWebSocket(w, r)
	if err != nil {
		return
	}
	defer c.Close()
	if !b.attach(c) {
		return
	}
	defer b.detach(c)
	for {
		op, p, err := c.readMessage()
		if err != nil {
			return
		}
		if op != 1 {
			continue
		}
		var m map[string]any
		if json.Unmarshal(p, &m) == nil && m != nil {
			b.handleMessage(c, m)
		}
	}
}
