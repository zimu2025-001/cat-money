// From server/: node --test tests/test_camera_viewer.js (Node.js 18+).
// Exercise the actual embedded viewer script without third-party DOM dependencies.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '..', 'web', 'index.html'), 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1];

function viewer() {
  const elements = new Map();
  const requests = [];
  const timers = new Map();
  const pageEvents = new Map();
  let nextTimer = 1;
  class Element {
    constructor(id, classes = '') {
      this.id = id;
      this.textContent = '';
      this.disabled = false;
      this.value = '';
      this.naturalWidth = 0;
      this.attributes = new Map();
      this.events = new Map();
      const names = new Set(classes.split(/\s+/).filter(Boolean));
      this.classList = {
        add: (...values) => values.forEach(value => names.add(value)),
        remove: (...values) => values.forEach(value => names.delete(value)),
        contains: value => names.has(value),
      };
    }
    set src(value) { this.attributes.set('src', value); requests.push({ element: this, url: value }); }
    get src() { return this.attributes.get('src'); }
    removeAttribute(name) { this.attributes.delete(name); }
    cloneNode() { return new Element(this.id); }
    replaceWith(element) { elements.set(this.id, element); }
    addEventListener(name, handler) { this.events.set(name, handler); }
    emit(name, event = {}) { this.events.get(name)?.(event); }
  }
  for (const match of source.matchAll(/<[^>]+id="([^"]+)"[^>]*>/g)) {
    const element = new Element(match[1], match[0].match(/class="([^"]*)"/)?.[1]);
    element.disabled = /\bdisabled\b/.test(match[0]);
    elements.set(element.id, element);
  }
  elements.get('cameraMode').value = source.match(/<option value="([^"]+)" selected>/)[1];
  const schedule = (fn, delay, repeat) => {
    const id = nextTimer++;
    timers.set(id, { fn, delay, repeat });
    return id;
  };
  class WebSocketMock { static OPEN = 1; readyState = 1; send() {} }
  const context = vm.createContext({
    document: { getElementById: id => elements.get(id), querySelectorAll: () => [] },
    window: { addEventListener: (name, handler) => pageEvents.set(name, handler) },
    location: { protocol: 'http:', host: 'localhost:8080' },
    WebSocket: WebSocketMock,
    fetch: () => new Promise(() => {}),
    setTimeout: (fn, delay) => schedule(fn, delay, false),
    setInterval: (fn, delay) => schedule(fn, delay, true),
    clearTimeout: id => timers.delete(id),
    clearInterval: id => timers.delete(id),
  });
  vm.runInContext(script, context);
  return {
    element: id => elements.get(id), requests, timers,
    click: id => elements.get(id).emit('click'),
    mode(value) { elements.get('cameraMode').value = value; elements.get('cameraMode').emit('change'); },
    status(camera) { context.statusData = { camera }; vm.runInContext('renderStatus(statusData)', context); },
    page(name, event) { pageEvents.get(name)(event); },
    tick(delay, repeat = false) {
      for (const [id, timer] of [...timers]) {
        if (timer.delay !== delay || timer.repeat !== repeat) continue;
        if (!timer.repeat) timers.delete(id);
        timer.fn();
      }
    },
    frame() {
      const image = elements.get('cameraStream');
      image.naturalWidth = 640;
      image.onload();
    },
  };
}

const online = { online: true, last_frame_age: 0.2, source_fps: 18.5, playback_ready: false };

test('live is default, starts before status or buffering, and repeated Play is idempotent', () => {
  const ui = viewer();
  assert.equal(ui.element('cameraMode').value, 'live');
  assert.equal(ui.requests.length, 0);
  ui.click('cameraPlayButton');
  ui.click('cameraPlayButton');
  assert.equal(ui.requests.length, 1);
  assert.match(ui.requests[0].url, /^\/mjpeg\?start=/);
  assert.equal(ui.element('cameraOverlay').classList.contains('loading'), true);
  assert.equal(ui.element('cameraOverlay').classList.contains('hidden'), false);
});

test('live does not wait for playback_ready; overlay waits for decoded image', () => {
  const ui = viewer();
  ui.status(online);
  ui.click('cameraPlayButton');
  assert.equal(ui.requests.length, 1);
  assert.equal(ui.element('cameraOverlay').classList.contains('hidden'), false);
  ui.frame();
  assert.equal(ui.element('cameraOverlay').classList.contains('hidden'), true);
  assert.equal(ui.element('cameraText').textContent, '实时观看');
  assert.equal(ui.element('cameraFpsText').textContent, '接收 18.5 FPS');
});

test('dimensions polling handles MJPEG browsers without first-frame load event', () => {
  const ui = viewer();
  ui.status(online);
  ui.click('cameraPlayButton');
  ui.element('cameraStream').naturalWidth = 640;
  ui.tick(250, true);
  assert.equal(ui.element('cameraOverlay').classList.contains('hidden'), true);
  assert.equal([...ui.timers.values()].some(timer => timer.delay === 250), false);
});

test('buffered mode alone waits for readiness and displays actual configured delay', () => {
  const ui = viewer();
  ui.mode('buffered');
  ui.status({ ...online, playback_delay: 30, start_buffer_seconds: 30, start_buffer_frames: 120, buffered_seconds: 2, buffered_frames: 50 });
  ui.click('cameraPlayButton');
  assert.equal(ui.requests.length, 0);
  assert.match(ui.element('cameraLoadText').textContent, /2\.0\/30 秒，50\/120 帧/);
  assert.equal(ui.element('cameraModeText').textContent, '缓冲 · 延迟 30 秒');
  ui.status({ ...online, playback_ready: true });
  ui.status({ ...online, playback_ready: true });
  assert.equal(ui.requests.length, 1);
  assert.match(ui.requests[0].url, /^\/mjpeg-buffered\?start=/);
});

test('switching away closes the old stream and ignores late load/error events', () => {
  const ui = viewer();
  ui.status(online);
  ui.click('cameraPlayButton');
  const old = ui.element('cameraStream');
  const lateLoad = old.onload;
  const lateError = old.onerror;
  ui.mode('buffered');
  assert.equal(old.src, undefined);
  old.naturalWidth = 640;
  lateLoad();
  lateError();
  assert.equal(ui.requests.length, 1);
  assert.equal(ui.element('cameraText').textContent, '正在缓冲');
  ui.mode('live');
  assert.equal(ui.requests.length, 2);
  lateError();
  assert.match(ui.element('cameraStream').src, /^\/mjpeg\?/);
});

test('Stop cancels a pending buffer start and allows a fresh user start', () => {
  const ui = viewer();
  ui.mode('buffered');
  ui.click('cameraPlayButton');
  ui.click('cameraStopButton');
  ui.status({ ...online, playback_ready: true });
  assert.equal(ui.requests.length, 0);
  assert.equal(ui.element('cameraPlayButton').disabled, false);
  assert.equal(ui.element('cameraStopButton').disabled, true);
  ui.click('cameraPlayButton');
  assert.equal(ui.requests.length, 1);
});

test('image failure retries once and Stop cancels scheduled retries', () => {
  const ui = viewer();
  ui.click('cameraPlayButton');
  const failed = ui.element('cameraStream');
  failed.onerror();
  assert.equal(failed.src, undefined);
  assert.equal(ui.element('cameraText').textContent, '正在重连');
  ui.status(online);
  assert.equal(ui.requests.length, 1);
  ui.tick(2000);
  assert.equal(ui.requests.length, 2);
  ui.element('cameraStream').onerror();
  ui.click('cameraStopButton');
  ui.tick(2000);
  assert.equal(ui.requests.length, 2);
  assert.equal(ui.element('cameraOverlay').classList.contains('loading'), false);
});

test('explicit reconnect replaces the stream without leaking active sources', () => {
  const ui = viewer();
  ui.click('cameraPlayButton');
  ui.click('cameraRetryButton');
  ui.click('cameraRetryButton');
  assert.equal(ui.requests.length, 3);
  assert.equal(new Set(ui.requests.map(request => request.url)).size, 3);
  assert.equal(ui.requests.filter(request => request.element.src).length, 1);
  ui.click('cameraStopButton');
  ui.click('cameraRetryButton');
  assert.equal(ui.requests.length, 3);
  assert.equal(ui.requests.filter(request => request.element.src).length, 0);
});

test('switching modes cancels an error retry and stopping blocks late first frames', () => {
  const ui = viewer();
  ui.status(online);
  ui.click('cameraPlayButton');
  ui.element('cameraStream').onerror();
  ui.mode('buffered');
  ui.tick(2000);
  assert.equal(ui.requests.length, 1);
  ui.mode('live');
  const stream = ui.element('cameraStream');
  const lateLoad = stream.onload;
  ui.click('cameraStopButton');
  stream.naturalWidth = 640;
  lateLoad();
  ui.tick(250, true);
  assert.equal(ui.element('cameraOverlay').classList.contains('hidden'), false);
  assert.equal(ui.element('cameraPlayButton').disabled, false);
  assert.equal(ui.requests.length, 2);
});

test('offline source is visibly stale and recovers without spawning another stream', () => {
  const ui = viewer();
  ui.status(online);
  ui.click('cameraPlayButton');
  ui.frame();
  ui.status({ ...online, online: false, last_frame_age: 8 });
  assert.equal(ui.element('cameraOverlay').classList.contains('hidden'), false);
  assert.equal(ui.element('cameraAgeText').textContent, '最新帧 8.0 秒前');
  assert.equal(ui.element('cameraFpsText').textContent, '接收 -- FPS');
  ui.status(online);
  assert.equal(ui.element('cameraOverlay').classList.contains('hidden'), true);
  assert.equal(ui.requests.length, 1);
});

test('page navigation disconnects, ignores in-flight status, and resumes on cached return', () => {
  const ui = viewer();
  ui.click('cameraPlayButton');
  ui.page('pagehide');
  assert.equal(ui.element('cameraStream').src, undefined);
  ui.status(online);
  assert.equal(ui.requests.length, 1);
  ui.page('pageshow', { persisted: true });
  assert.equal(ui.requests.length, 2);
  ui.click('cameraStopButton');
  ui.page('pagehide');
  ui.page('pageshow', { persisted: true });
  assert.equal(ui.requests.length, 2);
});
