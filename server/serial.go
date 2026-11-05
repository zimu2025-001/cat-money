package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"sync"
	"time"

	"go.bug.st/serial"
)

type serialTransport interface{ io.ReadWriteCloser }
type SerialBridge struct {
	mu       sync.Mutex
	writeMu  sync.Mutex
	portName string
	baud     int
	timeout  time.Duration
	port     serialTransport
	seq      int
	pending  map[int]chan map[string]any
	status   map[string]any
	closed   bool
}

func NewSerialBridge(port string, baud int, timeout time.Duration) *SerialBridge {
	return &SerialBridge{portName: port, baud: baud, timeout: timeout, pending: map[int]chan map[string]any{}, status: map[string]any{"online": false, "message": "serial disabled", "pan": nil, "tilt": nil}}
}
func (b *SerialBridge) Start() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.closed {
		return fmt.Errorf("serial bridge closed")
	}
	if b.port != nil || b.portName == "" {
		return nil
	}
	p, err := serial.Open(b.portName, &serial.Mode{BaudRate: b.baud, DataBits: 8, Parity: serial.NoParity, StopBits: serial.OneStopBit})
	if err != nil {
		b.status["message"] = "serial open failed: " + err.Error()
		return err
	}
	b.port = p
	b.status["online"] = true
	b.status["message"] = "serial connected: " + b.portName
	go b.readLoop(p)
	return nil
}
func (b *SerialBridge) Status() map[string]any {
	b.mu.Lock()
	defer b.mu.Unlock()
	return copyMap(b.status)
}
func (b *SerialBridge) Available() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.port != nil && !b.closed
}
func (b *SerialBridge) disconnected(p serialTransport, reason string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.port != p {
		return
	}
	b.port = nil
	b.status["online"] = false
	b.status["message"] = reason
	for seq, reply := range b.pending {
		select {
		case reply <- map[string]any{"ok": false, "seq": seq, "error": reason}:
		default:
		}
		delete(b.pending, seq)
	}
}
func (b *SerialBridge) Close() error {
	b.mu.Lock()
	b.closed = true
	p := b.port
	b.mu.Unlock()
	if p == nil {
		return nil
	}
	b.disconnected(p, "serial closed")
	return b.closePort(p)
}

// closePort never closes a descriptor underneath a writer. If a platform
// driver is stuck in Write, the caller still returns promptly and cleanup runs
// after that write exits. New/queued commands cannot use the disconnected port.
func (b *SerialBridge) closePort(p serialTransport) error {
	if b.writeMu.TryLock() {
		defer b.writeMu.Unlock()
		return p.Close()
	}
	go func() {
		b.writeMu.Lock()
		defer b.writeMu.Unlock()
		_ = p.Close()
	}()
	return nil
}

func (b *SerialBridge) SendCommand(cmd string, payload map[string]any) map[string]any {
	if !validCommands[cmd] {
		return map[string]any{"ok": false, "error": "invalid command: " + cmd}
	}
	if payload == nil {
		payload = map[string]any{}
	}
	b.mu.Lock()
	p := b.port
	if p == nil || b.closed {
		status := copyMap(b.status)
		b.mu.Unlock()
		return map[string]any{"ok": false, "error": status["message"], "status": status}
	}
	if len(b.pending) >= 1024 {
		b.mu.Unlock()
		return map[string]any{"ok": false, "error": "too many pending commands"}
	}
	for {
		b.seq = (b.seq + 1) & 0x7fffffff
		if _, exists := b.pending[b.seq]; !exists {
			break
		}
	}
	seq := b.seq
	reply := make(chan map[string]any, 1)
	b.pending[seq] = reply
	b.mu.Unlock()
	defer func() { b.mu.Lock(); delete(b.pending, seq); b.mu.Unlock() }()
	line, err := json.Marshal(map[string]any{"type": "cmd", "seq": seq, "cmd": cmd, "payload": payload})
	if err != nil {
		return map[string]any{"ok": false, "error": err.Error()}
	}
	line = append(line, '\n')
	// Serial drivers do not all expose write deadlines. Bound the whole operation
	// with the command timeout. Disconnect immediately on timeout; defer physical
	// close until any in-flight write exits so the driver cannot use a recycled fd.
	written := make(chan error, 1)
	go func() {
		b.writeMu.Lock()
		defer b.writeMu.Unlock()
		remaining := line
		for len(remaining) > 0 {
			// The library's Unix Write does not check whether its descriptor was
			// closed. All physical closes share writeMu; validate after taking
			// that lock and again before every partial-write retry.
			b.mu.Lock()
			active := b.port == p && !b.closed
			b.mu.Unlock()
			if !active {
				written <- io.ErrClosedPipe
				return
			}
			n, e := p.Write(remaining)
			if e != nil {
				written <- e
				return
			}
			if n == 0 {
				written <- io.ErrShortWrite
				return
			}
			remaining = remaining[n:]
		}
		written <- nil
	}()
	timer := time.NewTimer(b.timeout)
	defer timer.Stop()
	select {
	case err = <-written:
		if err != nil {
			b.disconnected(p, "serial write failed: "+err.Error())
			_ = b.closePort(p)
			return map[string]any{"ok": false, "error": err.Error(), "status": b.Status()}
		}
	case <-timer.C:
		b.disconnected(p, "serial write timeout")
		_ = b.closePort(p)
		return map[string]any{"ok": false, "error": "serial write timeout", "seq": seq, "status": b.Status()}
	}
	select {
	case r := <-reply:
		return r
	case <-timer.C:
		return map[string]any{"ok": false, "error": "serial ack timeout", "seq": seq, "status": b.Status()}
	}
}
func (b *SerialBridge) readLoop(p serialTransport) {
	scanner := bufio.NewScanner(p)
	scanner.Buffer(make([]byte, 4096), maxWSMessage)
	for scanner.Scan() {
		var m map[string]any
		if json.Unmarshal(scanner.Bytes(), &m) == nil && m != nil {
			b.handleSerialMessage(p, m)
		}
	}
	reason := "serial disconnected"
	if err := scanner.Err(); err != nil {
		reason = "serial read failed: " + err.Error()
	}
	b.disconnected(p, reason)
	_ = b.closePort(p)
}
func (b *SerialBridge) handleSerialMessage(port serialTransport, m map[string]any) {
	p := responsePayload(m)
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.port != port {
		return
	}
	if m["type"] == "ack" || m["type"] == "status" {
		b.status["online"] = true
		b.status["message"] = "ok"
		for _, k := range []string{"pan", "tilt"} {
			if v, ok := p[k]; ok {
				b.status[k] = v
			}
		}
	}
	if seq, ok := messageSeq(m["seq"]); ok {
		if reply, exists := b.pending[seq]; exists {
			typ := m["type"]
			if typ == nil {
				typ = "ack"
			}
			select {
			case reply <- map[string]any{"ok": messageOK(m), "seq": seq, "type": typ, "payload": p, "error": m["error"]}:
			default:
			}
			delete(b.pending, seq)
		}
	}
}
