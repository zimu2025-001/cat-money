package main

import (
	"context"
	"sync"
	"time"
)

type Frame struct {
	JPEG []byte
	Seq  int64
	Time time.Time
}

// FrameBuffer publishes immutable frames. Slow viewers receive the newest live
// frame instead of creating an unbounded queue; replay uses the retention ring.
type FrameBuffer struct {
	mu        sync.Mutex
	retention time.Duration
	frames    []Frame
	head      int
	latest    Frame
	source    string
	cameraID  any
	message   string
	changed   chan struct{}
	now       func() time.Time
}

func NewFrameBuffer(seconds float64) *FrameBuffer {
	if seconds < 1 {
		seconds = 1
	}
	return &FrameBuffer{retention: time.Duration(seconds * float64(time.Second)), source: "none", message: "waiting for frame", changed: make(chan struct{}), now: time.Now}
}

func (b *FrameBuffer) Update(jpeg []byte, source string, cameraID any, message string) int64 {
	b.mu.Lock()
	defer b.mu.Unlock()
	if len(jpeg) == 0 {
		return b.latest.Seq
	}
	frame := Frame{JPEG: append([]byte(nil), jpeg...), Seq: b.latest.Seq + 1, Time: b.now()}
	b.latest = frame
	b.source = source
	b.cameraID = cameraID
	b.message = message
	b.frames = append(b.frames, frame)
	cutoff := frame.Time.Add(-b.retention)
	for len(b.frames)-b.head > 1 && b.frames[b.head].Time.Before(cutoff) {
		b.frames[b.head] = Frame{}
		b.head++
	}
	if b.head > 1024 && b.head*2 >= len(b.frames) {
		b.frames = append([]Frame(nil), b.frames[b.head:]...)
		b.head = 0
	}
	close(b.changed)
	b.changed = make(chan struct{})
	return frame.Seq
}

func (b *FrameBuffer) Latest() Frame { b.mu.Lock(); defer b.mu.Unlock(); return b.latest }

func (b *FrameBuffer) WaitNext(ctx context.Context, lastSeq int64, timeout time.Duration) Frame {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	for {
		b.mu.Lock()
		frame, changed := b.latest, b.changed
		b.mu.Unlock()
		if len(frame.JPEG) > 0 && frame.Seq > lastSeq {
			return frame
		}
		select {
		case <-ctx.Done():
			return Frame{}
		case <-timer.C:
			return b.Latest()
		case <-changed:
		}
	}
}

func (b *FrameBuffer) Replay(lastSeq int64, allowed time.Time) Frame {
	b.mu.Lock()
	defer b.mu.Unlock()
	if lastSeq < 0 {
		for i := len(b.frames) - 1; i >= b.head; i-- {
			if !b.frames[i].Time.After(allowed) {
				return b.frames[i]
			}
		}
		return Frame{Seq: lastSeq}
	}
	for _, f := range b.frames[b.head:] {
		if f.Seq <= lastSeq {
			continue
		}
		if f.Time.After(allowed) {
			break
		}
		return f
	}
	return Frame{Seq: lastSeq}
}

func (b *FrameBuffer) Status(offlineAfter float64) map[string]any {
	b.mu.Lock()
	defer b.mu.Unlock()
	var age any
	online := false
	if !b.latest.Time.IsZero() {
		seconds := b.now().Sub(b.latest.Time).Seconds()
		age = seconds
		online = seconds <= offlineAfter
	}
	count := len(b.frames) - b.head
	duration, fps := 0.0, 0.0
	if count >= 2 {
		duration = b.frames[len(b.frames)-1].Time.Sub(b.frames[b.head].Time).Seconds()
		if duration > 0 {
			fps = float64(count-1) / duration
		}
	}
	return map[string]any{"online": online, "source": b.source, "camera_id": b.cameraID, "seq": b.latest.Seq, "last_frame_age": age, "buffered_frames": count, "buffered_seconds": duration, "source_fps": fps, "message": b.message}
}

func isCompleteJPEG(frame []byte) bool {
	return len(frame) >= 4 && frame[0] == 0xff && frame[1] == 0xd8 && frame[len(frame)-2] == 0xff && frame[len(frame)-1] == 0xd9
}
