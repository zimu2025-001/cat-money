package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func testServer(t *testing.T, c Config) *Server {
	t.Helper()
	serial := NewSerialBridge("", 115200, 50*time.Millisecond)
	control := NewDeviceBridge(serial, 50*time.Millisecond)
	t.Cleanup(func() { control.Close() })
	return NewServer(c, NewFrameBuffer(c.CameraBufferSeconds), control)
}
func request(t *testing.T, s *Server, method, path, body string, headers map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, path, strings.NewReader(body))
	for k, v := range headers {
		r.Header.Set(k, v)
	}
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	return w
}
func decodeResponse(t *testing.T, w *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var response map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &response); err != nil {
		t.Fatal(err, w.Body.String())
	}
	return response
}

func TestHTTPRoutesUploadAndMetadata(t *testing.T) {
	c := DefaultConfig()
	c.CameraToken = "secret"
	s := testServer(t, c)
	for _, path := range []string{"/", "/index.html"} {
		w := request(t, s, "GET", path, "", nil)
		if w.Code != 200 || !strings.Contains(w.Body.String(), "cameraMode") || w.Header().Get("Content-Type") != "text/html; charset=utf-8" {
			t.Fatal("UI route", path)
		}
	}
	if w := request(t, s, "GET", "/snapshot.jpg", "", nil); w.Code != 503 {
		t.Fatal("missing snapshot")
	}
	if w := request(t, s, "GET", "/unknown", "", nil); w.Code != 404 {
		t.Fatal("missing 404")
	}
	for _, path := range []string{"/ws", "/device"} {
		if w := request(t, s, "GET", path, "", nil); w.Code != 400 {
			t.Fatal("upgrade required")
		}
	}
	if w := request(t, s, "PUT", "/control", "", nil); w.Code != 405 {
		t.Fatal("method restriction")
	}
	headers := map[string]string{"Content-Type": "image/jpeg", "X-Camera-Token": "secret", "X-Camera-Id": "cam-42", "X-Frame-Seq": "88"}
	for _, tc := range []struct {
		name, body string
		headers    map[string]string
		status     int
	}{
		{"token", string(testJPEG), map[string]string{"Content-Type": "image/jpeg"}, 401},
		{"type", string(testJPEG), map[string]string{"X-Camera-Token": "secret", "Content-Type": "application/octet-stream"}, 400},
		{"empty", "", headers, 400}, {"JPEG", "not a JPEG", headers, 400},
	} {
		t.Run(tc.name, func(t *testing.T) {
			w := request(t, s, "POST", "/camera/frame", tc.body, tc.headers)
			if w.Code != tc.status || decodeResponse(t, w)["ok"] != false {
				t.Fatal("wrong error", w.Code, w.Body.String())
			}
		})
	}
	w := request(t, s, "POST", "/camera/frame", string(testJPEG), headers)
	if w.Code != 200 || decodeResponse(t, w)["seq"] != float64(1) {
		t.Fatal("upload", w.Body.String())
	}
	w = request(t, s, "GET", "/snapshot.jpg", "", nil)
	if w.Code != 200 || !bytes.Equal(w.Body.Bytes(), testJPEG) || w.Header().Get("Content-Type") != "image/jpeg" {
		t.Fatal("snapshot mismatch")
	}
	camera := decodeResponse(t, request(t, s, "GET", "/status.json", "", nil))["camera"].(map[string]any)
	if camera["source"] != "esp32-http" || camera["camera_id"] != "cam-42" || camera["message"] != "http frame 88" || camera["online"] != true {
		t.Fatal("metadata", camera)
	}
	s.config.CameraMaxFrameBytes = 4
	w = request(t, s, "POST", "/camera/frame", string(testJPEG), headers)
	if w.Code != 413 {
		t.Fatal("size cap", w.Code)
	}
}
func TestHTTPUploadShortAndDefaultCamera(t *testing.T) {
	s := testServer(t, DefaultConfig())
	r := httptest.NewRequest("POST", "/camera/frame", strings.NewReader(string(testJPEG)))
	r.ContentLength = int64(len(testJPEG) + 1)
	r.Header.Set("Content-Type", "image/jpeg")
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	if w.Code != 400 || decodeResponse(t, w)["error"] != "short frame body" {
		t.Fatal("short body")
	}
	w = request(t, s, "POST", "/camera/frame", string(testJPEG), map[string]string{"Content-Type": "IMAGE/JPEG; charset=binary"})
	if w.Code != 200 || s.frames.Status(10)["camera_id"] != "esp32s3-cam" {
		t.Fatal("default camera ID", w.Body.String())
	}
}
func TestHTTPControlValidation(t *testing.T) {
	s := testServer(t, DefaultConfig())
	for _, body := range []string{"", `{"cmd":`, "null", "[]", "123", `{"cmd":"not-a-command"}`, `"up" "down"`, strings.Repeat(" ", 65537) + `"up"`} {
		if w := request(t, s, "POST", "/control", body, nil); w.Code != 400 {
			t.Fatalf("malformed command accepted %q: %d", body, w.Code)
		}
	}
	for _, body := range []string{`"UP"`, `{"command":"left","step":7}`, `{"cmd":"status"}`, `{"cmd":"swing_toggle","payload":null}`} {
		w := request(t, s, "POST", "/control", body, nil)
		m := decodeResponse(t, w)
		if w.Code != 200 || m["route"] != "offline" || m["ok"] != false {
			t.Fatal("valid command failed", body, w.Body.String())
		}
	}
}
func TestStatusPlaybackReady(t *testing.T) {
	c := DefaultConfig()
	c.CameraPlaybackDelay = 5
	c.CameraStartBufferSeconds = 2
	c.CameraStartBufferFrames = 3
	c.CameraMinPlaybackRemaining = 2
	s := testServer(t, c)
	now := time.Unix(100, 0)
	s.frames.now = func() time.Time { return now }
	initial := s.StatusPayload()["camera"].(map[string]any)
	if initial["last_frame_age"] != nil || initial["playback_ready"] != false || initial["playback_buffer_remaining"] != float64(0) {
		t.Fatal(initial)
	}
	for i := 0; i < 3; i++ {
		s.frames.Update(testJPEG, "udp", nil, "frame received")
		now = now.Add(time.Second)
	}
	status := s.StatusPayload()["camera"].(map[string]any)
	if status["playback_ready"] != true || status["playback_buffer_remaining"] != float64(4) {
		t.Fatal(status)
	}
	now = now.Add(3 * time.Second)
	if s.StatusPayload()["camera"].(map[string]any)["playback_ready"] != false {
		t.Fatal("reserve exhaustion")
	}
}

func TestMJPEGLiveAndReplay(t *testing.T) {
	for _, path := range []string{"/mjpeg", "/mjpeg-buffered", "/mjpeg-replay"} {
		t.Run(path, func(t *testing.T) {
			c := DefaultConfig()
			c.CameraPlaybackDelay = 0
			c.CameraPlaybackFPS = 50
			s := testServer(t, c)
			s.frames.Update(testJPEG, "udp", nil, "frame received")
			server := httptest.NewServer(s)
			defer server.Close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			r, _ := http.NewRequestWithContext(ctx, "GET", server.URL+path, nil)
			response, err := server.Client().Do(r)
			if err != nil {
				t.Fatal(err)
			}
			defer response.Body.Close()
			if response.StatusCode != 200 || response.Header.Get("Content-Type") != "multipart/x-mixed-replace; boundary=frame" {
				t.Fatal("stream headers")
			}
			reader := multipart.NewReader(response.Body, "frame")
			for i := 0; i < 2; i++ {
				part, err := reader.NextPart()
				if err != nil {
					t.Fatal(err)
				}
				body := make([]byte, len(testJPEG))
				if _, err := io.ReadFull(part, body); err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(body, testJPEG) || part.Header.Get("Content-Type") != "image/jpeg" {
					t.Fatal("stream bytes")
				}
				if i == 0 {
					s.frames.Update(testJPEG, "udp", nil, "frame received")
				}
			}
			cancel()
		})
	}
}

// A client may disconnect before the first frame or while replay sleeps ten
// seconds. Request cancellation must release the handler in both cases.
func TestMJPEGIdleDisconnect(t *testing.T) {
	for _, path := range []string{"/mjpeg", "/mjpeg-buffered", "/mjpeg-replay"} {
		t.Run(path, func(t *testing.T) {
			c := DefaultConfig()
			c.CameraPlaybackFPS = 0.1
			s := testServer(t, c)
			finished := make(chan struct{})
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { s.ServeHTTP(w, r); close(finished) }))
			defer server.Close()
			response, err := server.Client().Get(server.URL + path)
			if err != nil {
				t.Fatal(err)
			}
			response.Body.Close()
			select {
			case <-finished:
			case <-time.After(time.Second):
				t.Fatal("idle MJPEG handler leaked")
			}
		})
	}
}

func TestMJPEGStreamWritesExactBoundary(t *testing.T) {
	s := testServer(t, DefaultConfig())
	s.frames.Update(testJPEG, "udp", nil, "frame received")
	server := httptest.NewServer(s)
	defer server.Close()
	response, err := server.Client().Get(server.URL + "/mjpeg")
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	reader := bufio.NewReader(response.Body)
	for _, want := range []string{"--frame\r\n", "Content-Type: image/jpeg\r\n", fmt.Sprintf("Content-Length: %d\r\n", len(testJPEG)), "\r\n"} {
		got, err := reader.ReadString('\n')
		if err != nil || got != want {
			t.Fatalf("boundary got %q err %v want %q", got, err, want)
		}
	}
	body := make([]byte, len(testJPEG)+2)
	if _, err := io.ReadFull(reader, body); err != nil || !bytes.Equal(body, append(append([]byte(nil), testJPEG...), '\r', '\n')) {
		t.Fatal("JPEG framing")
	}
}

func TestEarlyUploadRejectionDoesNotWaitForBody(t *testing.T) {
	c := DefaultConfig()
	c.CameraToken = "required"
	s := testServer(t, c)
	server := httptest.NewServer(s)
	defer server.Close()
	for _, headers := range []string{"Content-Type: image/jpeg\r\n", "X-Camera-Token: required\r\nContent-Type: text/plain\r\n"} {
		conn, err := net.Dial("tcp", server.Listener.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		conn.SetDeadline(time.Now().Add(time.Second))
		fmt.Fprintf(conn, "POST /camera/frame HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n%s\r\n", headers)
		response, err := http.ReadResponse(bufio.NewReader(conn), nil)
		if err != nil {
			conn.Close()
			t.Fatal("early error waited for missing request body:", err)
		}
		if response.StatusCode != 401 && response.StatusCode != 400 {
			t.Fatal("wrong reject", response.StatusCode)
		}
		response.Body.Close()
		conn.Close()
	}
}

func TestSuccessfulUploadKeepsConnectionUsableForStreaming(t *testing.T) {
	s := testServer(t, DefaultConfig())
	server := httptest.NewServer(s)
	defer server.Close()
	for attempt := 0; attempt < 12; attempt++ {
		conn, err := net.Dial("tcp", server.Listener.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		conn.SetDeadline(time.Now().Add(3 * time.Second))
		reader := bufio.NewReader(conn)
		for i := 0; i < 30; i++ {
			fmt.Fprintf(conn, "POST /camera/frame HTTP/1.1\r\nHost: localhost\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len(testJPEG))
			conn.Write(testJPEG)
			response, err := http.ReadResponse(reader, nil)
			if err != nil {
				conn.Close()
				t.Fatal(err)
			}
			io.Copy(io.Discard, response.Body)
			response.Body.Close()
			if response.StatusCode != 200 {
				t.Fatal(response.Status)
			}
		}
		fmt.Fprint(conn, "GET /mjpeg HTTP/1.1\r\nHost: localhost\r\n\r\n")
		response, err := http.ReadResponse(reader, nil)
		if err != nil {
			conn.Close()
			t.Fatal(err)
		}
		part, err := multipart.NewReader(response.Body, "frame").NextPart()
		if err != nil {
			conn.Close()
			t.Fatal("keep-alive stream context canceled:", err)
		}
		jpeg := make([]byte, len(testJPEG))
		if _, err = io.ReadFull(part, jpeg); err != nil || !bytes.Equal(jpeg, testJPEG) {
			conn.Close()
			t.Fatal("missing keep-alive frame", err)
		}
		conn.Close()
	}
}
