package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"net"
	"time"
)

const (
	udpMaxPending = 8
	udpMaxChunks  = 4096
	udpStale      = 5 * time.Second
)

type JPEGExtractor struct {
	buffer []byte
	max    int
}

func NewJPEGExtractor(max int) *JPEGExtractor { return &JPEGExtractor{max: max} }
func (e *JPEGExtractor) Feed(chunk []byte) [][]byte {
	if len(chunk) == 0 {
		return nil
	}
	e.buffer = append(e.buffer, chunk...)
	if len(e.buffer) > e.max {
		e.buffer = append([]byte(nil), e.buffer[len(e.buffer)-e.max/2:]...)
	}
	var frames [][]byte
	for {
		start := bytes.Index(e.buffer, []byte{0xff, 0xd8})
		if start < 0 {
			if len(e.buffer) > 2 {
				e.buffer = append([]byte(nil), e.buffer[len(e.buffer)-2:]...)
			}
			break
		}
		if start > 0 {
			e.buffer = e.buffer[start:]
		}
		end := bytes.Index(e.buffer[2:], []byte{0xff, 0xd9})
		if end < 0 {
			break
		}
		end += 4
		frame := append([]byte(nil), e.buffer[:end]...)
		e.buffer = e.buffer[end:]
		if isCompleteJPEG(frame) {
			frames = append(frames, frame)
		}
	}
	return frames
}

type udpFrameKey struct {
	sender string
	id     uint32
}
type udpPending struct {
	started         time.Time
	magic           string
	length          uint32
	crc             uint32
	chunks          [][]byte
	received, bytes int
	invalid         bool
}
type udpLatest struct {
	id        uint32
	completed time.Time
}

// FramedUDPAssembler is deliberately single-owner. A socket receiver feeds it
// serially, bounding both partial frames and sequence history across senders.
type FramedUDPAssembler struct {
	maxFrameBytes int
	stale         time.Duration
	frames        map[udpFrameKey]*udpPending
	latest        map[string]udpLatest
	now           func() time.Time
}

func NewFramedUDPAssembler(max int) *FramedUDPAssembler {
	return &FramedUDPAssembler{maxFrameBytes: max, stale: udpStale, frames: make(map[udpFrameKey]*udpPending), latest: make(map[string]udpLatest), now: time.Now}
}
func isNewerFrame(id, previous uint32) bool {
	delta := id - previous
	return delta > 0 && delta < 0x80000000
}
func (a *FramedUDPAssembler) Feed(packet []byte, sender string) [][]byte {
	if len(packet) < 4 {
		return nil
	}
	magic := string(packet[:4])
	header := 18
	if magic == "ZMC2" {
		header = 22
	} else if magic != "ZMC1" {
		return nil
	}
	if len(packet) < header {
		return nil
	}
	id, length := binary.BigEndian.Uint32(packet[4:8]), binary.BigEndian.Uint32(packet[8:12])
	var crc uint32
	offset := 12
	if magic == "ZMC2" {
		crc = binary.BigEndian.Uint32(packet[12:16])
		offset = 16
	}
	index, count, payloadLen := int(binary.BigEndian.Uint16(packet[offset:])), int(binary.BigEndian.Uint16(packet[offset+2:])), int(binary.BigEndian.Uint16(packet[offset+4:]))
	payload := packet[header:]
	if payloadLen != len(payload) || payloadLen == 0 || length < 4 || uint64(length) > uint64(a.maxFrameBytes) || uint32(payloadLen) > length || count == 0 || count > udpMaxChunks || uint32(count) > length || index >= count {
		return nil
	}
	now := a.now()
	a.prune(now)
	if latest, ok := a.latest[sender]; ok && !isNewerFrame(id, latest.id) {
		return nil
	}
	key := udpFrameKey{sender, id}
	state := a.frames[key]
	if state == nil {
		state = &udpPending{started: now, magic: magic, length: length, crc: crc, chunks: make([][]byte, count)}
		a.frames[key] = state
		a.trimPending()
	}
	if state.invalid {
		return nil
	}
	if state.magic != magic || state.length != length || state.crc != crc || len(state.chunks) != count {
		state.invalidate()
		return nil
	}
	if state.chunks[index] != nil {
		if !bytes.Equal(state.chunks[index], payload) {
			state.invalidate()
		}
		return nil
	}
	newBytes := state.bytes + payloadLen
	remaining := count - state.received - 1
	if uint64(newBytes)+uint64(remaining) > uint64(length) {
		state.invalidate()
		return nil
	}
	state.chunks[index] = append([]byte(nil), payload...)
	state.received++
	state.bytes = newBytes
	if state.received != count {
		return nil
	}
	if uint64(newBytes) != uint64(length) {
		state.invalidate()
		return nil
	}
	frame := make([]byte, 0, int(length))
	for _, chunk := range state.chunks {
		frame = append(frame, chunk...)
	}
	if !isCompleteJPEG(frame) || (magic == "ZMC2" && crc32.ChecksumIEEE(frame) != crc) {
		state.invalidate()
		return nil
	}
	a.latest[sender] = udpLatest{id, now}
	for key := range a.frames {
		if key.sender == sender && !isNewerFrame(key.id, id) {
			delete(a.frames, key)
		}
	}
	for len(a.latest) > udpMaxPending {
		var oldest string
		var at time.Time
		for peer, latest := range a.latest {
			if at.IsZero() || latest.completed.Before(at) {
				oldest = peer
				at = latest.completed
			}
		}
		delete(a.latest, oldest)
	}
	return [][]byte{frame}
}
func (p *udpPending) invalidate() { p.invalid = true; p.chunks = nil; p.bytes = 0 }
func (a *FramedUDPAssembler) prune(now time.Time) {
	for sender, last := range a.latest {
		if now.Sub(last.completed) >= a.stale {
			delete(a.latest, sender)
			for key := range a.frames {
				if key.sender == sender {
					delete(a.frames, key)
				}
			}
		}
	}
	for key, state := range a.frames {
		if now.Sub(state.started) >= a.stale {
			delete(a.frames, key)
		}
	}
}
func (a *FramedUDPAssembler) trimPending() {
	for len(a.frames) > udpMaxPending {
		var oldest udpFrameKey
		var at time.Time
		for key, state := range a.frames {
			if at.IsZero() || state.started.Before(at) {
				oldest = key
				at = state.started
			}
		}
		delete(a.frames, oldest)
	}
}

func ListenCameraUDP(c Config) (*net.UDPConn, error) {
	address, err := net.ResolveUDPAddr("udp4", net.JoinHostPort(c.UDPHost, fmt.Sprint(c.UDPPort)))
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp4", address)
	if err != nil {
		return nil, err
	}
	// SO_RCVBUF is advisory; the OS may clamp it to its configured maximum.
	_ = conn.SetReadBuffer(c.UDPRcvbuf)
	return conn, nil
}

func ReceiveCameraUDP(ctx context.Context, conn *net.UDPConn, frames *FrameBuffer, c Config) error {
	assembler := NewFramedUDPAssembler(c.ExtractorMaxBytes)
	// Legacy raw fragments have no IDs. Isolate them by sender as well, so
	// simultaneous cameras cannot accidentally combine incomplete JPEGs.
	type rawState struct {
		extractor *JPEGExtractor
		last      time.Time
	}
	raw := make(map[string]*rawState)
	packet := make([]byte, 65535)
	for {
		if ctx.Err() != nil {
			return nil
		}
		if err := conn.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
			return err
		}
		n, sender, err := conn.ReadFromUDP(packet)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			if nerr, ok := err.(net.Error); ok && nerr.Timeout() {
				continue
			}
			return err
		}
		data := packet[:n]
		if n == 0 {
			continue
		}
		var complete [][]byte
		if bytes.HasPrefix(data, []byte("ZMC1")) || bytes.HasPrefix(data, []byte("ZMC2")) {
			complete = assembler.Feed(data, sender.String())
		} else if c.PacketIsFrame && isCompleteJPEG(data) {
			complete = [][]byte{data}
		} else {
			now := time.Now()
			for key, state := range raw {
				if now.Sub(state.last) >= udpStale {
					delete(raw, key)
				}
			}
			key := sender.String()
			state := raw[key]
			if state == nil {
				if len(raw) >= udpMaxPending {
					var oldest string
					var at time.Time
					for peer, state := range raw {
						if at.IsZero() || state.last.Before(at) {
							oldest = peer
							at = state.last
						}
					}
					delete(raw, oldest)
				}
				state = &rawState{extractor: NewJPEGExtractor(c.ExtractorMaxBytes)}
				raw[key] = state
			}
			state.last = now
			complete = state.extractor.Feed(data)
		}
		for _, frame := range complete {
			frames.Update(frame, "udp", nil, "frame received")
		}
	}
}
