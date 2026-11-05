package main

import (
	"io"
	"sync"
	"testing"
	"time"
)

// Unlike net.Conn, this fake deliberately permits post-close Write calls so
// the test detects attempts to use drivers whose Write bypasses close checks.
type guardedSerialFake struct {
	mu          sync.Mutex
	writes      int
	afterClose  int
	closed      bool
	entered     chan struct{}
	release     chan struct{}
	closeSignal chan struct{}
	once        sync.Once
}

func newGuardedSerialFake() *guardedSerialFake {
	return &guardedSerialFake{entered: make(chan struct{}), release: make(chan struct{}), closeSignal: make(chan struct{})}
}
func (p *guardedSerialFake) Read([]byte) (int, error) { <-p.closeSignal; return 0, io.EOF }
func (p *guardedSerialFake) Write(b []byte) (int, error) {
	p.mu.Lock()
	p.writes++
	first := p.writes == 1
	if p.closed {
		p.afterClose++
	}
	p.mu.Unlock()
	if first {
		close(p.entered)
		<-p.release
	}
	return len(b), nil
}
func (p *guardedSerialFake) Close() error {
	p.mu.Lock()
	p.closed = true
	p.mu.Unlock()
	p.once.Do(func() { close(p.closeSignal) })
	return nil
}
func waitSerialResult(t *testing.T, ch <-chan map[string]any) map[string]any {
	t.Helper()
	select {
	case r := <-ch:
		return r
	case <-time.After(time.Second):
		t.Fatal("serial operation did not return promptly")
		return nil
	}
}
func TestSerialQueuedWriteAfterClose(t *testing.T) {
	b := NewSerialBridge("", 115200, 50*time.Millisecond)
	p := newGuardedSerialFake()
	b.port = p
	b.status["online"] = true
	first := make(chan map[string]any, 1)
	second := make(chan map[string]any, 1)
	go func() { first <- b.SendCommand("up", nil) }()
	<-p.entered
	go func() { second <- b.SendCommand("down", nil) }()
	deadline := time.Now().Add(time.Second)
	for {
		b.mu.Lock()
		pending := len(b.pending)
		b.mu.Unlock()
		if pending == 2 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("second command not queued")
		}
		time.Sleep(time.Millisecond)
	}
	closed := make(chan struct{})
	go func() { b.Close(); close(closed) }()
	select {
	case <-closed:
	case <-time.After(time.Second):
		t.Fatal("Close blocked on driver")
	}
	waitSerialResult(t, first)
	waitSerialResult(t, second)
	p.mu.Lock()
	physicallyClosed := p.closed
	p.mu.Unlock()
	if physicallyClosed {
		t.Fatal("closed descriptor beneath active writer")
	}
	close(p.release)
	select {
	case <-p.closeSignal:
	case <-time.After(time.Second):
		t.Fatal("deferred cleanup did not close port")
	}
	// Wait for the queued writer to get the lock and check the disconnected port.
	b.writeMu.Lock()
	b.writeMu.Unlock()
	time.Sleep(10 * time.Millisecond)
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.writes != 1 || p.afterClose != 0 {
		t.Fatalf("writes=%d post-close=%d", p.writes, p.afterClose)
	}
}
func TestSerialQueuedWriteAfterTimeout(t *testing.T) {
	b := NewSerialBridge("", 115200, 20*time.Millisecond)
	p := newGuardedSerialFake()
	b.port = p
	b.status["online"] = true
	first := make(chan map[string]any, 1)
	go func() { first <- b.SendCommand("status", nil) }()
	<-p.entered
	if r := waitSerialResult(t, first); r["error"] != "serial write timeout" {
		t.Fatal(r)
	}
	if b.Available() {
		t.Fatal("timed-out port still available")
	}
	if r := b.SendCommand("center", nil); r["ok"] != false {
		t.Fatal(r)
	}
	close(p.release)
	select {
	case <-p.closeSignal:
	case <-time.After(time.Second):
		t.Fatal("port not closed after writer returned")
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.writes != 1 || p.afterClose != 0 {
		t.Fatal(p.writes, p.afterClose)
	}
}
