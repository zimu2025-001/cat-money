package main

import (
	"crypto/subtle"
	_ "embed"
	"encoding/json"
	"fmt"
	"html"
	"io"
	"math"
	"net/http"
	"strings"
	"time"
)

//go:embed web/index.html
var indexHTML string

type Server struct {
	config  Config
	frames  *FrameBuffer
	control *DeviceBridge
}

func NewServer(config Config, frames *FrameBuffer, control *DeviceBridge) *Server {
	return &Server{config, frames, control}
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Server", "ESP32MJPEG/3.0 (Go)")
	path := r.URL.Path
	if r.Method == http.MethodGet {
		switch {
		case strings.HasPrefix(path, "/ws"):
			s.control.BrowserWebSocket(w, r, s.config.ControlStep, s.StatusPayload)
		case strings.HasPrefix(path, "/device"):
			s.control.DeviceWebSocket(w, r)
		case path == "/" || path == "/index.html":
			body := strings.ReplaceAll(indexHTML, "__HOST__", html.EscapeString(r.Host))
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			w.Header().Set("Content-Length", fmt.Sprint(len(body)))
			w.Header().Set("Cache-Control", "no-store")
			_, _ = io.WriteString(w, body)
		case path == "/snapshot.jpg":
			s.snapshot(w, r)
		case path == "/status.json":
			sendJSON(w, http.StatusOK, s.StatusPayload())
		case strings.HasPrefix(path, "/mjpeg-buffered") || strings.HasPrefix(path, "/mjpeg-replay"):
			s.streamMJPEG(w, r, true)
		case strings.HasPrefix(path, "/mjpeg"):
			s.streamMJPEG(w, r, false)
		default:
			http.NotFound(w, r)
		}
		return
	}
	if r.Method == http.MethodPost {
		switch path {
		case "/camera/frame":
			s.uploadFrame(w, r)
		case "/control":
			s.controlHTTP(w, r)
		default:
			http.NotFound(w, r)
		}
		return
	}
	w.Header().Set("Allow", "GET, POST")
	http.Error(w, "Method Not Allowed", http.StatusMethodNotAllowed)
}

func sendJSON(w http.ResponseWriter, status int, payload map[string]any) {
	body, err := json.Marshal(payload)
	if err != nil {
		http.Error(w, "JSON encoding failed", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Content-Length", fmt.Sprint(len(body)))
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_, _ = w.Write(body)
}
func jsonError(w http.ResponseWriter, status int, message string) {
	sendJSON(w, status, map[string]any{"ok": false, "error": message})
}

// Close unread request bodies before clearing their read deadline. net/http may
// otherwise try to drain a small advertised body forever after an early reject.
func requestBodyDeadline(w http.ResponseWriter, r *http.Request) func() {
	controller := http.NewResponseController(w)
	_ = controller.SetReadDeadline(time.Now().Add(15 * time.Second))
	return func() {
		if strings.EqualFold(w.Header().Get("Connection"), "close") {
			_ = controller.SetReadDeadline(time.Now())
		}
		_ = r.Body.Close()
		_ = controller.SetReadDeadline(time.Time{})
	}
}

func bodyError(w http.ResponseWriter, status int, message string) {
	// Do not wait for a rejected upload merely to reuse its connection.
	w.Header().Set("Connection", "close")
	jsonError(w, status, message)
}

func (s *Server) controlHTTP(w http.ResponseWriter, r *http.Request) {
	defer requestBodyDeadline(w, r)()
	r.Body = http.MaxBytesReader(w, r.Body, 64*1024)
	decoder := json.NewDecoder(r.Body)
	decoder.UseNumber()
	var message any
	if err := decoder.Decode(&message); err != nil {
		bodyError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		bodyError(w, http.StatusBadRequest, "expected one JSON command")
		return
	}
	cmd, payload, err := NormalizeControlMessage(message, s.config.ControlStep)
	if err != nil {
		jsonError(w, http.StatusBadRequest, err.Error())
		return
	}
	sendJSON(w, http.StatusOK, s.control.SendCommand(cmd, payload))
}

func (s *Server) uploadFrame(w http.ResponseWriter, r *http.Request) {
	defer requestBodyDeadline(w, r)()
	if s.config.CameraToken != "" && subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Camera-Token")), []byte(s.config.CameraToken)) != 1 {
		bodyError(w, http.StatusUnauthorized, "invalid camera token")
		return
	}
	if strings.ToLower(strings.TrimSpace(strings.SplitN(r.Header.Get("Content-Type"), ";", 2)[0])) != "image/jpeg" {
		bodyError(w, http.StatusBadRequest, "Content-Type must be image/jpeg")
		return
	}
	if r.ContentLength <= 0 {
		bodyError(w, http.StatusBadRequest, "empty frame")
		return
	}
	if r.ContentLength > int64(s.config.CameraMaxFrameBytes) {
		bodyError(w, http.StatusRequestEntityTooLarge, "frame too large")
		return
	}
	frame, err := io.ReadAll(io.LimitReader(r.Body, r.ContentLength+1))
	if err != nil || int64(len(frame)) != r.ContentLength {
		bodyError(w, http.StatusBadRequest, "short frame body")
		return
	}
	if !isCompleteJPEG(frame) {
		jsonError(w, http.StatusBadRequest, "invalid jpeg frame")
		return
	}
	cameraID := r.Header.Get("X-Camera-Id")
	if cameraID == "" {
		cameraID = "esp32s3-cam"
	}
	seq := s.frames.Update(frame, "esp32-http", cameraID, strings.TrimSpace("http frame "+r.Header.Get("X-Frame-Seq")))
	sendJSON(w, http.StatusOK, map[string]any{"ok": true, "seq": seq})
}

func (s *Server) snapshot(w http.ResponseWriter, r *http.Request) {
	frame := s.frames.Latest()
	if len(frame.JPEG) == 0 {
		http.Error(w, "No frame yet", http.StatusServiceUnavailable)
		return
	}
	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("Content-Length", fmt.Sprint(len(frame.JPEG)))
	w.Header().Set("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
	_, _ = w.Write(frame.JPEG)
}

func (s *Server) StatusPayload() map[string]any {
	camera := s.frames.Status(s.config.CameraOfflineAfter)
	remaining := 0.0
	age, hasAge := camera["last_frame_age"].(float64)
	if hasAge {
		remaining = math.Max(0, s.config.CameraPlaybackDelay-age)
	}
	camera["playback_delay"] = s.config.CameraPlaybackDelay
	camera["playback_fps"] = s.config.CameraPlaybackFPS
	camera["start_buffer_seconds"] = s.config.CameraStartBufferSeconds
	camera["start_buffer_frames"] = s.config.CameraStartBufferFrames
	camera["min_playback_remaining"] = s.config.CameraMinPlaybackRemaining
	camera["playback_buffer_remaining"] = remaining
	camera["playback_ready"] = hasAge && camera["buffered_seconds"].(float64) >= s.config.CameraStartBufferSeconds && camera["buffered_frames"].(int) >= s.config.CameraStartBufferFrames && remaining >= s.config.CameraMinPlaybackRemaining
	return map[string]any{"camera": camera, "control": s.control.Status()}
}

func (s *Server) streamMJPEG(w http.ResponseWriter, r *http.Request, replay bool) {
	controller := http.NewResponseController(w)
	defer controller.SetWriteDeadline(time.Time{})
	_ = controller.SetWriteDeadline(time.Now().Add(5 * time.Second))
	w.Header().Set("Age", "0")
	w.Header().Set("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
	w.Header().Set("Pragma", "no-cache")
	w.Header().Set("Content-Type", "multipart/x-mixed-replace; boundary=frame")
	w.Header().Set("X-Accel-Buffering", "no")
	if err := controller.Flush(); err != nil {
		return
	}
	write := func(jpeg []byte) error {
		if err := controller.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil && err != http.ErrNotSupported {
			return err
		}
		if _, err := fmt.Fprintf(w, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len(jpeg)); err != nil {
			return err
		}
		if _, err := w.Write(jpeg); err != nil {
			return err
		}
		if _, err := io.WriteString(w, "\r\n"); err != nil {
			return err
		}
		return controller.Flush()
	}
	lastSeq := int64(-1)
	if !replay {
		for {
			if r.Context().Err() != nil {
				return
			}
			frame := s.frames.WaitNext(r.Context(), lastSeq, time.Second)
			if len(frame.JPEG) == 0 || frame.Seq == lastSeq {
				continue
			}
			if err := write(frame.JPEG); err != nil {
				return
			}
			lastSeq = frame.Seq
		}
	}
	interval := time.Duration(float64(time.Second) / math.Max(0.1, s.config.CameraPlaybackFPS))
	if interval < time.Millisecond {
		interval = time.Millisecond
	}
	var lastJPEG []byte
	for {
		if r.Context().Err() != nil {
			return
		}
		frame := s.frames.Replay(lastSeq, time.Now().Add(-time.Duration(s.config.CameraPlaybackDelay*float64(time.Second))))
		if len(frame.JPEG) > 0 && frame.Seq != lastSeq {
			lastSeq = frame.Seq
			lastJPEG = frame.JPEG
		}
		if len(lastJPEG) > 0 {
			if err := write(lastJPEG); err != nil {
				return
			}
		}
		timer := time.NewTimer(interval)
		select {
		case <-r.Context().Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}
