package main

import (
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"time"
)

var validCommands = map[string]bool{"up": true, "down": true, "left": true, "right": true, "stop": true, "center": true, "status": true, "swing_on": true, "swing_off": true, "swing_toggle": true}

func NormalizeControlMessage(message any, defaultStep int) (string, map[string]any, error) {
	var cmd string
	payload := map[string]any{}
	switch m := message.(type) {
	case string:
		cmd = m
	case map[string]any:
		value := m["cmd"]
		if value == nil || value == "" {
			value = m["command"]
		}
		var ok bool
		cmd, ok = value.(string)
		if !ok {
			return "", nil, fmt.Errorf("missing command")
		}
		if p, ok := m["payload"].(map[string]any); ok {
			for k, v := range p {
				payload[k] = v
			}
		}
		if v, ok := m["step"]; ok {
			if _, exists := payload["step"]; !exists {
				payload["step"] = v
			}
		}
	default:
		return "", nil, fmt.Errorf("message must be a JSON object or command string")
	}
	cmd = strings.ToLower(cmd)
	if !validCommands[cmd] {
		return "", nil, fmt.Errorf("invalid command: %s", cmd)
	}
	if cmd == "up" || cmd == "down" || cmd == "left" || cmd == "right" {
		if _, ok := payload["step"]; !ok {
			payload["step"] = defaultStep
		}
	}
	return cmd, payload, nil
}

func copyMap(m map[string]any) map[string]any {
	out := make(map[string]any, len(m))
	for k, v := range m {
		out[k] = v
	}
	return out
}
func messageSeq(v any) (int, bool) {
	switch n := v.(type) {
	case float64:
		if n >= 0 && n <= 2147483647 && n == float64(int(n)) {
			return int(n), true
		}
	case int:
		return n, true
	case json.Number:
		i, e := n.Int64()
		return int(i), e == nil && i >= 0 && i <= 2147483647
	}
	return 0, false
}
func messageOK(m map[string]any) bool {
	v, ok := m["ok"]
	if !ok {
		return true
	}
	b, ok := v.(bool)
	return ok && b
}
func responsePayload(m map[string]any) map[string]any {
	p, _ := m["payload"].(map[string]any)
	if p == nil {
		p = map[string]any{}
	}
	return p
}

type devicePending struct {
	conn  *webSocket
	reply chan map[string]any
}
type DeviceBridge struct {
	mu       sync.Mutex
	fallback *SerialBridge
	timeout  time.Duration
	conn     *webSocket
	seq      int
	pending  map[int]devicePending
	status   map[string]any
	closed   bool
	browsers map[*webSocket]struct{}
}

func NewDeviceBridge(fallback *SerialBridge, timeout time.Duration) *DeviceBridge {
	return &DeviceBridge{fallback: fallback, timeout: timeout, pending: map[int]devicePending{}, browsers: map[*webSocket]struct{}{}, status: map[string]any{"online": false, "message": "device websocket disconnected", "device_id": nil, "mg996r": nil, "sg90": nil, "swing": nil}}
}
func (b *DeviceBridge) Status() map[string]any {
	b.mu.Lock()
	d := copyMap(b.status)
	b.mu.Unlock()
	u := b.fallback.Status()
	route := "offline"
	if d["online"] == true {
		route = "device"
	} else if u["online"] == true {
		route = "uart"
	}
	return map[string]any{"route": route, "device": d, "uart": u}
}
func (b *DeviceBridge) failPendingLocked(conn *webSocket, reason string) {
	for seq, p := range b.pending {
		if p.conn == conn {
			select {
			case p.reply <- map[string]any{"ok": false, "seq": seq, "error": reason, "route": "device"}:
			default:
			}
			delete(b.pending, seq)
		}
	}
}
func (b *DeviceBridge) attach(conn *webSocket) bool {
	b.mu.Lock()
	if b.closed {
		b.mu.Unlock()
		return false
	}
	old := b.conn
	if old != nil {
		b.failPendingLocked(old, "device websocket replaced")
	}
	b.conn = conn
	b.status = map[string]any{"online": true, "message": "device websocket connected", "device_id": nil, "mg996r": nil, "sg90": nil, "swing": nil}
	b.mu.Unlock()
	if old != nil && old != conn {
		old.Close()
	}
	return true
}
func (b *DeviceBridge) detach(conn *webSocket) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.failPendingLocked(conn, "device websocket disconnected")
	if b.conn == conn {
		b.conn = nil
		b.status["online"] = false
		b.status["message"] = "device websocket disconnected"
	}
}
func (b *DeviceBridge) handleMessage(conn *webSocket, m map[string]any) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.conn != conn {
		return
	}
	p := responsePayload(m)
	switch m["type"] {
	case "hello":
		b.status["online"] = true
		b.status["message"] = "device ready"
		b.status["device_id"] = m["device_id"]
	case "servo_ack", "servo_status", "status":
		b.status["online"] = true
		b.status["message"] = "ok"
		if !messageOK(m) {
			b.status["message"] = m["error"]
		}
		for _, k := range []string{"mg996r", "sg90", "swing"} {
			if v, ok := p[k]; ok {
				b.status[k] = v
			}
		}
		if seq, ok := messageSeq(m["seq"]); ok {
			if pending, exists := b.pending[seq]; exists && pending.conn == conn {
				select {
				case pending.reply <- map[string]any{"ok": messageOK(m), "seq": seq, "type": m["type"], "payload": p, "error": m["error"], "route": "device"}:
				default:
				}
				delete(b.pending, seq)
			}
		}
	}
}
func (b *DeviceBridge) SendCommand(cmd string, payload map[string]any) map[string]any {
	if !validCommands[cmd] {
		return map[string]any{"ok": false, "error": "invalid command: " + cmd}
	}
	if payload == nil {
		payload = map[string]any{}
	}
	b.mu.Lock()
	conn := b.conn
	if conn == nil {
		b.mu.Unlock()
		if b.fallback.Available() {
			r := b.fallback.SendCommand(cmd, payload)
			r["route"] = "uart"
			return r
		}
		return map[string]any{"ok": false, "error": "device websocket disconnected", "route": "offline", "status": b.Status()}
	}
	if len(b.pending) >= 1024 {
		b.mu.Unlock()
		return map[string]any{"ok": false, "error": "too many pending commands", "route": "device"}
	}
	for {
		b.seq = (b.seq + 1) & 0x7fffffff
		if _, exists := b.pending[b.seq]; !exists {
			break
		}
	}
	seq := b.seq
	reply := make(chan map[string]any, 1)
	b.pending[seq] = devicePending{conn, reply}
	b.mu.Unlock()
	defer func() { b.mu.Lock(); delete(b.pending, seq); b.mu.Unlock() }()
	if err := conn.WriteJSON(map[string]any{"type": "servo_cmd", "seq": seq, "cmd": cmd, "payload": payload}); err != nil {
		b.detach(conn)
		conn.Close()
		r := b.fallback.SendCommand(cmd, payload)
		r["route"] = "uart"
		return r
	}
	timer := time.NewTimer(b.timeout)
	defer timer.Stop()
	select {
	case r := <-reply:
		return r
	case <-timer.C:
		return map[string]any{"ok": false, "error": "device ack timeout", "seq": seq, "route": "device", "status": b.Status()}
	}
}
func (b *DeviceBridge) Close() error {
	b.mu.Lock()
	b.closed = true
	c := b.conn
	clients := make([]*webSocket, 0, len(b.browsers))
	for client := range b.browsers {
		clients = append(clients, client)
	}
	b.mu.Unlock()
	for _, client := range clients {
		client.Close()
	}
	if c != nil {
		b.detach(c)
		c.Close()
	}
	return b.fallback.Close()
}
