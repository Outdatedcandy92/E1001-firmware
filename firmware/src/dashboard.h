// Fabricate metrics dashboard, fetched and drawn on the device (url = "fabricate").
//
// Calls the Fabricate admin MCP (https://fabricate.hackclub.com/mcp) over HTTPS with the token in
// cfg.fabToken, draws the screen into fb and sleeps until just after the next full hour. The clock
// is set from the server's Date header, so no NTP is needed. If a fetch fails the last frame stays
// up with an OFFLINE badge in the header and pull() backs off as usual.
//
// Layout notes: coordinates are PIL-style inclusive boxes; text anchors follow PIL ("la", "rm",
// ...) using the ascent/descent that gen_fonts.py stored with each font.
#pragma once
#include "dash_fonts.h"
#include "roots.h"

#define FABRICATE_MCP "https://fabricate.hackclub.com/mcp"
#define FAB_WAKE_AFTER_HOUR_S 20

RTC_DATA_ATTR bool rtcDashShown = false;

// ---------- drawing ----------

namespace dash {

const uint16_t BLACK = 0, WHITE = 1;

int tw(const String& s, const DashFont& f) {
  int w = 0;
  for (char ch : s)
    if (ch >= f.gfx->first && ch <= f.gfx->last) w += f.gfx->glyph[ch - f.gfx->first].xAdvance;
  return w;
}

// anchor: h = l/m/r, v = a (ascender) / m (middle) / s (baseline). Returns the text width.
int text(float x, float y, const String& s, const DashFont& f, uint16_t color = BLACK, const char* anchor = "la") {
  int w = tw(s, f);
  float bx = anchor[0] == 'r' ? x - w : anchor[0] == 'm' ? x - w / 2.0f : x;
  float by = anchor[1] == 'a' ? y + f.ascent : anchor[1] == 'm' ? y + (f.ascent - f.descent) / 2.0f : y;
  canvas->setFont(f.gfx);
  canvas->setTextSize(1);
  canvas->setTextWrap(false);
  canvas->setTextColor(color);
  canvas->setCursor(lroundf(bx), lroundf(by));
  canvas->print(s);
  return w;
}

void fill(float x0, float y0, float x1, float y1, uint16_t c = BLACK) {
  int a = lroundf(x0), b = lroundf(y0), r = lroundf(x1), d = lroundf(y1);
  if (r >= a && d >= b) canvas->fillRect(a, b, r - a + 1, d - b + 1, c);
}

void outline(float x0, float y0, float x1, float y1, int width = 1) {
  for (int i = 0; i < width; i++) {
    int a = lroundf(x0) + i, b = lroundf(y0) + i, r = lroundf(x1) - i, d = lroundf(y1) - i;
    if (r >= a && d >= b) canvas->drawRect(a, b, r - a + 1, d - b + 1, BLACK);
  }
}

// 1-bit "grey": diagonal lines
void hatch(float fx0, float fy0, float fx1, float fy1, int step = 2) {
  int x0 = fx0, y0 = fy0, x1 = fx1, y1 = fy1;
  for (int y = y0; y <= y1; y++)
    for (int x = x0 + (step - (x0 + y) % step) % step; x <= x1; x += step) canvas->drawPixel(x, y, BLACK);
}

void dots(float x0, float x1, int y, int gap) {
  for (int x = x0; x <= (int)x1; x += gap) canvas->drawPixel(x, y, BLACK);
}

struct Box { float x0, y0, x1, y1; };

// bordered panel with a black title tab; returns the content area
Box section(Box b, const String& title, const String& right = "") {
  outline(b.x0, b.y0, b.x1, b.y1, 2);
  int w = tw(title, PIXB8) + 12;
  fill(b.x0, b.y0, b.x0 + w, b.y0 + 13);
  text(b.x0 + 6, b.y0 + 3, title, PIXB8, WHITE);
  if (right.length()) text(b.x1 - 6, b.y0 + 4, right, PIX8, BLACK, "ra");
  return {b.x0 + 8, b.y0 + 20, b.x1 - 8, b.y1 - 6};
}

// mini column chart: newest bar solid, the rest hatched
void bars(Box b, const float* v, int n) {
  if (n <= 0) return;
  float top = 0;
  for (int i = 0; i < n; i++) top = max(top, v[i]);
  if (top <= 0) top = 1;
  float bw = (b.x1 - b.x0 + 1) / n;
  for (int i = 0; i < n; i++) {
    float bx0 = b.x0 + i * bw, bx1 = bx0 + bw - 2;
    float h = v[i] <= 0 ? 0 : max(1.0f, (b.y1 - b.y0) * v[i] / top);
    if (!h) continue;
    if (i == n - 1) fill(bx0, b.y1 - h, bx1, b.y1);
    else { outline(bx0, b.y1 - h, bx1, b.y1); hatch(bx0, b.y1 - h, bx1, b.y1); }
  }
  canvas->drawFastHLine(b.x0, b.y1 + 1, b.x1 - b.x0 + 1, BLACK);
}

String fmt(float n) {
  char s[16];
  float a = fabsf(n);
  if (a >= 10000) snprintf(s, sizeof s, "%.0fk", n / 1000);
  else if (a >= 1000) snprintf(s, sizeof s, "%.1fk", n / 1000);
  else if (n == (int)n || a >= 100) snprintf(s, sizeof s, "%.0f", n);
  else snprintf(s, sizeof s, "%.1f", n);
  return s;
}

String thousands(long n) {
  String s = String(n), o;
  for (int i = 0; i < (int)s.length(); i++) {
    if (i && (s.length() - i) % 3 == 0) o += ',';
    o += s[i];
  }
  return o;
}

String pct(float a, float b) { return b ? String(lroundf(100 * a / b)) + "%" : "-"; }

String delta(float cur, float prev) {
  if (!prev) return cur ? "NEW" : "FLAT";
  long d = lroundf((cur - prev) / prev * 100);
  return (d >= 0 ? "+" : "") + String(d) + "%";
}

String upper(String s) { s.toUpperCase(); return s; }

}  // namespace dash

// ---------- data ----------

struct DashData {
  JsonDocument metrics, overview, daily, queue, grants;
};

// one MCP tools/call; fills out with result.structuredContent
String mcpCall(HTTPClient& http, WiFiClientSecure& tls, const char* name, const char* args, JsonDocument& out,
               String* date = nullptr) {
  http.begin(tls, FABRICATE_MCP);
  http.setTimeout(20000);
  http.addHeader("Authorization", "Bearer " + cfg.fabToken);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json, text/event-stream");
  const char* keep[] = {"Date"};
  http.collectHeaders(keep, 1);
  String body = String("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"") + name +
                "\",\"arguments\":" + args + "}}";
  int code = http.POST(body);
  if (code != 200) {
    http.end();
    return String(name) + " http " + code;
  }
  if (date) *date = http.header("Date");
  String resp = http.getString();
  http.end();
  int i = resp.startsWith("event:") || resp.startsWith("data:") ? resp.indexOf("data:") : -1;
  if (i >= 0) resp = resp.substring(i + 5);  // SSE framing
  JsonDocument filter;
  filter["result"]["structuredContent"] = true;
  filter["result"]["isError"] = true;
  filter["error"] = true;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, resp, DeserializationOption::Filter(filter));
  if (e) return String(name) + " json " + e.c_str();
  if (d["error"].is<JsonObject>() || d["result"]["isError"].as<bool>()) return String(name) + " tool error";
  out.set(d["result"]["structuredContent"]);
  return "";
}

// Date: Mon, 19 Oct 2026 13:51:40 GMT -> system clock
void setClockFromHttpDate(const String& date) {
  static const char* mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int d, y, hh, mm, ss;
  char m[4];
  if (sscanf(date.c_str(), "%*3s, %d %3s %d %d:%d:%d", &d, m, &y, &hh, &mm, &ss) != 6) return;
  const char* p = strstr(mon, m);
  if (!p) return;
  int mo = (p - mon) / 3 + 1;
  // days from civil (Howard Hinnant)
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long days = era * 146097L + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  timeval tv = {(time_t)(days * 86400L + hh * 3600 + mm * 60 + ss), 0};
  settimeofday(&tv, nullptr);
}

void applyTz() {
  setenv("TZ", cfg.tz.c_str(), 1);
  tzset();
}

// ---------- the screen ----------

void renderDashboard(DashData& D) {
  using namespace dash;
  JsonObject m = D.metrics.as<JsonObject>();
  JsonArray daily = D.daily["series"].as<JsonArray>();
  JsonObject qs = D.queue["summary"].as<JsonObject>();
  canvas->fillScreen(WHITE);

  time_t now = time(nullptr);
  tm lt;
  localtime_r(&now, &lt);

  // header
  fill(0, 0, W, 38);
  float x = 14 + text(14, 6, "FABRICATE", BIG22, WHITE);
  text(x + 10, 15, "HACK CLUB YSWS / ADMIN METRICS", PIX8, WHITE);
  char right[32], sub[48];
  strftime(right, sizeof right, "%a %d %b  %H:%M", &lt);
  String r = upper(right);
  text(W - 14, 9, r, MONO18B, WHITE, "ra");
  uint32_t mv = batteryMv();
  snprintf(sub, sizeof sub, "NEXT %02d:00  %s", (lt.tm_hour + 1) % 24,
           usbPowered() ? "USB" : (String("BAT ") + batteryPct(mv) + "%").c_str());
  text(W - 14 - tw(r, MONO18B) - 12, 16, sub, PIX8, WHITE, "ra");

  // KPI row: 7-day sums vs the 7 days before, from the daily series
  int nd = daily.size();
  auto sum7 = [&](const char* k, int from) {
    float s = 0;
    for (int i = max(0, nd - from); i < nd - from + 7 && i < nd; i++) s += daily[i][k].as<float>();
    return s;
  };
  auto last7 = [&](const char* k) { return sum7(k, 7); };
  auto prev7 = [&](const char* k) { return sum7(k, 14); };
  float series[90];
  auto dailySeries = [&](const char* k) {
    int n = min(nd, 90);
    for (int i = 0; i < n; i++) series[i] = daily[nd - n + i][k].as<float>();
    return n;
  };

  auto kpi = [&](Box b, const String& label, const String& value, const char* unit, const String& subt,
                 const String& chg, int n) {
    Box in = section(b, label, chg);
    float vx = in.x0 + text(in.x0, in.y0 - 2, value, BIG40);
    if (unit) text(vx + 4, in.y0 + 30, unit, PIXB8);
    text(in.x0, in.y0 + 50, subt, MONO12M);
    bars({in.x0, in.y0 + 68, in.x1, in.y1 - 1}, series, n);
  };

  const float T = 46, KH = 128;
  JsonObject act = m["activity"];
  JsonArray aser = act["series"];
  int na = min((int)aser.size(), 30);
  for (int i = 0; i < na; i++) series[i] = aser[aser.size() - na + i]["users"].as<float>();
  kpi({12, T, 197, T + KH}, "ACTIVE MAKERS", fmt(act["dau"]), "DAU",
      "WAU " + fmt(act["wau"]) + "  MAU " + fmt(act["mau"]),
      String(lroundf(act["stickiness"].as<float>() * 100)) + "% STICKY", na);
  int n = dailySeries("new_accounts");
  kpi({205, T, 390, T + KH}, "SIGNUPS 7D", fmt(last7("new_accounts")), nullptr,
      fmt(D.overview["users"]["total"]) + " total", delta(last7("new_accounts"), prev7("new_accounts")), n);
  n = dailySeries("journal_hours");
  kpi({398, T, 583, T + KH}, "HOURS BUILT 7D", fmt(last7("journal_hours")), "H",
      fmt(last7("journal_entries")) + " entries", delta(last7("journal_hours"), prev7("journal_hours")), n);
  n = dailySeries("ships_submitted");
  kpi({591, T, 788, T + KH}, "SHIPS 7D", fmt(last7("ships_submitted")), nullptr,
      fmt(last7("projects_created")) + " new projects", delta(last7("ships_submitted"), prev7("ships_submitted")), n);

  // funnel
  const float MT = T + KH + 10, MB = 360;
  Box in = section({12, MT, 390, MB}, "SIGNUP > BUILD FUNNEL", "ALL TIME");
  static const char* shortNames[][2] = {
      {"Joined (RSVP or sign-in)", "Joined"}, {"Identity verified", "Verified"},
      {"Created a project", "Made project"}, {"Wrote a journal entry", "Journaled"},
      {"Shipped a design", "Design shipped"}, {"Requested a grant", "Got grant"},
      {"Shipped a build", "Build shipped"},
  };
  struct Step { String label; long count; } steps[16];
  int ns = 0;
  for (JsonObject s : m["funnel"].as<JsonArray>()) {
    String l = s["label"].as<String>();
    if (l == "Signed in with Hack Club" || ns >= 16) continue;
    for (auto& sn : shortNames)
      if (l == sn[0]) l = sn[1];
    steps[ns++] = {l.substring(0, 16), s["count"].as<long>()};
  }
  if (ns) {
    float top = steps[0].count ? steps[0].count : 1;
    float rh = (in.y1 - in.y0) / ns;
    const float lw = 112, nw = 46, cw = 34;
    float bx0 = in.x0 + lw, bx1 = in.x1 - nw - cw;
    for (int i = 0; i < ns; i++) {
      float cy = in.y0 + i * rh + rh / 2;
      float lx = in.x0 + text(in.x0, cy, steps[i].label, SANS13M, BLACK, "lm");
      dots(max(bx0, lx + 4), bx1, (int)cy, 4);
      float w = (bx1 - bx0) * steps[i].count / top;
      if (steps[i].count) fill(bx0, cy - rh / 2 + 3, bx0 + max(2.0f, w), cy + rh / 2 - 3);
      text(bx1 + nw - 2, cy, thousands(steps[i].count), MONO13B, BLACK, "rm");
      if (i) {
        long prev = steps[i - 1].count;
        text(in.x1, cy, prev ? pct(steps[i].count, prev) : "-", MONO11R, BLACK, "rm");
      }
    }
  }

  // review ops
  JsonObject rv = m["review"];
  in = section({398, MT, 583, MB}, "REVIEW OPS");
  String pending = String(qs["pending"].as<long>());
  float px = in.x0 + text(in.x0, in.y0 - 2, pending, BIG40) + 6;
  text(px, in.y0 + 8, "IN QUEUE", PIXB8);
  text(px, in.y0 + 22, String(qs["designs"].as<long>()) + " DESIGN " + qs["builds"].as<long>() + " BUILD", PIX8);
  float y = in.y0 + 52;
  long staleN = qs["stale"];
  String lines[4][2] = {
      {"oldest waiting", String(rv["oldest_pending_days"] | 0) + "d"},
      {"stale (>3d)", String(staleN)},
      {"resubmissions", String(qs["resubmissions"].as<long>())},
      {"median / p90", String(lroundf(rv["median_hours"] | 0.0f)) + "h/" + lroundf(rv["p90_hours"] | 0.0f) + "h"},
  };
  for (auto& l : lines) {
    text(in.x0, y, l[0], SANS12M);
    text(in.x1, y, l[1], MONO12B, BLACK, "ra");
    y += 15;
  }
  if (staleN) outline(in.x0 - 3, in.y0 + 52 + 15 - 1, in.x1 + 3, in.y0 + 52 + 30 - 1);
  JsonObject dec = rv["decisions"];
  long ok = dec["approved"] | 0, back = dec["returned"] | 0, no = dec["rejected"] | 0, tot = ok + back + no;
  y += 6;
  text(in.x0, y, "DECISIONS 30D", PIXB8);
  text(in.x1, y, fmt(tot), PIXB8, BLACK, "ra");
  y += 12;
  float bx = in.x0;
  long parts[3] = {ok, back, no};
  for (int k = 0; k < 3; k++) {
    float w = (in.x1 - in.x0) * parts[k] / (tot ? tot : 1);
    if (w >= 1) {
      if (k == 0) fill(bx, y, bx + w - 1, y + 12);
      else outline(bx, y, bx + w - 1, y + 12);
      if (k == 1) hatch(bx, y, bx + w - 1, y + 12);
    }
    bx += w;
  }
  y += 17;
  text((in.x0 + in.x1) / 2, y, String(ok) + " OK  " + back + " BACK  " + no + " NO", PIX8, BLACK, "ma");

  // money + rewards
  JsonObject mo = m["money"];
  in = section({591, MT, 788, MB}, "MONEY + REWARDS");
  float issued = 0, outD = 0;
  long gcount = 0;
  for (JsonObject g : D.grants["by_status"].as<JsonArray>()) {
    if (g["status"] != "cancelled") { issued += g["dollars"].as<float>(); gcount += g["count"].as<long>(); }
    outD += g["outstanding_dollars"] | 0.0f;
  }
  text(in.x0, in.y0 - 2, "$" + fmt(issued), BIG40);
  text(in.x1, in.y0 + 8, "GRANTED", PIXB8, BLACK, "ra");
  text(in.x1, in.y0 + 22, String(gcount) + " GRANTS", PIX8, BLACK, "ra");
  y = in.y0 + 52;
  JsonObject req = D.grants["requested"];
  String waiting = String(req["count"].as<long>());
  if (!req["oldest_days"].isNull()) waiting += " / " + String(req["oldest_days"].as<long>()) + "d";
  String mlines[4][2] = {
      {"outstanding", "$" + fmt(outD)},
      {"requests waiting", waiting},
      {"scraps awarded", fmt(mo["scraps_awarded"])},
      {"chips held", fmt(mo["chips_outstanding"])},
  };
  for (auto& l : mlines) {
    text(in.x0, y, l[0], SANS12M);
    text(in.x1, y, l[1], MONO12B, BLACK, "ra");
    y += 15;
  }
  y += 6;
  float aw = mo["scraps_awarded"].as<float>();
  if (!aw) aw = 1;
  text(in.x0, y, "SPENT " + fmt(mo["scraps_spent"]), PIXB8);
  text(in.x1, y, "UNSPENT " + fmt(mo["scraps_outstanding"]), PIXB8, BLACK, "ra");
  y += 12;
  float sw = (in.x1 - in.x0) * mo["scraps_spent"].as<float>() / aw;
  outline(in.x0, y, in.x1, y + 12);
  if (sw >= 1) fill(in.x0, y, in.x0 + sw, y + 12);
  hatch(in.x0 + sw + 1, y + 1, in.x1 - 1, y + 11);
  text((in.x0 + in.x1) / 2, y + 17, pct(mo["scraps_spent"].as<float>(), aw) + " OF SCRAPS -> PARTS", PIX8, BLACK, "ma");

  // countries
  const float BT = MB + 10, BB = 470;
  JsonObject au = m["audience"];
  JsonArray ctry = au["countries"];
  in = section({12, BT, 390, BB}, "WHERE MAKERS ARE", String(ctry.size()) + " COUNTRIES");
  float cmax = ctry.size() ? ctry[0]["value"].as<float>() : 1;
  float colw = (in.x1 - in.x0 - 12) / 2, rh = (in.y1 - in.y0) / 5;
  for (int i = 0; i < 10 && i < (int)ctry.size(); i++) {
    float cx = in.x0 + (i / 5) * (colw + 12), cy = in.y0 + (i % 5) * rh + rh / 2;
    text(cx, cy, ctry[i]["label"].as<String>(), MONO12B, BLACK, "lm");
    float b0 = cx + 22, b1 = cx + colw - 34;
    fill(b0, cy - 4, b0 + max(1.0f, (b1 - b0) * ctry[i]["value"].as<float>() / cmax), cy + 4);
    text(cx + colw, cy, fmt(ctry[i]["value"]), MONO12R, BLACK, "rm");
  }

  // audience
  float total = au["total"];
  in = section({398, BT, 788, BB}, "AUDIENCE", fmt(total) + " HCA USERS");
  float verified = 0, teens = 0, groups = 0;
  for (JsonObject v : au["verification"].as<JsonArray>())
    if (v["label"] == "verified") verified = v["value"];
  for (JsonObject g : au["groups"].as<JsonArray>()) {
    groups += g["value"].as<float>();
    if (g["label"] == "Teen") teens = g["value"];
  }
  String stats[3][2] = {
      {pct(au["ysws_eligible"], total), "YSWS ELIGIBLE"},
      {pct(verified, total), "ID VERIFIED"},
      {pct(teens, groups), "TEENS"},
  };
  y = in.y0 - 2;
  for (auto& s : stats) {
    text(in.x0, y, s[0], BIG18);
    text(in.x0 + 58, y + 6, s[1], PIXB8);
    y += 22;
  }
  JsonArray ages = au["ages"];
  float hx0 = in.x0 + 160, hx1 = in.x1, hy0 = in.y0 - 4, hy1 = in.y1 - 12, amax = 1;
  for (JsonObject a : ages) amax = max(amax, a["value"].as<float>());
  float abw = ages.size() ? (hx1 - hx0) / ages.size() : 1;
  int i = 0;
  for (JsonObject a : ages) {
    float b0 = hx0 + i * abw + 3, b1 = hx0 + (i + 1) * abw - 3, v = a["value"];
    float h = (hy1 - hy0 - 10) * v / amax;
    if (v == amax) fill(b0, hy1 - h, b1, hy1);
    else { outline(b0, hy1 - h, b1, hy1); hatch(b0, hy1 - h, b1, hy1); }
    text((b0 + b1) / 2, hy1 + 3, a["label"].as<String>(), PIX8, BLACK, "ma");
    i++;
  }
  text(hx0 - 6, hy1 + 3, "AGE", PIXB8, BLACK, "ra");
}

// keep the last good screen but say it is out of date
void drawOfflineBadge(const String& why) {
  using namespace dash;
  memcpy(fb, prevFrame, FRAME_BYTES);
  fill(250, 4, 520, 34);
  outline(252, 6, 518, 32);
  text(385, 10, "OFFLINE - SHOWING LAST DATA", PIXB8, WHITE, "ma");
  text(385, 21, upper(why).substring(0, 40), PIX8, WHITE, "ma");
}

// ---------- pull ----------

bool fabricatePull() {
  applyTz();
  if (cfg.fabToken.isEmpty()) {
    lastPullResult = "no fab_token";
    rtcFails++;
    rtcNextOverride = 0;
    return false;
  }
  uint32_t t0 = millis();
  WiFiClientSecure tls;
  tls.setCACert(FABRICATE_ROOTS);
  HTTPClient http;
  http.setReuse(true);
  DashData* D = new DashData();
  String date, err;
  struct { const char* name; const char* args; JsonDocument* out; } calls[] = {
      {"get_metrics", "{\"section\":\"all\",\"range\":\"30d\"}", &D->metrics},
      {"get_program_overview", "{}", &D->overview},
      {"get_daily_activity", "{\"days\":30}", &D->daily},
      {"get_review_queue", "{\"limit\":1}", &D->queue},
      {"get_grant_summary", "{\"limit\":1}", &D->grants},
  };
  for (auto& c : calls) {
    err = mcpCall(http, tls, c.name, c.args, *c.out, date.isEmpty() ? &date : nullptr);
    if (err.length()) break;
  }
  bool changed = false;
  if (err.isEmpty()) {
    setClockFromHttpDate(date);
    renderDashboard(*D);
    bool same = prevValid && memcmp(fb, prevFrame, FRAME_BYTES) == 0;
    if (!same) { showFrame(REFRESH_AUTO); changed = true; }
    rtcDashShown = true;
    rtcFails = 0;
    time_t now = time(nullptr);
    tm lt;
    localtime_r(&now, &lt);
    rtcNextOverride = 3600 - (lt.tm_min * 60 + lt.tm_sec) + FAB_WAKE_AFTER_HOUR_S;
    lastPullResult = String("fabricate ok ") + (millis() - t0) + "ms";
  } else {
    lastPullResult = "fabricate: " + err;
    rtcFails++;
    rtcNextOverride = 0;  // pull() back-off
    if (rtcFails == 1 && rtcDashShown && prevValid) {
      drawOfflineBadge(err);
      showFrame(REFRESH_PARTIAL);
      changed = true;
    }
  }
  delete D;
  Serial.printf("pull: %s\n", lastPullResult.c_str());
  return changed;
}
