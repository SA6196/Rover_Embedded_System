// Rover: WiFi keyboard control + ultrasonic radar + GPS + MPU6050
// No extra libraries needed.
//
// WiFi: connect to "Rover" (password rover1234), open http://192.168.4.1
// Keys: W S A D drive, R spin, Space stop (double-tap = fast)
//       Q / E pan the ultrasonic left / right, C center, T sweep on/off,
//       X zero the heading
//
// Motors : L RPWM 25, L LPWM 26, R RPWM 27, R LPWM 14
// Servo  : signal GPIO 13      Ultrasonic: TRIG 18, ECHO 19 (through divider)
// GPS    : GPS TX -> GPIO 16, GPS RX -> GPIO 17
// MPU6050: SDA 21, SCL 22

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <math.h>

// ---------- Settings you can change ----------
const char* AP_SSID = "Rover";
const char* AP_PASS = "rover1234";

const int NORMAL_SPEED = 140;      // 0 to 255
const int FAST_SPEED   = 200;      // after a double-tap
const int LEFT_DIR  = 1;           // -1 if a side goes the wrong way
const int RIGHT_DIR = 1;
const bool SWAP_SIDES = false;     // true if D turns left

const int OBSTACLE_STOP_CM = 20;   // forward motion stops closer than this. 0 = off
const int SWEEP_MIN = 20;          // sweep range of the servo (degrees)
const int SWEEP_MAX = 160;
const bool SERVO_REVERSED = false; // true if left and right look swapped on the radar
// ---------------------------------------------

const int L_RPWM = 25, L_LPWM = 26, R_RPWM = 27, R_LPWM = 14;
const int LED_PIN = 2;
const int SERVO_PIN = 13, TRIG_PIN = 18, ECHO_PIN = 19;
const int GPS_RX = 16, GPS_TX = 17;
const int SDA_PIN = 21, SCL_PIN = 22;

const int PWM_FREQ = 5000;
const int PWM_BITS = 8;
const int pins[4] = {L_RPWM, L_LPWM, R_RPWM, R_LPWM};
const int SERVO_CH = 4;            // used only by older ESP32 core versions

WebServer server(80);

// ---------- Drive state ----------
unsigned long lastCmd = 0;
bool moving = false;
int cmdL = 0, cmdR = 0, cmdSp = 0;
bool blockedNow = false;

// ---------- Ultrasonic state ----------
int servoAngle = 90;
int scanDir = 1;
bool sweepOn = true;
unsigned long lastScan = 0;
int distNow = 0;
int frontDist = 0;
unsigned long frontTime = 0;
int radar[19];

// ---------- MPU state ----------
const uint8_t MPU_ADDR = 0x68;
bool mpuOk = false;
float pitchDeg = 0, rollDeg = 0, yawDeg = 0, gyroBiasZ = 0;
unsigned long lastMpu = 0;

// ---------- GPS state ----------
char gpsBuf[100];
int gpsLen = 0;
double gpsLat = 0, gpsLon = 0;
float gpsAlt = 0, gpsSpeed = 0;
int gpsSats = 0, gpsFix = 0;
unsigned long lastGpsByte = 0;

// =============== Motors ===============
void setupPwm() {
  for (int i = 0; i < 4; i++) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(pins[i], PWM_FREQ, PWM_BITS);
#else
    ledcSetup(i, PWM_FREQ, PWM_BITS);
    ledcAttachPin(pins[i], i);
#endif
  }
}

void pwmWrite(int i, int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pins[i], duty);
#else
  ledcWrite(i, duty);
#endif
}

void setLeft(int speed) {
  speed *= LEFT_DIR;
  if (speed >= 0) { pwmWrite(0, speed); pwmWrite(1, 0); }
  else            { pwmWrite(0, 0);     pwmWrite(1, -speed); }
}

void setRight(int speed) {
  speed *= RIGHT_DIR;
  if (speed >= 0) { pwmWrite(2, speed); pwmWrite(3, 0); }
  else            { pwmWrite(2, 0);     pwmWrite(3, -speed); }
}

void stopAll() { setLeft(0); setRight(0); }

// Applies the current command, unless an obstacle blocks forward motion
void applyMotors() {
  bool forward = (cmdL >= 0 && cmdR >= 0 && (cmdL > 0 || cmdR > 0));
  bool close = (OBSTACLE_STOP_CM > 0 && frontDist > 0 && frontDist < OBSTACLE_STOP_CM &&
                millis() - frontTime < 1500);
  blockedNow = forward && close;
  if (blockedNow) stopAll();
  else { setLeft(cmdL * cmdSp); setRight(cmdR * cmdSp); }
}

// =============== Servo and ultrasonic ===============
void servoSetup() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(SERVO_PIN, 50, 14);
#else
  ledcSetup(SERVO_CH, 50, 14);
  ledcAttachPin(SERVO_PIN, SERVO_CH);
#endif
}

void servoWrite(int angle) {
  angle = constrain(angle, 0, 180);
  if (SERVO_REVERSED) angle = 180 - angle;
  uint32_t us = 500 + (uint32_t)angle * 1900UL / 180UL;
  uint32_t duty = us * 16383UL / 20000UL;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(SERVO_PIN, duty);
#else
  ledcWrite(SERVO_CH, duty);
#endif
}

int readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, 25000);
  if (us == 0) return 0;                       // 0 = no echo
  int cm = (int)(us * 0.0343f / 2.0f);
  return (cm < 2 || cm > 400) ? 0 : cm;
}

void scanStep() {
  unsigned long now = millis();
  if (now - lastScan < 90) return;
  lastScan = now;

  int d = readDistanceCm();                    // servo has had 90 ms to arrive
  distNow = d;
  radar[servoAngle / 10] = d;
  if (abs(servoAngle - 90) <= 20) { frontDist = d; frontTime = now; }

  if (sweepOn) {
    servoAngle += 10 * scanDir;
    if (servoAngle >= SWEEP_MAX) { servoAngle = SWEEP_MAX; scanDir = -1; }
    if (servoAngle <= SWEEP_MIN) { servoAngle = SWEEP_MIN; scanDir = 1; }
    servoWrite(servoAngle);
  }
  if (moving) applyMotors();
}

// =============== MPU6050 ===============
bool mpuRead(float &ax, float &ay, float &az, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDR, 14) != 14) return false;
  int16_t v[7];
  for (int i = 0; i < 7; i++) {
    int hi = Wire.read();
    int lo = Wire.read();
    v[i] = (int16_t)((hi << 8) | lo);
  }
  ax = v[0] / 16384.0f;
  ay = v[1] / 16384.0f;
  az = v[2] / 16384.0f;
  gz = v[6] / 131.0f;
  return true;
}

bool mpuBegin() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);                            // wake up
  if (Wire.endTransmission() != 0) return false;
  delay(100);
  float sum = 0; int n = 0;                    // keep the rover still for this
  for (int i = 0; i < 100; i++) {
    float ax, ay, az, gz;
    if (mpuRead(ax, ay, az, gz)) { sum += gz; n++; }
    delay(5);
  }
  gyroBiasZ = n ? sum / n : 0;
  lastMpu = micros();
  return true;
}

void mpuUpdate() {
  if (!mpuOk) return;
  unsigned long now = micros();
  if (now - lastMpu < 20000) return;
  float dt = (now - lastMpu) / 1000000.0f;
  lastMpu = now;
  float ax, ay, az, gz;
  if (!mpuRead(ax, ay, az, gz)) return;
  float roll = atan2f(ay, az) * 57.2958f;
  float pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f;
  rollDeg = 0.85f * rollDeg + 0.15f * roll;
  pitchDeg = 0.85f * pitchDeg + 0.15f * pitch;
  float g = gz - gyroBiasZ;
  if (fabsf(g) < 0.3f) g = 0;
  yawDeg -= g * dt;                            // clockwise = positive
  yawDeg = fmodf(yawDeg, 360.0f);
  if (yawDeg < 0) yawDeg += 360.0f;
}

// =============== GPS (reads NMEA text itself) ===============
double nmeaToDeg(const char* s, const char* hemi) {
  if (!s || !*s) return 0;
  double v = atof(s);
  int deg = (int)(v / 100);
  double d = deg + (v - deg * 100) / 60.0;
  if (hemi && (hemi[0] == 'S' || hemi[0] == 'W')) d = -d;
  return d;
}

void gpsParse(char* line) {
  char* star = strchr(line, '*');
  if (line[0] != '$' || !star) return;
  uint8_t cs = 0;
  for (char* p = line + 1; p < star; p++) cs ^= *p;
  if ((uint8_t)strtol(star + 1, NULL, 16) != cs) return;     // bad checksum
  *star = 0;

  char* f[20]; int n = 0;
  f[n++] = line;
  for (char* p = line; *p && n < 20; p++) {
    if (*p == ',') { *p = 0; f[n++] = p + 1; }
  }
  if (strlen(f[0]) < 6) return;

  if (strcmp(f[0] + 3, "GGA") == 0 && n >= 10) {
    gpsFix = atoi(f[6]);
    gpsSats = atoi(f[7]);
    gpsAlt = atof(f[9]);
    if (gpsFix > 0) {
      gpsLat = nmeaToDeg(f[2], f[3]);
      gpsLon = nmeaToDeg(f[4], f[5]);
    }
  } else if (strcmp(f[0] + 3, "RMC") == 0 && n >= 8) {
    gpsSpeed = (f[2][0] == 'A') ? atof(f[7]) * 1.852f : 0;    // knots to km/h
  }
}

void gpsFeed() {
  while (Serial2.available()) {
    char c = Serial2.read();
    lastGpsByte = millis();
    if (c == '$') gpsLen = 0;
    if (gpsLen < (int)sizeof(gpsBuf) - 1) gpsBuf[gpsLen++] = c;
    if (c == '\n') {
      gpsBuf[gpsLen] = 0;
      gpsParse(gpsBuf);
      gpsLen = 0;
    }
  }
}

// =============== Web page ===============
const char PAGE[] = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Rover</title>
<style>
body{font-family:Arial,sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:16px}
.ok{color:#4caf50}.bad{color:#f44336}.fast{color:#ff9800}
#mode{font-size:34px;font-weight:bold;margin:10px}
#warn{font-size:22px;font-weight:bold;color:#f44336;min-height:28px}
.card{background:#1c1c1c;border:1px solid #333;border-radius:10px;max-width:420px;margin:12px auto;padding:12px}
.card h2{margin:0 0 8px;font-size:18px;color:#4fc3f7}
.big{font-size:36px;font-weight:bold}
button{background:#333;color:#eee;border:1px solid #555;border-radius:6px;padding:8px 12px;margin:3px;font-size:15px}
table{margin:8px auto;border-collapse:collapse;font-size:14px}
td{border:1px solid #444;padding:4px 10px;text-align:left}
.k{font-weight:bold;color:#4fc3f7}
canvas{background:#0a0a0a;border-radius:8px;max-width:100%}
</style></head><body>
<h1>Rover</h1>
<div id="st" class="bad">Connecting...</div>
<div id="mode">STOPPED</div>
<div id="warn"></div>

<div class="card"><h2>Ultrasonic radar</h2>
<div id="dist" class="big">--</div>
<canvas id="radar" width="360" height="200"></canvas>
<div>
<button onclick="u('l')">&#9664; Pan left (Q)</button>
<button onclick="u('c')">Center (C)</button>
<button onclick="u('r')">Pan right (E) &#9654;</button><br>
<button onclick="u('t')">Sweep on / off (T)</button>
</div></div>

<div class="card"><h2>Orientation (MPU6050)</h2><div id="mpu">--</div>
<button onclick="u('z')">Zero heading (X)</button></div>

<div class="card"><h2>GPS</h2><div id="gps">--</div></div>

<div class="card"><h2>Keys</h2>
<table>
<tr><td class="k">W / S</td><td>Forward / backward</td></tr>
<tr><td class="k">A / D</td><td>Turn left / right</td></tr>
<tr><td class="k">R</td><td>Spin</td></tr>
<tr><td class="k">Space</td><td>Stop</td></tr>
<tr><td colspan="2">Double-tap a key and hold it for FAST</td></tr>
</table></div>

<script>
const $ = id => document.getElementById(id);
const keys = new Set(), fastKeys = new Set(), lastDown = {};
const MOVE = ['w','a','s','d','r'];
const UIKEY = {q:'l', e:'r', c:'c', t:'t', x:'z'};
let busy = false, lastMoving = false;

function state() {
  const t = (keys.has('w') ? 1 : 0) - (keys.has('s') ? 1 : 0);
  const n = (keys.has('d') ? 1 : 0) - (keys.has('a') ? 1 : 0);
  let l, r;
  if (keys.has('r')) { l = 1; r = -1; }
  else {
    l = Math.max(-1, Math.min(1, t + n));
    r = Math.max(-1, Math.min(1, t - n));
  }
  let f = 0;
  for (const k of keys) if (fastKeys.has(k)) f = 1;
  return {l: l, r: r, f: f};
}

function setConn(ok) {
  $('st').textContent = ok ? 'Connected' : 'Not connected';
  $('st').className = ok ? 'ok' : 'bad';
}

function send(l, r, f, force) {
  if (busy && !force) return;
  busy = true;
  const c = new AbortController();
  const to = setTimeout(() => c.abort(), 400);
  fetch('/c?l=' + l + '&r=' + r + '&f=' + f, {signal: c.signal, cache: 'no-store'})
    .then(() => setConn(true))
    .catch(() => setConn(false))
    .finally(() => { clearTimeout(to); busy = false; });
}

function u(c) { fetch('/u?c=' + c, {cache: 'no-store'}).catch(() => {}); }

function update() {
  const s = state();
  const moving = (s.l !== 0 || s.r !== 0);
  if (moving) {
    $('mode').textContent = s.f ? 'FAST' : 'NORMAL';
    $('mode').className = s.f ? 'fast' : '';
    send(s.l, s.r, s.f, false);
  } else {
    $('mode').textContent = 'STOPPED';
    $('mode').className = '';
    if (lastMoving) send(0, 0, 0, true);
  }
  lastMoving = moving;
}

document.addEventListener('keydown', e => {
  const k = (e.key === ' ') ? ' ' : e.key.toLowerCase();
  if (k === ' ') { keys.clear(); fastKeys.clear(); update(); e.preventDefault(); return; }
  if (UIKEY[k]) { e.preventDefault(); if (!e.repeat) u(UIKEY[k]); return; }
  if (!MOVE.includes(k)) return;
  e.preventDefault();
  if (e.repeat) return;
  const now = Date.now();
  if (lastDown[k] && now - lastDown[k] < 350) fastKeys.add(k); else fastKeys.delete(k);
  lastDown[k] = now;
  keys.add(k);
  update();
});
document.addEventListener('keyup', e => {
  const k = e.key.toLowerCase();
  keys.delete(k); fastKeys.delete(k); update();
});
window.addEventListener('blur', () => { keys.clear(); fastKeys.clear(); update(); });
setInterval(update, 100);
setInterval(() => { if (!lastMoving) send(0, 0, 0, false); }, 2000);

function drawRadar(radar, ang) {
  const c = $('radar'), x = c.getContext('2d');
  const W = c.width, H = c.height, cx = W / 2, cy = H - 10, R = H - 30;
  x.clearRect(0, 0, W, H);
  x.strokeStyle = '#333'; x.fillStyle = '#777'; x.lineWidth = 1; x.font = '11px Arial';
  for (let i = 1; i <= 4; i++) {
    x.beginPath(); x.arc(cx, cy, R * i / 4, Math.PI, 2 * Math.PI); x.stroke();
    x.fillText((i * 50) + ' cm', cx + 4, cy - R * i / 4 + 12);
  }
  const a = ang * Math.PI / 180;
  x.strokeStyle = '#4fc3f7';
  x.beginPath(); x.moveTo(cx, cy); x.lineTo(cx + R * Math.cos(a), cy - R * Math.sin(a)); x.stroke();
  for (let i = 0; i < radar.length; i++) {
    const d = radar[i]; if (d <= 0) continue;
    const th = i * 10 * Math.PI / 180, r = Math.min(d, 200) / 200 * R;
    x.fillStyle = d < 30 ? '#f44336' : '#4caf50';
    x.beginPath(); x.arc(cx + r * Math.cos(th), cy - r * Math.sin(th), 5, 0, 2 * Math.PI); x.fill();
  }
}

function poll() {
  fetch('/s', {cache: 'no-store'}).then(r => r.json()).then(s => {
    setConn(true);
    $('dist').textContent = s.d > 0 ? s.d + ' cm' : 'no echo';
    drawRadar(s.rd, s.a);
    $('warn').textContent = s.bl ? 'OBSTACLE - forward blocked' : (s.ti ? 'ROVER TILTED' : '');
    $('mpu').innerHTML = s.mo
      ? 'Pitch ' + s.pi + '&deg; &middot; Roll ' + s.ro + '&deg;<br>Heading change ' + s.ya + '&deg;'
      : 'MPU6050 not found - check wiring, then press EN';
    if (!s.go) $('gps').textContent = 'No GPS data - check wiring';
    else if (!s.fx) $('gps').textContent = 'Searching for satellites... (' + s.sa + ' seen). Go outdoors.';
    else $('gps').innerHTML = 'Fix OK &middot; ' + s.sa + ' satellites<br>' + s.la + ', ' + s.lo +
      '<br>Speed ' + s.sp + ' km/h &middot; Altitude ' + s.al + ' m';
  }).catch(() => setConn(false));
}
setInterval(poll, 300);
</script></body></html>
)rawliteral";

// =============== Web handlers ===============
void handleRoot() { server.send(200, "text/html", PAGE); }

void handleCmd() {
  int l = constrain(server.arg("l").toInt(), -1, 1);
  int r = constrain(server.arg("r").toInt(), -1, 1);
  if (SWAP_SIDES) { int t = l; l = r; r = t; }
  cmdL = l;
  cmdR = r;
  cmdSp = (server.arg("f") == "1") ? FAST_SPEED : NORMAL_SPEED;
  moving = (l != 0 || r != 0);
  lastCmd = millis();
  applyMotors();
  server.send(200, "text/plain", "ok");
}

void handleUi() {
  String c = server.arg("c");
  if (c == "t") sweepOn = !sweepOn;
  else if (c == "l") { sweepOn = false; servoAngle = constrain(servoAngle + 10, 0, 180); servoWrite(servoAngle); }
  else if (c == "r") { sweepOn = false; servoAngle = constrain(servoAngle - 10, 0, 180); servoWrite(servoAngle); }
  else if (c == "c") { sweepOn = false; servoAngle = 90; servoWrite(90); }
  else if (c == "z") yawDeg = 0;
  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  bool tilt = (fabsf(rollDeg) > 45 || fabsf(pitchDeg) > 45);
  bool gpsOk = (lastGpsByte != 0 && millis() - lastGpsByte < 3000);
  String j;
  j.reserve(420);
  j += "{\"d\":";  j += distNow;
  j += ",\"a\":";  j += servoAngle;
  j += ",\"bl\":"; j += (blockedNow ? 1 : 0);
  j += ",\"rd\":[";
  for (int i = 0; i < 19; i++) { if (i) j += ","; j += radar[i]; }
  j += "],\"mo\":"; j += (mpuOk ? 1 : 0);
  j += ",\"pi\":";  j += String(pitchDeg, 0);
  j += ",\"ro\":";  j += String(rollDeg, 0);
  j += ",\"ya\":";  j += String(yawDeg, 0);
  j += ",\"ti\":";  j += (tilt ? 1 : 0);
  j += ",\"go\":";  j += (gpsOk ? 1 : 0);
  j += ",\"fx\":";  j += (gpsFix > 0 ? 1 : 0);
  j += ",\"sa\":";  j += gpsSats;
  j += ",\"la\":";  j += String(gpsLat, 6);
  j += ",\"lo\":";  j += String(gpsLon, 6);
  j += ",\"al\":";  j += String(gpsAlt, 0);
  j += ",\"sp\":";  j += String(gpsSpeed, 1);
  j += "}";
  server.send(200, "application/json", j);
}

// =============== Setup and loop ===============
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  setupPwm();
  stopAll();
  servoSetup();
  servoWrite(90);

  Serial2.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  mpuOk = mpuBegin();                          // keep the rover still for 1 second
  Serial.println(mpuOk ? "MPU6050 found" : "MPU6050 NOT found");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);

  server.on("/", handleRoot);
  server.on("/c", handleCmd);
  server.on("/u", handleUi);
  server.on("/s", handleStatus);
  server.begin();

  digitalWrite(LED_PIN, HIGH);                 // blue light on = ready
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  server.handleClient();
  gpsFeed();
  mpuUpdate();
  scanStep();
  if (moving && millis() - lastCmd > 400) {    // failsafe
    cmdL = 0; cmdR = 0;
    moving = false;
    stopAll();
  }
}
