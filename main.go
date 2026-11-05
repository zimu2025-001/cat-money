package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return
		}
		log.Print(err)
		os.Exit(1)
	}
}
func run(args []string) error {
	c, err := ParseConfig(args, os.Stderr)
	if err != nil {
		return err
	}
	listener, err := net.Listen("tcp", net.JoinHostPort(c.HTTPHost, fmt.Sprint(c.HTTPPort)))
	if err != nil {
		return fmt.Errorf("HTTP listen: %w", err)
	}
	defer listener.Close()
	var udp *net.UDPConn
	if !c.DisableUDP {
		udp, err = ListenCameraUDP(c)
		if err != nil {
			return fmt.Errorf("UDP listen: %w", err)
		}
		defer udp.Close()
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	timeout := time.Duration(c.ControlTimeout * float64(time.Second))
	serial := NewSerialBridge(c.SerialPort, c.SerialBaud, timeout)
	if err := serial.Start(); err != nil {
		log.Printf("[UART] %v", err)
	}
	control := NewDeviceBridge(serial, timeout)
	defer control.Close()
	frames := NewFrameBuffer(c.CameraBufferSeconds)
	handler := NewServer(c, frames, control)
	server := &http.Server{Handler: handler, ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 64 * 1024, BaseContext: func(net.Listener) context.Context { return ctx }}
	results := make(chan error, 2)
	if udp != nil {
		go func() { results <- ReceiveCameraUDP(ctx, udp, frames, c) }()
	}
	go func() { results <- server.Serve(listener) }()
	log.Printf("[HTTP] http://%s/ (Live: /mjpeg, Buffered: /mjpeg-buffered, Snapshot: /snapshot.jpg, Browser WS: /ws, Device WS: /device)", listener.Addr())
	log.Printf("[CAM ] delay=%.1fs fps=%.1f buffer=%.1fs start_after=%.1fs/%df min_remaining=%.1fs", c.CameraPlaybackDelay, c.CameraPlaybackFPS, c.CameraBufferSeconds, c.CameraStartBufferSeconds, c.CameraStartBufferFrames, c.CameraMinPlaybackRemaining)
	if udp != nil {
		log.Printf("[UDP ] listening on %s (packet_is_frame=%v)", udp.LocalAddr(), c.PacketIsFrame)
	} else {
		log.Print("[UDP ] ESP32-CAM UDP receiver disabled")
	}
	log.Printf("[UART] %v", serial.Status()["message"])
	select {
	case <-ctx.Done():
	case err = <-results:
		stop()
	}
	// Cancel streaming handlers, close hijacked WebSockets, then drain HTTP.
	stop()
	_ = control.Close()
	if udp != nil {
		_ = udp.Close()
	}
	shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if shutdownErr := server.Shutdown(shutdown); shutdownErr != nil {
		_ = server.Close()
		if err == nil {
			err = shutdownErr
		}
	}
	if errors.Is(err, http.ErrServerClosed) || errors.Is(err, net.ErrClosed) {
		return nil
	}
	return err
}
