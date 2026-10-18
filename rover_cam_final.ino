// ESP32-CAM (AI Thinker): joins the rover's "Rover" WiFi at 192.168.4.2
// Camera page (full screen + rotate): http://192.168.4.2
// Raw stream (used by the rover page too): http://192.168.4.2:81/stream
// Board: AI Thinker ESP32-CAM, PSRAM enabled. ESP32 only supports 2.4 GHz WiFi.

#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_http_server.h"

// ---------- Must match the rover's AP_SSID / AP_PASS ----------
const char* SSID = "Rover";
const char* PASS = "rover1234";
const char* HOSTNAME = "rovercam";
// --------------------------------------------------------------

// AI Thinker pins
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

static const char PAGE[] = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Rover Cam</title>
<style>
html,body{margin:0;height:100%;background:#000;overflow:hidden}
#cam{position:absolute;left:50%;top:50%;object-fit:cover;transform:translate(-50%,-50%)}
#bar{position:absolute;left:0;right:0;bottom:12px;text-align:center;z-index:2}
button{background:rgba(0,0,0,.55);color:#eee;border:1px solid #777;border-radius:6px;
padding:8px 12px;margin:3px;font-size:15px}
</style></head><body>
<img id="cam" alt="camera">
<div id="bar"><button onclick="rot()">Rotate &#10227;</button>
<button onclick="fs()">Full screen</button></div>
<script>
let r = 0;
try { r = parseInt(localStorage.getItem('r') || '0') || 0; } catch (e) {}
const img = document.getElementById('cam');
function apply() {
  const W = innerWidth, H = innerHeight, sw = (r % 180 !== 0);
  img.style.width = (sw ? H : W) + 'px';
  img.style.height = (sw ? W : H) + 'px';
  img.style.transform = 'translate(-50%,-50%) rotate(' + r + 'deg)';
}
function rot() { r = (r + 90) % 360; try { localStorage.setItem('r', r); } catch (e) {} apply(); }
function fs() {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen();
}
function start() { img.src = 'http://' + location.hostname + ':81/stream?' + Date.now(); }
img.onerror = () => setTimeout(start, 2000);
addEventListener('resize', apply);
document.addEventListener('keydown', e => {
  if (e.key === 'v') rot();
  if (e.key === 'f') fs();
});
apply(); start();
</script></body></html>
)rawliteral";

static esp_err_t page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t stream_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  const char* BOUNDARY = "\r\n--frame\r\n";
  char part[64];
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;
    esp_err_t res = httpd_resp_send_chunk(req, BOUNDARY, strlen(BOUNDARY));
    if (res == ESP_OK) {
      size_t hl = snprintf(part, sizeof(part),
        "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)fb->len);
      res = httpd_resp_send_chunk(req, part, hl);
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;      // browser closed the stream
  }
  return ESP_OK;
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.config(IPAddress(192, 168, 4, 2), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(SSID, PASS);
  WiFi.setSleep(false);
  Serial.print("Connecting to ");
  Serial.print(SSID);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - t > 20000) {            // retry every 20 s
      Serial.println("\nRetrying...");
      WiFi.disconnect();
      WiFi.begin(SSID, PASS);
      t = millis();
    }
  }
  Serial.println("\nConnected.");
  Serial.println("Camera IP: " + WiFi.localIP().toString());
}

void setup() {
  Serial.begin(115200);

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM; c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_QVGA;   // 320x240, keeps the rover WiFi responsive
  c.jpeg_quality = 12;             // higher number = smaller, lower quality
  c.fb_count = 2;
  c.grab_mode = CAMERA_GRAB_LATEST;
  c.fb_location = CAMERA_FB_IN_PSRAM;

  if (esp_camera_init(&c) != ESP_OK) {
    Serial.println("Camera init failed - check ribbon cable and power");
    return;
  }

  connectWifi();
  if (MDNS.begin(HOSTNAME)) Serial.println("Open http://" + String(HOSTNAME) + ".local");

  // Page server on port 80
  httpd_config_t pc = HTTPD_DEFAULT_CONFIG();
  pc.server_port = 80;
  pc.ctrl_port = 32768;
  httpd_uri_t pageUri = { "/", HTTP_GET, page_handler, NULL };
  httpd_handle_t pageSrv = NULL;
  if (httpd_start(&pageSrv, &pc) == ESP_OK) httpd_register_uri_handler(pageSrv, &pageUri);

  // Stream server on port 81
  httpd_config_t sc = HTTPD_DEFAULT_CONFIG();
  sc.server_port = 81;
  sc.ctrl_port = 32769;
  httpd_uri_t streamUri = { "/stream", HTTP_GET, stream_handler, NULL };
  httpd_handle_t streamSrv = NULL;
  if (httpd_start(&streamSrv, &sc) == ESP_OK) httpd_register_uri_handler(streamSrv, &streamUri);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(2000); }
  delay(1000);
}
