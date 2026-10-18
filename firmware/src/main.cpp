// reTerminal E1001 custom firmware
//
// Two ways to get pixels on the glass:
//   push: HTTP API on the device (POST /image, /text, ...), available while awake
//         (always on USB power, or for `awake` seconds after a button press / boot)
//   pull: on battery the device deep-sleeps, wakes every `interval` seconds, GETs `url`
//         (PNG or raw 48000-byte 1bpp), skips the redraw on 304/unchanged, sleeps again.
//         The server may override the next sleep with an `X-Next-Refresh: <seconds>` header.
//
// Images are always pure 1-bit: no dithering ever happens on the device.
// Buttons (all wake from deep sleep): green GPIO3 = fetch now, GPIO4 = info screen, GPIO5 = stay awake.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <SPI.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "panel.h"
#include "web_ui.h"

#define FW_VERSION "1.1.0"

// pins (Seeed schematic v1.2)
#define PIN_EPD_SCK 7
#define PIN_EPD_MOSI 9
#define PIN_EPD_CS 10
#define PIN_EPD_DC 11
#define PIN_EPD_RST 12  // also the panel's power switch
#define PIN_EPD_BUSY 13
#define PIN_KEY_FETCH 3  // green
#define PIN_KEY_INFO 4
#define PIN_KEY_AWAKE 5
#define PIN_LED 6  // active low
#define PIN_BUZZER 45
#define PIN_VBAT_ADC 1
#define PIN_VBAT_EN 21
#define PIN_SD_EN 16
#define PIN_MIC_EN 38
#define PIN_SDA0 19
#define PIN_SCL0 20
#define PIN_SDA1 39
#define PIN_SCL1 40

#define W 800
#define H 480
#define FRAME_BYTES (W / 8 * H)
#define LAST_FRAME_PATH "/last.bin"
#define KEY_MASK ((1ULL << PIN_KEY_FETCH) | (1ULL << PIN_KEY_INFO) | (1ULL << PIN_KEY_AWAKE))

struct Config {
  String ssid, pass, ssid2, pass2, url, host;  // ssid2 = fallback network
  uint32_t interval;   // seconds between pulls (0 = only on button)
  uint32_t awake;      // seconds to stay awake after button/boot on battery
  uint8_t usbAwake;    // never sleep while USB powered
  uint8_t fullEvery;   // partial refreshes between full refreshes
} cfg;

// survive deep sleep
RTC_DATA_ATTR uint8_t rtcBssid[6];
RTC_DATA_ATTR int32_t rtcChannel = 0;
RTC_DATA_ATTR uint8_t rtcPartials = 0;
RTC_DATA_ATTR uint32_t rtcNextOverride = 0;
uint32_t stayAwakeReq = 0;  // X-Stay-Awake from the pull server
RTC_DATA_ATTR uint32_t rtcFails = 0;
RTC_DATA_ATTR uint32_t rtcBoots = 0;
RTC_DATA_ATTR bool rtcLowBattShown = false;
RTC_DATA_ATTR char rtcEtag[64] = "";

SPIClass hspi(HSPI);
Panel panel(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY);
GFXcanvas1* canvas;  // framebuffer, color 1 = white
uint8_t* fb;
uint8_t* prevFrame;  // what is on the glass
bool prevValid = false, prevDirty = false;

WebServer server(80);
Preferences prefs;
PNG png;

uint32_t awakeUntil = 0, lastPull = 0, nextPullMs = 0;
bool apMode = false, pendingSleep = false, pendingReboot = false;
String lastPullResult = "never";
String wakeReason = "boot";

// ---------- hardware helpers ----------

uint32_t batteryMv() {
  digitalWrite(PIN_VBAT_EN, HIGH);
  delay(10);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VBAT_ADC);
  digitalWrite(PIN_VBAT_EN, LOW);
  return sum / 8 * 2;
}

int batteryPct(uint32_t mv) {
  static const uint16_t curve[][2] = {{4180, 100}, {4100, 92}, {4000, 82}, {3900, 70}, {3800, 55},
                                      {3750, 45}, {3700, 33}, {3650, 22}, {3600, 13}, {3500, 5}, {3300, 0}};
  if (mv >= curve[0][0]) return 100;
  for (int i = 1; i < 11; i++)
    if (mv >= curve[i][0])
      return curve[i][1] + (int)(mv - curve[i][0]) * (curve[i - 1][1] - curve[i][1]) / (curve[i - 1][0] - curve[i][0]);
  return 0;
}

// SY6974B charger on the second I2C bus; REG08 bit2 = power good (USB present)
int chargerStatus() {
  Wire1.beginTransmission(0x6B);
  Wire1.write(0x08);
  if (Wire1.endTransmission(false) != 0) return -1;
  if (Wire1.requestFrom(0x6B, 1) != 1) return -1;
  return Wire1.read();
}

bool usbPowered() {
  int s = chargerStatus();
  if (s >= 0) return s & 0x04;
  return batteryMv() > 4250;  // fallback
}

const char* chargeState() {
  int s = chargerStatus();
  if (s < 0) return "unknown";
  static const char* st[] = {"not charging", "pre-charge", "fast charging", "charged"};
  return st[(s >> 3) & 3];
}

bool readSht40(float& t, float& rh) {
  Wire.beginTransmission(0x44);
  Wire.write(0xFD);
  if (Wire.endTransmission() != 0) return false;
  delay(10);
  if (Wire.requestFrom(0x44, 6) != 6) return false;
  uint8_t b[6];
  for (auto& x : b) x = Wire.read();
  t = -45 + 175.0f * ((b[0] << 8) | b[1]) / 65535.0f;
  rh = constrain(-6 + 125.0f * ((b[3] << 8) | b[4]) / 65535.0f, 0, 100);
  return true;
}

void beep(int ms = 60, int freq = 2700) {
  ledcAttachPin(PIN_BUZZER, 0);
  ledcWriteTone(0, freq);
  delay(ms);
  ledcWriteTone(0, 0);
  ledcDetachPin(PIN_BUZZER);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
}

void led(bool on) { digitalWrite(PIN_LED, on ? LOW : HIGH); }

// ---------- config ----------

void loadConfig() {
  prefs.begin("epd", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.ssid2 = prefs.getString("ssid2", "");
  cfg.pass2 = prefs.getString("pass2", "");
  cfg.url = prefs.getString("url", "");
  cfg.host = prefs.getString("host", "reterminal");
  cfg.interval = prefs.getUInt("interval", 900);
  cfg.awake = prefs.getUInt("awake", 120);
  cfg.usbAwake = prefs.getUChar("usb_awake", 1);
  cfg.fullEvery = prefs.getUChar("full_every", 10);
  prefs.end();
}

// returns true if wifi credentials changed
bool applyConfig(JsonObjectConst j) {
  bool wifi = false;
  prefs.begin("epd", false);
  if (j["ssid"].is<const char*>()) { cfg.ssid = j["ssid"].as<String>(); prefs.putString("ssid", cfg.ssid); wifi = true; }
  if (j["pass"].is<const char*>()) { cfg.pass = j["pass"].as<String>(); prefs.putString("pass", cfg.pass); wifi = true; }
  if (j["ssid2"].is<const char*>()) { cfg.ssid2 = j["ssid2"].as<String>(); prefs.putString("ssid2", cfg.ssid2); wifi = true; }
  if (j["pass2"].is<const char*>()) { cfg.pass2 = j["pass2"].as<String>(); prefs.putString("pass2", cfg.pass2); wifi = true; }
  if (j["url"].is<const char*>()) { cfg.url = j["url"].as<String>(); prefs.putString("url", cfg.url); rtcEtag[0] = 0; }
  if (j["host"].is<const char*>()) { cfg.host = j["host"].as<String>(); prefs.putString("host", cfg.host); }
  if (j["interval"].is<uint32_t>()) { cfg.interval = j["interval"]; prefs.putUInt("interval", cfg.interval); rtcNextOverride = 0; }
  if (j["awake"].is<uint32_t>()) { cfg.awake = j["awake"]; prefs.putUInt("awake", cfg.awake); }
  if (j["usb_awake"].is<uint32_t>()) { cfg.usbAwake = j["usb_awake"]; prefs.putUChar("usb_awake", cfg.usbAwake); }
  if (j["full_every"].is<uint32_t>()) { cfg.fullEvery = j["full_every"]; prefs.putUChar("full_every", cfg.fullEvery); }
  prefs.end();
  return wifi;
}

void configJson(JsonObject o) {
  o["ssid"] = cfg.ssid;
  o["ssid2"] = cfg.ssid2;
  o["url"] = cfg.url;
  o["host"] = cfg.host;
  o["interval"] = cfg.interval;
  o["awake"] = cfg.awake;
  o["usb_awake"] = cfg.usbAwake;
  o["full_every"] = cfg.fullEvery;
}

// ---------- display ----------

const char* modeName(RefreshMode m) {
  static const char* n[] = {"auto", "full", "fast", "partial"};
  return n[m];
}

RefreshMode parseMode(const String& s) {
  if (s == "full") return REFRESH_FULL;
  if (s == "fast") return REFRESH_FAST;
  if (s == "partial") return REFRESH_PARTIAL;
  return REFRESH_AUTO;
}

// push fb to the glass; returns the mode actually used
RefreshMode showFrame(RefreshMode mode) {
  if (mode == REFRESH_AUTO) mode = (prevValid && rtcPartials < cfg.fullEvery) ? REFRESH_PARTIAL : REFRESH_FULL;
  if (mode == REFRESH_PARTIAL && !prevValid) mode = REFRESH_FULL;
  uint32_t t = millis();
  panel.show(fb, prevValid ? prevFrame : nullptr, mode);
  Serial.printf("refresh %s %lums\n", modeName(mode), millis() - t);
  memcpy(prevFrame, fb, FRAME_BYTES);
  prevValid = prevDirty = true;
  rtcPartials = mode == REFRESH_PARTIAL ? rtcPartials + 1 : 0;
  return mode;
}

void saveLastFrame() {
  if (!prevDirty) return;
  File f = LittleFS.open(LAST_FRAME_PATH, "w");
  if (f) {
    f.write(prevFrame, FRAME_BYTES);
    f.close();
    prevDirty = false;
  }
}

void loadLastFrame() {
  File f = LittleFS.open(LAST_FRAME_PATH, "r");
  if (f && f.size() == FRAME_BYTES) prevValid = f.read(prevFrame, FRAME_BYTES) == FRAME_BYTES;
  if (f) f.close();
}

// wire format: bit 1 = BLACK (what tools/epd.py sends). fb: bit 1 = white.
void loadWire(const uint8_t* src) {
  for (uint32_t i = 0; i < FRAME_BYTES; i++) fb[i] = ~src[i];
}

int pngX, pngY;
uint16_t pngLine[W * 2];
int pngDraw(PNGDRAW* d) {
  int y = d->y + pngY;
  if (y < 0 || y >= H) return 1;
  png.getLineAsRGB565(d, pngLine, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  for (int x = 0; x < d->iWidth; x++) {
    int px = x + pngX;
    if (px < 0 || px >= W) continue;
    uint16_t c = pngLine[x];
    uint32_t r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
    canvas->drawPixel(px, y, (r * 299 + g * 587 + b * 114) / 1000 >= 128 ? 1 : 0);
  }
  return 1;
}

// decode a PNG or raw frame into fb; returns error string or "" on success
String decodeImage(uint8_t* data, size_t len) {
  if (len >= 8 && memcmp(data, "\x89PNG", 4) == 0) {
    if (png.openRAM(data, len, pngDraw) != PNG_SUCCESS) return "bad png";
    canvas->fillScreen(1);
    pngX = (W - png.getWidth()) / 2;
    pngY = (H - png.getHeight()) / 2;
    int rc = png.decode(nullptr, 0);
    png.close();
    return rc == PNG_SUCCESS ? "" : "png decode failed";
  }
  if (len == FRAME_BYTES) {
    loadWire(data);
    return "";
  }
  return "expected PNG or 48000-byte raw frame, got " + String(len) + " bytes";
}

void drawCentered(const String& s, int y, const GFXfont* f, int scale = 1) {
  canvas->setFont(f);
  canvas->setTextSize(scale);
  int16_t x1, y1;
  uint16_t w, h;
  canvas->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  canvas->setCursor((W - w) / 2 - x1, y);
  canvas->print(s);
}

void renderText(const String& text, int size) {
  canvas->fillScreen(1);
  canvas->setTextColor(0);
  const GFXfont* f = &FreeSansBold24pt7b;
  int lineH = 56 * size;
  int lines = 1;
  for (char c : text) lines += c == '\n';
  int y = (H - lines * lineH) / 2 + lineH * 3 / 4;
  int start = 0;
  for (int i = 0; i <= (int)text.length(); i++) {
    if (i == (int)text.length() || text[i] == '\n') {
      drawCentered(text.substring(start, i), y, f, size);
      y += lineH;
      start = i + 1;
    }
  }
}

String ipString() { return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }

void renderInfo(const String& headline) {
  uint32_t mv = batteryMv();
  float t = NAN, rh = NAN;
  readSht40(t, rh);
  canvas->fillScreen(1);
  canvas->fillRect(0, 0, W, 84, 0);
  canvas->setTextColor(1);
  drawCentered(headline, 56, &FreeSansBold24pt7b);
  canvas->setTextColor(0);
  canvas->setFont(&FreeSansBold12pt7b);
  canvas->setTextSize(1);
  struct { const char* k; String v; } rows[] = {
      {"Address", apMode ? "Join WiFi \"" + WiFi.softAPSSID() + "\", open http://" + ipString()
                         : "http://" + cfg.host + ".local   (" + ipString() + ")"},
      {"WiFi", apMode ? String("setup mode") : WiFi.SSID() + "  " + String(WiFi.RSSI()) + " dBm"},
      {"Battery", String(mv / 1000.0f, 2) + " V  " + batteryPct(mv) + "%  " + chargeState()},
      {"Room", isnan(t) ? String("-") : String(t, 1) + " C   " + String(rh, 0) + "% RH"},
      {"Pull", cfg.url.length() ? cfg.url : String("(no url set - push only)")},
      {"Interval", String(cfg.interval) + " s, awake " + cfg.awake + " s, full every " + cfg.fullEvery},
      {"Buttons", "green: fetch now   left: this screen   right: stay awake"},
      {"Firmware", String(FW_VERSION) + "   MAC " + WiFi.macAddress()},
  };
  int y = 140;
  for (auto& r : rows) {
    canvas->setFont(&FreeSansBold12pt7b);
    canvas->setCursor(30, y);
    canvas->print(r.k);
    canvas->setFont(&FreeSans12pt7b);
    canvas->setCursor(170, y);
    canvas->print(r.v);
    y += 42;
  }
}

// ---------- wifi ----------

RTC_DATA_ATTR uint8_t rtcNet = 0;  // which network the cached BSSID belongs to

bool connectWifi(uint32_t timeoutMs = 12000) {
  if (cfg.ssid.isEmpty()) return false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.host.c_str());
  // attempt 0: cached BSSID/channel (fast), 1: primary network, 2: fallback network
  for (int attempt = 0; attempt < 3; attempt++) {
    bool fast = attempt == 0;
    if (fast && rtcChannel <= 0) continue;
    int net = fast ? rtcNet : attempt - 1;
    const String& ssid = net ? cfg.ssid2 : cfg.ssid;
    const String& pass = net ? cfg.pass2 : cfg.pass;
    if (ssid.isEmpty()) continue;
    if (fast) WiFi.begin(ssid.c_str(), pass.c_str(), rtcChannel, rtcBssid);
    else WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < (fast ? 5000 : timeoutMs)) delay(20);
    if (WiFi.status() == WL_CONNECTED) {
      rtcChannel = WiFi.channel();
      rtcNet = net;
      memcpy(rtcBssid, WiFi.BSSID(), 6);
      Serial.printf("wifi ok %s on %s in %lums (%s)\n", WiFi.localIP().toString().c_str(), ssid.c_str(), millis() - t0, fast ? "fast" : "scan");
      return true;
    }
    WiFi.disconnect();
    rtcChannel = 0;
  }
  Serial.println("wifi failed");
  return false;
}

void startAp() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  String name = "reTerminal-" + WiFi.macAddress().substring(12);
  name.replace(":", "");
  WiFi.softAP(name.c_str());
  Serial.printf("AP mode: %s %s\n", name.c_str(), WiFi.softAPIP().toString().c_str());
}

// ---------- pull ----------

// returns true if the screen changed
bool pull() {
  lastPull = millis();
  if (cfg.url.isEmpty()) { lastPullResult = "no url"; return false; }
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) { lastPullResult = "wifi failed"; rtcFails++; return false; }

  HTTPClient http;
  WiFiClientSecure tls;
  WiFiClient plain;
  if (cfg.url.startsWith("https")) {
    tls.setInsecure();
    http.begin(tls, cfg.url);
  } else {
    http.begin(plain, cfg.url);
  }
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const char* keep[] = {"ETag", "X-Next-Refresh", "X-Stay-Awake", "Content-Type"};
  http.collectHeaders(keep, 4);
  uint32_t mv = batteryMv();
  http.addHeader("X-Battery-mV", String(mv));
  http.addHeader("X-Battery-Pct", String(batteryPct(mv)));
  http.addHeader("X-Device-Id", WiFi.macAddress());
  http.addHeader("X-Wake-Reason", wakeReason);
  http.addHeader("X-Firmware", FW_VERSION);
  http.addHeader("X-RSSI", String(WiFi.RSSI()));
  if (prevValid && rtcEtag[0]) http.addHeader("If-None-Match", rtcEtag);

  int code = http.GET();
  rtcNextOverride = http.header("X-Next-Refresh").toInt();
  stayAwakeReq = http.header("X-Stay-Awake").toInt();
  bool changed = false;
  if (code == 304) {
    lastPullResult = "304 unchanged";
    rtcFails = 0;
  } else if (code == 200) {
    int len = http.getSize();
    size_t cap = len > 0 ? len : 512 * 1024;
    uint8_t* buf = (uint8_t*)ps_malloc(cap);
    size_t got = 0;
    if (buf) {
      WiFiClient* s = http.getStreamPtr();
      uint32_t t0 = millis();
      while ((len < 0 || got < (size_t)len) && got < cap && millis() - t0 < 20000 && (http.connected() || s->available())) {
        int n = s->read(buf + got, cap - got);
        if (n > 0) { got += n; t0 = millis(); }
        else delay(2);
      }
      String err = decodeImage(buf, got);
      free(buf);
      if (err.isEmpty()) {
        String etag = http.header("ETag");
        bool same = prevValid && memcmp(fb, prevFrame, FRAME_BYTES) == 0;
        if (!same) { showFrame(REFRESH_AUTO); changed = true; }
        strlcpy(rtcEtag, etag.c_str(), sizeof(rtcEtag));
        lastPullResult = same ? "200 identical" : "200 updated";
        rtcFails = 0;
      } else {
        lastPullResult = err;
        rtcFails++;
      }
    } else {
      lastPullResult = "out of memory";
    }
  } else {
    lastPullResult = "http " + String(code);
    rtcFails++;
  }
  http.end();
  Serial.printf("pull: %s\n", lastPullResult.c_str());
  return changed;
}

uint32_t nextIntervalS() {
  if (rtcNextOverride) return rtcNextOverride;
  if (rtcFails) return min<uint32_t>(cfg.interval ? cfg.interval : 3600, 60u << min<uint32_t>(rtcFails - 1, 5));  // back off
  return cfg.interval;
}

// ---------- sleep ----------

void goToSleep() {
  Serial.println("sleeping");
  saveLastFrame();
  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  panel.hibernate();
  led(false);
  digitalWrite(PIN_VBAT_EN, LOW);
  digitalWrite(PIN_SD_EN, LOW);
  digitalWrite(PIN_MIC_EN, LOW);
  // wait for keys to be released so we don't wake instantly
  uint32_t t0 = millis();
  while ((!digitalRead(PIN_KEY_FETCH) || !digitalRead(PIN_KEY_INFO) || !digitalRead(PIN_KEY_AWAKE)) && millis() - t0 < 3000) delay(10);
  esp_sleep_enable_ext1_wakeup(KEY_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
  uint32_t s = cfg.url.length() && !rtcLowBattShown ? nextIntervalS() : 0;
  if (s) esp_sleep_enable_timer_wakeup((uint64_t)s * 1000000ULL);
  Serial.printf("next wake: %s\n", s ? (String(s) + "s").c_str() : "button only");
  Serial.flush();
  esp_deep_sleep_start();
}

void stayAwake(uint32_t s) { awakeUntil = max<uint32_t>(awakeUntil, millis() + s * 1000); }

// ---------- http api ----------

uint8_t* upBuf = nullptr;
size_t upLen = 0, upCap = 0;
bool otaOk = false;
String otaErr;

void rawToBuffer() {
  HTTPRaw& r = server.raw();
  if (r.status == RAW_START) {
    upCap = 1024 * 1024;
    if (!upBuf) upBuf = (uint8_t*)ps_malloc(upCap);
    upLen = 0;
  } else if (r.status == RAW_WRITE && upBuf) {
    size_t n = min(r.currentSize, upCap - upLen);
    memcpy(upBuf + upLen, r.buf, n);
    upLen += n;
  }
}

void rawToOta() {
  HTTPRaw& r = server.raw();
  if (r.status == RAW_START) {
    otaErr = "";
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaErr = Update.errorString();
  } else if (r.status == RAW_WRITE && otaErr.isEmpty()) {
    if (Update.write(r.buf, r.currentSize) != r.currentSize) otaErr = Update.errorString();
  } else if (r.status == RAW_END && otaErr.isEmpty()) {
    if (!Update.end(true)) otaErr = Update.errorString();
  } else if (r.status == RAW_ABORTED) {
    Update.abort();
    otaErr = "aborted";
  }
}

// WebServer skips query parsing for raw-body POSTs, so params can also come as X-<Name> headers
String param(const char* name) {
  if (server.hasArg(name)) return server.arg(name);
  return server.header(String("X-") + name);
}

void touch() { stayAwake(cfg.awake ? cfg.awake : 60); }

void sendJson(JsonDocument& d) {
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleStatus() {
  touch();
  JsonDocument d;
  uint32_t mv = batteryMv();
  float t, rh;
  d["firmware"] = FW_VERSION;
  d["battery_mv"] = mv;
  d["battery_pct"] = batteryPct(mv);
  d["usb_power"] = usbPowered();
  d["charging"] = chargeState();
  if (readSht40(t, rh)) { d["temp_c"] = round(t * 10) / 10; d["humidity"] = round(rh); }
  d["rssi"] = WiFi.RSSI();
  d["ssid"] = WiFi.SSID();
  d["ip"] = ipString();
  d["uptime_s"] = millis() / 1000;
  d["boots"] = rtcBoots;
  d["wake_reason"] = wakeReason;
  d["last_pull"] = lastPullResult;
  d["partials_since_full"] = rtcPartials;
  d["sleeps_in_s"] = (usbPowered() && cfg.usbAwake) ? -1 : (int32_t)((int64_t)awakeUntil - millis()) / 1000;
  d["free_psram"] = ESP.getFreePsram();
  configJson(d["config"].to<JsonObject>());
  sendJson(d);
}

void setupServer() {
  server.on("/", HTTP_GET, [] { touch(); server.send_P(200, "text/html", WEB_UI); });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/screen.bin", HTTP_GET, [] {
    touch();
    static uint8_t out[FRAME_BYTES];
    for (uint32_t i = 0; i < FRAME_BYTES; i++) out[i] = ~prevFrame[i];
    server.send_P(200, "application/octet-stream", (const char*)out, FRAME_BYTES);
  });
  server.on("/image", HTTP_POST, [] {
    touch();
    String err = upBuf ? decodeImage(upBuf, upLen) : "no body";
    if (!err.isEmpty()) { server.send(400, "text/plain", err); return; }
    uint32_t t = millis();
    RefreshMode m = showFrame(parseMode(param("mode")));
    server.send(200, "text/plain", String("ok ") + modeName(m) + " " + (millis() - t) + "ms");
  }, rawToBuffer);
  server.on("/text", HTTP_POST, [] {
    touch();
    String text = upBuf ? String((const char*)upBuf).substring(0, upLen) : server.arg("plain");
    if (server.hasArg("text")) text = server.arg("text");
    String size = param("size");
    renderText(text, size.length() ? constrain(size.toInt(), 1, 4) : 2);
    RefreshMode m = showFrame(parseMode(param("mode")));
    server.send(200, "text/plain", String("ok ") + modeName(m));
  }, [] { rawToBuffer(); if (upBuf && upLen < upCap) upBuf[upLen] = 0; });
  server.on("/config", HTTP_GET, [] { touch(); JsonDocument d; configJson(d.to<JsonObject>()); sendJson(d); });
  server.on("/config", HTTP_POST, [] {
    touch();
    JsonDocument d;
    String body = upBuf ? String((const char*)upBuf).substring(0, upLen) : server.arg("plain");
    if (deserializeJson(d, body)) { server.send(400, "text/plain", "bad json"); return; }
    bool wifi = applyConfig(d.as<JsonObjectConst>());
    nextPullMs = millis() + nextIntervalS() * 1000;
    server.send(200, "text/plain", wifi ? "saved, rebooting to join wifi" : "saved");
    if (wifi) pendingReboot = true;
  }, [] { rawToBuffer(); if (upBuf && upLen < upCap) upBuf[upLen] = 0; });
  server.on("/refresh", HTTP_POST, [] { touch(); rtcEtag[0] = 0; pull(); server.send(200, "text/plain", lastPullResult); });
  server.on("/clear", HTTP_POST, [] { touch(); canvas->fillScreen(1); showFrame(REFRESH_FULL); server.send(200, "text/plain", "cleared"); });
  server.on("/info", HTTP_POST, [] { touch(); renderInfo("reTerminal"); showFrame(REFRESH_AUTO); server.send(200, "text/plain", "ok"); });
  server.on("/beep", HTTP_POST, [] { touch(); beep(120); server.send(200, "text/plain", "beep"); });
  server.on("/sleep", HTTP_POST, [] { server.send(200, "text/plain", "going to sleep"); pendingSleep = true; });
  server.on("/reboot", HTTP_POST, [] { server.send(200, "text/plain", "rebooting"); pendingReboot = true; });
  server.on("/ota", HTTP_POST, [] {
    otaOk = otaErr.isEmpty();
    server.send(otaOk ? 200 : 500, "text/plain", otaOk ? "update ok, rebooting" : "update failed: " + otaErr);
    if (otaOk) pendingReboot = true;
  }, rawToOta);
  const char* hdrs[] = {"X-mode", "X-size"};
  server.collectHeaders(hdrs, 2);
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
  server.begin();
  MDNS.begin(cfg.host.c_str());
  MDNS.addService("http", "tcp", 80);
}

// ---------- serial console (for setup over USB) ----------

void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { line += c; continue; }
    line.trim();
    if (line.isEmpty()) continue;
    if (line.startsWith("{")) {  // {"ssid":"..","pass":".."}
      JsonDocument d;
      if (deserializeJson(d, line)) Serial.println("bad json");
      else { bool wifi = applyConfig(d.as<JsonObjectConst>()); Serial.println("saved"); if (wifi) pendingReboot = true; }
    } else if (line == "status") {
      Serial.printf("ip=%s ssid=%s rssi=%d batt=%lumV usb=%d url=%s interval=%lu last_pull=%s\n", ipString().c_str(),
                    cfg.ssid.c_str(), WiFi.RSSI(), batteryMv(), usbPowered(), cfg.url.c_str(), cfg.interval, lastPullResult.c_str());
    } else if (line == "reboot") pendingReboot = true;
    else if (line == "sleep") pendingSleep = true;
    else if (line == "pull") { rtcEtag[0] = 0; pull(); }
    else if (line == "info") { renderInfo("reTerminal"); showFrame(REFRESH_AUTO); }
    else Serial.println("commands: status | reboot | sleep | pull | info | {json config}");
    line = "";
    stayAwake(60);
  }
}

// ---------- main ----------

void setup() {
  Serial.begin(115200);
  rtcBoots++;
  pinMode(PIN_LED, OUTPUT);
  led(false);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  for (int p : {PIN_VBAT_EN, PIN_SD_EN, PIN_MIC_EN}) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  for (int p : {PIN_KEY_FETCH, PIN_KEY_INFO, PIN_KEY_AWAKE}) pinMode(p, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VBAT_ADC, ADC_11db);
  Wire.begin(PIN_SDA0, PIN_SCL0);
  Wire1.begin(PIN_SDA1, PIN_SCL1);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  int key = -1;
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t m = esp_sleep_get_ext1_wakeup_status();
    key = m & (1ULL << PIN_KEY_FETCH) ? PIN_KEY_FETCH : m & (1ULL << PIN_KEY_INFO) ? PIN_KEY_INFO : PIN_KEY_AWAKE;
    wakeReason = key == PIN_KEY_FETCH ? "button_fetch" : key == PIN_KEY_INFO ? "button_info" : "button_awake";
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    wakeReason = "timer";
  }
  Serial.printf("\nreTerminal fw %s, wake=%s\n", FW_VERSION, wakeReason.c_str());

  loadConfig();
  canvas = new GFXcanvas1(W, H);
  fb = canvas->getBuffer();
  prevFrame = (uint8_t*)ps_malloc(FRAME_BYTES);
  LittleFS.begin(true);
  loadLastFrame();

  hspi.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, -1);
  panel.selectSPI(hspi, SPISettings(10000000, MSBFIRST, SPI_MODE0));
  panel.init(0, false);  // we always write both buffers ourselves

  if (key >= 0) {
    led(true);
    beep(25);
  }

  // protect the battery: below 3.4V on battery, show a notice once and only wake on buttons
  uint32_t mv = batteryMv();
  bool usb = usbPowered();
  if (!usb && mv > 2500 && mv < 3400) {
    if (!rtcLowBattShown) {
      renderText("Battery low\nplease charge", 1);
      showFrame(REFRESH_FULL);
      rtcLowBattShown = true;
    }
    if (key < 0) goToSleep();
  } else {
    rtcLowBattShown = false;
  }

  // timer wake on battery: the fast path. Fetch, maybe redraw, sleep again.
  if (cause == ESP_SLEEP_WAKEUP_TIMER && !(usb && cfg.usbAwake)) {
    pull();
    if (!stayAwakeReq) goToSleep();
  }

  bool wifiOk = connectWifi();
  if (!wifiOk && cause != ESP_SLEEP_WAKEUP_TIMER) {
    startAp();
    renderInfo("WiFi setup");
    showFrame(REFRESH_FULL);
    stayAwake(600);
  }
  setupServer();

  if (key == PIN_KEY_INFO || (wifiOk && (!prevValid || (cause == ESP_SLEEP_WAKEUP_UNDEFINED && cfg.url.isEmpty())))) {
    renderInfo("reTerminal");
    showFrame(REFRESH_AUTO);
  }
  bool pulled = lastPull != 0;  // the timer fast path may already have pulled
  if (wifiOk && !pulled && (key == PIN_KEY_FETCH || cause == ESP_SLEEP_WAKEUP_UNDEFINED || cause == ESP_SLEEP_WAKEUP_TIMER)) {
    if (key == PIN_KEY_FETCH) rtcEtag[0] = 0;
    pull();
  }
  stayAwake(stayAwakeReq ? stayAwakeReq : cfg.awake);
  nextPullMs = millis() + nextIntervalS() * 1000;
  led(false);
}

void loop() {
  server.handleClient();
  handleSerial();

  if (pendingReboot) { delay(300); saveLastFrame(); ESP.restart(); }
  if (pendingSleep) { delay(300); goToSleep(); }

  static uint32_t lastPowerCheck = 0;
  static bool onUsb = usbPowered();
  if (millis() - lastPowerCheck > 5000) {
    lastPowerCheck = millis();
    onUsb = usbPowered();
    // persist the frame now and then while on USB so a reboot can still partial-refresh
    if (onUsb && prevDirty && millis() - lastPull > 60000) saveLastFrame();
  }

  // periodic pull while awake (mostly: on USB power)
  if (cfg.url.length() && cfg.interval && !apMode && (int32_t)(millis() - nextPullMs) >= 0) {
    pull();
    if (stayAwakeReq) stayAwake(stayAwakeReq);
    nextPullMs = millis() + nextIntervalS() * 1000;
  }

  if (!(onUsb && cfg.usbAwake) && (int32_t)(millis() - awakeUntil) >= 0) goToSleep();

  // buttons while awake
  static uint32_t lastKey = 0;
  if (millis() - lastKey > 400) {
    if (!digitalRead(PIN_KEY_FETCH)) { lastKey = millis(); beep(25); rtcEtag[0] = 0; pull(); touch(); }
    else if (!digitalRead(PIN_KEY_INFO)) { lastKey = millis(); beep(25); renderInfo("reTerminal"); showFrame(REFRESH_AUTO); touch(); }
    else if (!digitalRead(PIN_KEY_AWAKE)) { lastKey = millis(); beep(25); touch(); }
  }
  delay(5);
}
