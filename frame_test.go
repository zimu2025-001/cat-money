package main

import (
	"context"
	"testing"
	"time"
)

func TestFrameBufferWakeTimeoutAndOwnership(t *testing.T) {
	b := NewFrameBuffer(10)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if f := b.WaitNext(ctx, -1, 0); len(f.JPEG) != 0 || f.Seq != 0 {
		t.Fatal("initial frame")
	}
	result := make(chan Frame, 1)
	go func() { result <- b.WaitNext(ctx, -1, time.Second) }()
	select {
	case <-result:
		t.Fatal("empty predicate woke")
	case <-time.After(10 * time.Millisecond):
	}
	jpeg := append([]byte(nil), testJPEG...)
	b.Update(jpeg, "udp", nil, "frame received")
	jpeg[0] = 0
	select {
	case f := <-result:
		if !isCompleteJPEG(f.JPEG) || f.Seq != 1 {
			t.Fatal("frame ownership")
		}
	case <-time.After(time.Second):
		t.Fatal("wait not woken")
	}
	if f := b.WaitNext(ctx, -1, 0); f.Seq != 1 {
		t.Fatal("latest not immediate")
	}
	cancel()
	if f := b.WaitNext(ctx, 1, time.Hour); f.Seq != 0 {
		t.Fatal("cancel not honored")
	}
}
func TestFrameBufferReplayPruningAndStatus(t *testing.T) {
	b := NewFrameBuffer(3)
	now := time.Unix(100, 0)
	b.now = func() time.Time { return now }
	initial := b.Status(10)
	if initial["online"] != false || initial["last_frame_age"] != nil || initial["camera_id"] != nil || initial["source"] != "none" {
		t.Fatalf("initial status %v", initial)
	}
	for i := 0; i < 6; i++ {
		b.Update(testJPEG, "esp32-http", "cam", "http frame")
		now = now.Add(time.Second)
	}
	s := b.Status(10)
	if s["buffered_frames"] != 4 || s["buffered_seconds"] != float64(3) || s["source_fps"] != float64(1) || s["seq"] != int64(6) {
		t.Fatalf("status %v", s)
	}
	if f := b.Replay(-1, time.Unix(103, 0)); f.Seq != 4 {
		t.Fatalf("initial replay must choose newest eligible: %v", f.Seq)
	}
	if f := b.Replay(4, time.Unix(105, 0)); f.Seq != 5 {
		t.Fatalf("replay must advance sequentially: %v", f.Seq)
	}
	if f := b.Replay(6, now); len(f.JPEG) != 0 {
		t.Fatal("replay invented frame")
	}
	if f := b.Replay(-1, time.Unix(100, 0)); len(f.JPEG) != 0 {
		t.Fatal("replay crossed cutoff")
	}
	now = now.Add(20 * time.Second)
	if b.Status(10)["online"] != false {
		t.Fatal("offline threshold")
	}
}
