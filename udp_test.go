package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"net"
	"testing"
	"time"
)

var testJPEG = []byte{0xff, 0xd8, 'a', 'b', 'c', 'd', 'e', 'f', 0xff, 0xd9}

func udpPacket(id uint32, index int, payload []byte, length, count, version int, crc uint32) []byte {
	header := 18
	if version == 2 {
		header = 22
	}
	packet := make([]byte, header+len(payload))
	copy(packet, fmt.Sprintf("ZMC%d", version))
	binary.BigEndian.PutUint32(packet[4:], id)
	binary.BigEndian.PutUint32(packet[8:], uint32(length))
	offset := 12
	if version == 2 {
		binary.BigEndian.PutUint32(packet[12:], crc)
		offset = 16
	}
	binary.BigEndian.PutUint16(packet[offset:], uint16(index))
	binary.BigEndian.PutUint16(packet[offset+2:], uint16(count))
	binary.BigEndian.PutUint16(packet[offset+4:], uint16(len(payload)))
	copy(packet[header:], payload)
	return packet
}
func udpPackets(id uint32, jpeg []byte, version int) [][]byte {
	split := len(jpeg) / 2
	return [][]byte{udpPacket(id, 0, jpeg[:split], len(jpeg), 2, version, crc32.ChecksumIEEE(jpeg)), udpPacket(id, 1, jpeg[split:], len(jpeg), 2, version, crc32.ChecksumIEEE(jpeg))}
}
func completeUDP(a *FramedUDPAssembler, id uint32, sender string, version int) [][]byte {
	var frames [][]byte
	for _, packet := range udpPackets(id, testJPEG, version) {
		frames = append(frames, a.Feed(packet, sender)...)
	}
	return frames
}
func assertOneJPEG(t *testing.T, frames [][]byte) {
	t.Helper()
	if len(frames) != 1 || !bytes.Equal(frames[0], testJPEG) {
		t.Fatalf("want one JPEG, got %v", frames)
	}
}

func TestUDPNewerCompletionCannotRewind(t *testing.T) {
	a := NewFramedUDPAssembler(1024)
	old := udpPackets(10, testJPEG, 2)
	a.Feed(old[0], "a")
	assertOneJPEG(t, completeUDP(a, 11, "a", 2))
	for _, frames := range [][][]byte{a.Feed(old[1], "a"), completeUDP(a, 11, "a", 2), completeUDP(a, 9, "a", 2)} {
		if len(frames) != 0 {
			t.Fatal("stale JPEG accepted")
		}
	}
	assertOneJPEG(t, completeUDP(a, 12, "a", 2))
}
func TestUDPWraparoundAndReboot(t *testing.T) {
	a := NewFramedUDPAssembler(1024)
	now := time.Unix(100, 0)
	a.now = func() time.Time { return now }
	for _, id := range []uint32{0xfffffffe, 0xffffffff, 0, 1} {
		assertOneJPEG(t, completeUDP(a, id, "a", 2))
	}
	for _, id := range []uint32{0xffffffff, 0x80000001} {
		if len(completeUDP(a, id, "a", 2)) != 0 {
			t.Fatal("ambiguous/old frame accepted")
		}
	}
	assertOneJPEG(t, completeUDP(a, 200, "a", 2))
	now = now.Add(4900 * time.Millisecond)
	if len(completeUDP(a, 1, "a", 2)) != 0 {
		t.Fatal("reset before idle window")
	}
	now = now.Add(100 * time.Millisecond)
	assertOneJPEG(t, completeUDP(a, 2, "a", 2))
	if len(completeUDP(a, 1, "a", 2)) != 0 {
		t.Fatal("stale traffic extended reset")
	}
}
func TestUDPSenderIsolation(t *testing.T) {
	a := NewFramedUDPAssembler(1024)
	p := udpPackets(1, testJPEG, 2)
	if len(a.Feed(p[0], "a")) != 0 || len(a.Feed(p[1], "b")) != 0 {
		t.Fatal("mixed senders")
	}
	assertOneJPEG(t, a.Feed(p[1], "a"))
	assertOneJPEG(t, a.Feed(p[0], "b"))
	if len(completeUDP(a, 1, "a", 2)) != 0 {
		t.Fatal("duplicate")
	}
}
func TestUDPRejectMalformedHeaders(t *testing.T) {
	a := NewFramedUDPAssembler(1024)
	packets := [][]byte{[]byte("ZMC2"), []byte("no protocol"), udpPacket(1, 0, bytes.Repeat([]byte{'x'}, 11), 10, 2, 2, 0), udpPacket(2, 0, []byte{'x'}, 10, 11, 2, 0), udpPacket(3, 0, []byte{'x'}, 3, 2, 2, 0), udpPacket(4, 0, []byte{}, 10, 2, 2, 0), udpPacket(5, 2, []byte{'x'}, 10, 2, 2, 0), udpPacket(6, 0, []byte{'x'}, 1025, 2, 2, 0), udpPacket(7, 0, []byte{'x'}, 5000, 4097, 2, 0)}
	bad := udpPackets(8, testJPEG, 2)[0]
	bad[21]++
	packets = append(packets, bad)
	for _, packet := range packets {
		if len(a.Feed(packet, "a")) != 0 {
			t.Fatal("invalid accepted")
		}
	}
	if len(a.frames) != 0 {
		t.Fatal("malformed retained state")
	}
}
func TestUDPPoisonConflictsAndBounds(t *testing.T) {
	for _, kind := range []string{"payload", "length", "version", "crc", "count", "overshoot", "short", "jpeg", "checksum"} {
		t.Run(kind, func(t *testing.T) {
			a := NewFramedUDPAssembler(1024)
			parts := udpPackets(1, testJPEG, 2)
			a.Feed(parts[0], "a")
			switch kind {
			case "payload":
				bad := append([]byte(nil), parts[0]...)
				bad[len(bad)-1] ^= 1
				a.Feed(bad, "a")
			case "length":
				a.Feed(udpPacket(1, 1, testJPEG[5:], 11, 2, 2, crc32.ChecksumIEEE(testJPEG)), "a")
			case "version":
				a.Feed(udpPacket(1, 1, testJPEG[5:], 10, 2, 1, 0), "a")
			case "crc":
				a.Feed(udpPacket(1, 1, testJPEG[5:], 10, 2, 2, 0), "a")
			case "count":
				a.Feed(udpPacket(1, 1, testJPEG[5:], 10, 3, 2, crc32.ChecksumIEEE(testJPEG)), "a")
			case "overshoot":
				a.Feed(udpPacket(1, 1, append(testJPEG[5:], 0), 10, 2, 2, crc32.ChecksumIEEE(testJPEG)), "a")
			case "short":
				a.Feed(udpPacket(1, 1, testJPEG[5:9], 10, 2, 2, crc32.ChecksumIEEE(testJPEG)), "a")
			case "jpeg":
				bad := append([]byte(nil), parts[1]...)
				bad[len(bad)-1] = 0
				a.Feed(bad, "a")
			case "checksum":
				bad := append([]byte(nil), parts[1]...)
				bad[len(bad)-3] ^= 1
				a.Feed(bad, "a")
			}
			state := a.frames[udpFrameKey{"a", 1}]
			if state == nil || !state.invalid || state.bytes != 0 || len(state.chunks) != 0 {
				t.Fatalf("no tombstone: %#v", state)
			}
			if len(a.Feed(parts[1], "a")) != 0 || len(completeUDP(a, 1, "a", 2)) != 0 {
				t.Fatal("poisoned frame restarted")
			}
		})
	}
}
func TestUDPDuplicatesExpiryAndPendingLimit(t *testing.T) {
	a := NewFramedUDPAssembler(1024)
	now := time.Unix(100, 0)
	a.now = func() time.Time { return now }
	parts := udpPackets(1, testJPEG, 2)
	a.Feed(parts[0], "a")
	a.Feed(parts[0], "a")
	assertOneJPEG(t, a.Feed(parts[1], "a"))
	parts = udpPackets(2, testJPEG, 2)
	a.Feed(parts[0], "a")
	now = now.Add(4 * time.Second)
	a.Feed(parts[0], "a")
	now = now.Add(time.Second)
	if len(a.Feed(parts[1], "a")) != 0 {
		t.Fatal("expired partial retained")
	}
	for i := 0; i < 100; i++ {
		a.Feed(udpPackets(uint32(i), testJPEG, 2)[0], fmt.Sprint(i))
		if len(a.frames) > udpMaxPending {
			t.Fatal("pending limit")
		}
	}
	for i := 0; i < 100; i++ {
		completeUDP(a, uint32(i), fmt.Sprint(i), 2)
		if len(a.latest) > udpMaxPending || len(a.frames) > udpMaxPending {
			t.Fatal("sender limit")
		}
	}
	assertOneJPEG(t, completeUDP(NewFramedUDPAssembler(1024), 1, "a", 1))
}
func TestJPEGExtractor(t *testing.T) {
	e := NewJPEGExtractor(1024)
	if len(e.Feed([]byte("junk\xff"))) != 0 {
		t.Fatal("partial")
	}
	if len(e.Feed(testJPEG[1:5])) != 0 {
		t.Fatal("partial")
	}
	assertOneJPEG(t, e.Feed(testJPEG[5:]))
	double := append(append([]byte(nil), testJPEG...), testJPEG...)
	if len(e.Feed(double)) != 2 {
		t.Fatal("multiple JPEGs")
	}
	e.Feed(bytes.Repeat([]byte{'x'}, 2048))
	if len(e.buffer) > 1024 {
		t.Fatal("extractor exceeds limit")
	}
}
func TestUDPReceiverRawAndFramed(t *testing.T) {
	for _, packetIsFrame := range []bool{false, true} {
		t.Run(fmt.Sprint(packetIsFrame), func(t *testing.T) {
			c := DefaultConfig()
			c.UDPHost = "127.0.0.1"
			c.UDPPort = 0
			c.PacketIsFrame = packetIsFrame
			listener, err := ListenCameraUDP(c)
			if err != nil {
				t.Fatal(err)
			}
			defer listener.Close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			fb := NewFrameBuffer(10)
			done := make(chan error, 1)
			go func() { done <- ReceiveCameraUDP(ctx, listener, fb, c) }()
			client, err := net.DialUDP("udp4", nil, listener.LocalAddr().(*net.UDPAddr))
			if err != nil {
				t.Fatal(err)
			}
			defer client.Close()
			if _, err = client.Write(testJPEG); err != nil {
				t.Fatal(err)
			}
			frame := fb.WaitNext(ctx, 0, time.Second)
			if frame.Seq != 1 || !bytes.Equal(frame.JPEG, testJPEG) {
				t.Fatal("raw datagram")
			}
			for _, part := range udpPackets(1, testJPEG, 2) {
				client.Write(part)
			}
			frame = fb.WaitNext(ctx, 1, time.Second)
			if frame.Seq != 2 {
				t.Fatal("framed datagram")
			}
			client.Write(testJPEG[:5])
			client.Write(testJPEG[5:])
			frame = fb.WaitNext(ctx, 2, time.Second)
			if frame.Seq != 3 {
				t.Fatal("legacy fragments")
			}
			cancel()
			listener.Close()
			select {
			case err = <-done:
				if err != nil {
					t.Fatal(err)
				}
			case <-time.After(time.Second):
				t.Fatal("receiver shutdown")
			}
		})
	}
}
