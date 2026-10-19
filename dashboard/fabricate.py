#!/usr/bin/env python3
"""fabricate - Fabricate program metrics dashboard for the reTerminal E1001 (800x480, 1-bit).

  fabricate.py render out.png          # fetch metrics once and write the screen
  fabricate.py render out.png --cache  # re-render from the last fetched data (no network)
  fabricate.py serve [--port 8080]     # pull-mode server: GET /fabricate.png

Data comes from the Fabricate admin MCP (https://fabricate.hackclub.com/mcp), called directly
over HTTP. The token is read from $FABRICATE_MCP_TOKEN or ~/.config/fabricate-epd/token.

In serve mode the device is told to sleep until just after the next full hour, and metrics are
re-fetched when it wakes (at most every 5 minutes). If a fetch fails the last good data is drawn
with a STALE marker, so the screen never goes blank.
"""
import argparse
import datetime as dt
import hashlib
import http.server
import io
import json
import os
import sys
import threading
import time
import urllib.request

from PIL import Image, ImageDraw, ImageFont

W, H = 800, 480
HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "fonts")
CACHE = os.path.expanduser("~/.cache/fabricate-epd/data.json")
MCP_URL = os.environ.get("FABRICATE_MCP_URL", "https://fabricate.hackclub.com/mcp")
REFETCH_S = 300
WAKE_AFTER_HOUR_S = 20  # wake a little after the hour so the clock in the header reads :00


# ---------------------------------------------------------------- data

def token():
    t = os.environ.get("FABRICATE_MCP_TOKEN")
    if not t:
        p = os.path.expanduser("~/.config/fabricate-epd/token")
        if os.path.exists(p):
            t = open(p).read().strip()
    if not t:
        sys.exit("no token: set FABRICATE_MCP_TOKEN or write it to ~/.config/fabricate-epd/token")
    return t


def mcp(name, args=None):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args or {}}}).encode()
    req = urllib.request.Request(MCP_URL, body, {
        "Authorization": "Bearer " + token(),
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
    })
    raw = urllib.request.urlopen(req, timeout=60).read().decode()
    if raw.startswith("event:") or raw.startswith("data:"):  # SSE framing
        raw = next(l[5:] for l in raw.splitlines() if l.startswith("data:"))
    r = json.loads(raw)
    if "error" in r:
        raise RuntimeError(f"{name}: {r['error']}")
    res = r["result"]
    if res.get("isError"):
        raise RuntimeError(f"{name}: {res['content'][0]['text']}")
    return res.get("structuredContent") or json.loads(res["content"][0]["text"])


def fetch():
    calls = {
        "metrics": ("get_metrics", {"section": "all", "range": "30d"}),
        "overview": ("get_program_overview", {}),
        "daily": ("get_daily_activity", {"days": 30}),
        "queue": ("get_review_queue", {"limit": 1}),
        "grants": ("get_grant_summary", {"limit": 1}),
    }
    out, errs = {}, []

    def run(k, name, args):
        try:
            out[k] = mcp(name, args)
        except Exception as e:  # noqa: BLE001 - any failure falls back to the cache
            errs.append(f"{name}: {e}")

    ts = [threading.Thread(target=run, args=(k, *v)) for k, v in calls.items()]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    if errs:
        raise RuntimeError("; ".join(errs))
    out["fetched_at"] = time.time()
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE + ".tmp", "w") as f:
        json.dump(out, f)
    os.replace(CACHE + ".tmp", CACHE)
    return out


def load_cache():
    with open(CACHE) as f:
        return json.load(f)


# ---------------------------------------------------------------- drawing helpers

_fonts = {}


def font(name, size, var=None):
    key = (name, size, var)
    if key not in _fonts:
        f = ImageFont.truetype(os.path.join(FONTS, name), size)
        if var:
            f.set_variation_by_name(var)
        _fonts[key] = f
    return _fonts[key]


def BIG(s): return font("ArchivoBlack-Regular.ttf", s)
def PIX(s=16): return font("Silkscreen-Regular.ttf", s)
def PIXB(s=16): return font("Silkscreen-Bold.ttf", s)
def MONO(s, w="Bold"): return font("JetBrainsMono.ttf", s, w)
def SANS(s, w="Bold"): return font("SpaceGrotesk.ttf", s, w)


def fmt(n):
    n = float(n)
    if abs(n) >= 10000:
        return f"{n / 1000:.0f}k"
    if abs(n) >= 1000:
        return f"{n / 1000:.1f}k"
    return f"{n:.0f}" if n == int(n) or abs(n) >= 100 else f"{n:.1f}"


def pct(a, b):
    return f"{100 * a / b:.0f}%" if b else "-"


def delta(cur, prev):
    if not prev:
        return "new" if cur else "flat"
    d = (cur - prev) / prev * 100
    return f"{'+' if d >= 0 else ''}{d:.0f}%"


class Canvas:
    def __init__(self):
        self.img = Image.new("L", (W, H), 255)
        self.d = ImageDraw.Draw(self.img)

    def text(self, xy, s, f, fill=0, anchor="la"):
        self.d.text(xy, s, font=f, fill=fill, anchor=anchor)
        return self.d.textlength(s, font=f)

    def tw(self, s, f):
        return self.d.textlength(s, font=f)

    def rect(self, box, fill=None, outline=None, width=1):
        self.d.rectangle(box, fill=fill, outline=outline, width=width)

    def hatch(self, box, step=3):
        """1-bit 'grey': diagonal lines, reads as a lighter tone than solid black."""
        x0, y0, x1, y1 = [int(v) for v in box]
        for y in range(y0, y1 + 1):
            for x in range(x0 + (step - (x0 + y) % step) % step, x1 + 1, step):
                self.d.point((x, y), 0)

    def dots(self, x0, x1, y, gap=3):
        for x in range(int(x0), int(x1) + 1, gap):
            self.d.point((x, y), 0)

    def panel(self, box, title, right=None):
        """Bordered panel with a black title tab."""
        x0, y0, x1, y1 = box
        self.rect(box, outline=0, width=2)
        tw = self.tw(title, PIXB(8)) + 12
        self.rect((x0, y0, x0 + tw, y0 + 13), fill=0)
        self.text((x0 + 6, y0 + 3), title, PIXB(8), fill=255)
        if right:
            self.text((x1 - 6, y0 + 4), right, PIX(8), anchor="ra")
        return (x0 + 8, y0 + 20, x1 - 8, y1 - 6)

    def bars(self, box, values, highlight_last=True, gap=1):
        """Mini column chart; the newest bar is solid, the rest hatched."""
        x0, y0, x1, y1 = box
        if not values:
            return
        n = len(values)
        top = max(values) or 1
        bw = (x1 - x0 + gap) / n
        for i, v in enumerate(values):
            bx0 = x0 + i * bw
            bx1 = bx0 + bw - gap - 1
            h = 0 if v <= 0 else max(1, (y1 - y0) * v / top)
            if h:
                b = (bx0, y1 - h, bx1, y1)
                if highlight_last and i == n - 1:
                    self.rect(b, fill=0)
                else:
                    self.rect(b, outline=0)
                    self.hatch(b, 2)
        self.d.line((x0, y1 + 1, x1, y1 + 1), fill=0)


# ---------------------------------------------------------------- the screen

def render(data, stale=False, now=None):
    m, ov, daily = data["metrics"], data["overview"], data["daily"]["series"]
    queue, grants = data["queue"], data["grants"]
    now = now or dt.datetime.now()
    c = Canvas()

    # header -------------------------------------------------------------
    c.rect((0, 0, W, 38), fill=0)
    x = 14 + c.text((14, 6), "FABRICATE", BIG(22), fill=255)
    c.text((x + 10, 15), "HACK CLUB YSWS / ADMIN METRICS", PIX(8), fill=255)
    fetched = dt.datetime.fromtimestamp(data["fetched_at"])
    right = now.strftime("%a %d %b  %H:%M").upper()
    c.text((W - 14, 9), right, MONO(18), fill=255, anchor="ra")
    nxt = (now.replace(minute=0, second=0, microsecond=0) + dt.timedelta(hours=1)).strftime("%H:%M")
    sub = f"DATA {fetched:%H:%M}  NEXT {nxt}"
    if stale:
        sub = f"STALE! LAST GOOD {fetched:%d %b %H:%M}"
    c.text((W - 14 - c.tw(right, MONO(18)) - 12, 16), sub, PIX(8), fill=255, anchor="ra")

    # KPI row ------------------------------------------------------------
    ser = m["activity"]["series"][-30:]
    d14 = daily[-14:]
    last7 = lambda k: sum(r[k] for r in d14[-7:])
    prev7 = lambda k: sum(r[k] for r in d14[:7])

    def kpi(box, label, value, unit, sub, chg, series):
        x0, y0, x1, y1 = box
        inner = c.panel(box, label, chg)
        ix0, iy0, ix1, iy1 = inner
        vx = ix0 + c.text((ix0, iy0 - 2), value, BIG(40))
        if unit:
            c.text((vx + 4, iy0 + 30), unit, PIXB(8))
        c.text((ix0, iy0 + 50), sub, MONO(12, "Medium"))
        c.bars((ix0, iy0 + 68, ix1, iy1 - 1), series)

    T = 46
    KH = 128
    cols = [(12, 197), (205, 390), (398, 583), (591, 788)]
    dau = m["activity"]["dau"]
    kpi((*cols[0][:1], T, cols[0][1], T + KH), "ACTIVE MAKERS", fmt(dau), "DAU",
        f"WAU {fmt(m['activity']['wau'])}  MAU {fmt(m['activity']['mau'])}",
        f"{m['activity']['stickiness'] * 100:.0f}% STICKY", [r["users"] for r in ser])
    kpi((cols[1][0], T, cols[1][1], T + KH), "SIGNUPS 7D", fmt(last7("new_accounts")), None,
        f"{fmt(ov['users']['total'])} total", delta(last7("new_accounts"), prev7("new_accounts")),
        [r["new_accounts"] for r in daily])
    kpi((cols[2][0], T, cols[2][1], T + KH), "HOURS BUILT 7D", fmt(last7("journal_hours")), "H",
        f"{fmt(last7('journal_entries'))} entries", delta(last7("journal_hours"), prev7("journal_hours")),
        [r["journal_hours"] for r in daily])
    kpi((cols[3][0], T, cols[3][1], T + KH), "SHIPS 7D", fmt(last7("ships_submitted")), None,
        f"{fmt(last7('projects_created'))} new projects", delta(last7("ships_submitted"), prev7("ships_submitted")),
        [r["ships_submitted"] for r in daily])

    # funnel -------------------------------------------------------------
    MT, MB = T + KH + 10, 360
    ix0, iy0, ix1, iy1 = c.panel((12, MT, 390, MB), "SIGNUP > BUILD FUNNEL", "ALL TIME")
    steps = [s for s in m["funnel"] if s["label"] != "Signed in with Hack Club"]
    short = {
        "Joined (RSVP or sign-in)": "Joined",
        "Identity verified": "Verified",
        "Created a project": "Made project",
        "Wrote a journal entry": "Journaled",
        "Shipped a design": "Design shipped",
        "Design approved": "Design approved",
        "Requested a grant": "Got grant",
        "Shipped a build": "Build shipped",
        "Build approved": "Build approved",
    }
    top = steps[0]["count"] or 1
    rh = (iy1 - iy0) / len(steps)
    lw, nw, cw = 112, 46, 34
    bx0, bx1 = ix0 + lw, ix1 - nw - cw
    for i, s in enumerate(steps):
        y = iy0 + i * rh
        cy = y + rh / 2
        lx = ix0 + c.text((ix0, cy), short.get(s["label"], s["label"])[:16], SANS(13, "Medium"), anchor="lm")
        c.dots(max(bx0, lx + 4), bx1, int(cy), 4)
        w = (bx1 - bx0) * s["count"] / top
        if s["count"]:
            c.rect((bx0, cy - rh / 2 + 3, bx0 + max(2, w), cy + rh / 2 - 3), fill=0)
        c.text((bx1 + nw - 2, cy), f"{s['count']:,}", MONO(13), anchor="rm")
        if i:
            prev = steps[i - 1]["count"]
            c.text((ix1, cy), pct(s["count"], prev) if prev else "-", MONO(11, "Regular"), anchor="rm")

    # review ops ---------------------------------------------------------
    rv = m["review"]
    ix0, iy0, ix1, iy1 = c.panel((398, MT, 583, MB), "REVIEW OPS")
    c.text((ix0, iy0 - 2), str(queue["summary"]["pending"]), BIG(40))
    c.text((ix0 + c.tw(str(queue["summary"]["pending"]), BIG(40)) + 6, iy0 + 8), "IN QUEUE", PIXB(8))
    c.text((ix0 + c.tw(str(queue["summary"]["pending"]), BIG(40)) + 6, iy0 + 22),
           f"{queue['summary']['designs']} DESIGN {queue['summary']['builds']} BUILD", PIX(8))
    y = iy0 + 52
    stale_n = queue["summary"]["stale"]
    lines = [
        ("oldest waiting", f"{rv['oldest_pending_days'] or 0}d"),
        ("stale (>3d)", str(stale_n)),
        ("resubmissions", str(queue["summary"]["resubmissions"])),
        ("median / p90", f"{rv['median_hours']:.0f}h/{rv['p90_hours']:.0f}h"),
    ]
    for k, v in lines:
        c.text((ix0, y), k, SANS(12, "Medium"))
        c.text((ix1, y), v, MONO(12), anchor="ra")
        y += 15
    if stale_n:  # box the line that needs attention
        c.rect((ix0 - 3, iy0 + 52 + 15 - 1, ix1 + 3, iy0 + 52 + 30 - 1), outline=0)
    # 30d decisions as one stacked bar: approved solid, returned hatched, rejected empty
    dec = rv["decisions"]
    tot = sum(dec.values()) or 1
    y += 6
    c.text((ix0, y), "DECISIONS 30D", PIXB(8))
    c.text((ix1, y), f"{fmt(sum(dec.values()))}", PIXB(8), anchor="ra")
    y += 12
    bx = ix0
    for k, style in (("approved", "solid"), ("returned", "hatch"), ("rejected", "empty")):
        w = (ix1 - ix0) * dec.get(k, 0) / tot
        if w >= 1:
            b = (bx, y, bx + w - 1, y + 12)
            c.rect(b, fill=0 if style == "solid" else None, outline=0)
            if style == "hatch":
                c.hatch(b, 2)
        bx += w
    y += 17
    legend = f"{dec.get('approved', 0)} OK  {dec.get('returned', 0)} BACK  {dec.get('rejected', 0)} NO"
    c.text(((ix0 + ix1) / 2, y), legend, PIX(8), anchor="ma")

    # money & rewards ----------------------------------------------------
    mo = m["money"]
    ix0, iy0, ix1, iy1 = c.panel((591, MT, 788, MB), "MONEY + REWARDS")
    issued = sum(g["dollars"] for g in grants["by_status"] if g["status"] != "cancelled")
    gcount = sum(g["count"] for g in grants["by_status"] if g["status"] != "cancelled")
    out_d = sum(g.get("outstanding_dollars", 0) for g in grants["by_status"])
    c.text((ix0, iy0 - 2), f"${fmt(issued)}", BIG(40))
    c.text((ix1, iy0 + 8), "GRANTED", PIXB(8), anchor="ra")
    c.text((ix1, iy0 + 22), f"{gcount} GRANTS", PIX(8), anchor="ra")
    y = iy0 + 52
    req = grants["requested"]
    lines = [
        ("outstanding", f"${fmt(out_d)}"),
        ("requests waiting", f"{req['count']}" + (f" / {req['oldest_days']}d" if req["oldest_days"] else "")),
        ("scraps awarded", fmt(mo["scraps_awarded"])),
        ("chips held", fmt(mo["chips_outstanding"])),
    ]
    for k, v in lines:
        c.text((ix0, y), k, SANS(12, "Medium"))
        c.text((ix1, y), v, MONO(12), anchor="ra")
        y += 15
    # scraps: spent vs unspent
    y += 6
    aw = mo["scraps_awarded"] or 1
    c.text((ix0, y), f"SPENT {fmt(mo['scraps_spent'])}", PIXB(8))
    c.text((ix1, y), f"UNSPENT {fmt(mo['scraps_outstanding'])}", PIXB(8), anchor="ra")
    y += 12
    sw = (ix1 - ix0) * mo["scraps_spent"] / aw
    c.rect((ix0, y, ix1, y + 12), outline=0)
    if sw >= 1:
        c.rect((ix0, y, ix0 + sw, y + 12), fill=0)
    c.hatch((ix0 + sw + 1, y + 1, ix1 - 1, y + 11), 2)
    c.text(((ix0 + ix1) / 2, y + 17), f"{pct(mo['scraps_spent'], aw)} OF SCRAPS -> PARTS", PIX(8), anchor="ma")

    # countries ----------------------------------------------------------
    BT, BB = MB + 10, 470
    au = m["audience"]
    ctry = au["countries"]
    ix0, iy0, ix1, iy1 = c.panel((12, BT, 390, BB), "WHERE MAKERS ARE", f"{len(ctry)} COUNTRIES")
    topc = ctry[:10]
    cmax = topc[0]["value"] if topc else 1
    colw = (ix1 - ix0 - 12) / 2
    rows = 5
    rh = (iy1 - iy0) / rows
    for i, r in enumerate(topc):
        cx = ix0 + (i // rows) * (colw + 12)
        cy = iy0 + (i % rows) * rh + rh / 2
        c.text((cx, cy), r["label"], MONO(12), anchor="lm")
        b0, b1 = cx + 22, cx + colw - 34
        w = (b1 - b0) * r["value"] / cmax
        c.rect((b0, cy - 4, b0 + max(1, w), cy + 4), fill=0)
        c.text((cx + colw, cy), fmt(r["value"]), MONO(12, "Regular"), anchor="rm")

    # audience -----------------------------------------------------------
    ix0, iy0, ix1, iy1 = c.panel((398, BT, 788, BB), "AUDIENCE", f"{fmt(au['total'])} HCA USERS")
    ver = {v["label"]: v["value"] for v in au["verification"]}
    teen = {g["label"]: g["value"] for g in au["groups"]}
    stats = [
        (pct(au["ysws_eligible"], au["total"]), "YSWS ELIGIBLE"),
        (pct(ver.get("verified", 0), au["total"]), "ID VERIFIED"),
        (pct(teen.get("Teen", 0), sum(teen.values())), "TEENS"),
    ]
    y = iy0 - 2
    for v, k in stats:
        c.text((ix0, y), v, BIG(18))
        c.text((ix0 + 58, y + 6), k, PIXB(8))
        y += 22
    # age histogram
    ages = au["ages"]
    hx0, hx1 = ix0 + 160, ix1
    hy0, hy1 = iy0 - 4, iy1 - 12
    amax = max(a["value"] for a in ages) or 1
    bw = (hx1 - hx0) / len(ages)
    for i, a in enumerate(ages):
        bx0 = hx0 + i * bw + 3
        bx1 = hx0 + (i + 1) * bw - 3
        h = (hy1 - hy0 - 10) * a["value"] / amax
        b = (bx0, hy1 - h, bx1, hy1)
        if a["value"] == amax:
            c.rect(b, fill=0)
        else:
            c.rect(b, outline=0)
            c.hatch(b, 2)
        c.text(((bx0 + bx1) / 2, hy1 + 3), a["label"], PIX(8), anchor="ma")
    c.text((hx0 - 6, hy1 + 3), "AGE", PIXB(8), anchor="ra")

    return c.img.point(lambda v: 255 if v >= 140 else 0, mode="1")


# ---------------------------------------------------------------- serving

def current(force=False):
    """Fresh data if possible, otherwise the cache flagged as stale."""
    try:
        data = load_cache()
    except (OSError, ValueError):
        data = None
    if force or not data or time.time() - data["fetched_at"] > REFETCH_S:
        try:
            return fetch(), False
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"fetch failed: {e}\n")
            if not data:
                raise
            return data, True
    return data, False


def seconds_to_next_hour():
    now = dt.datetime.now()
    nxt = now.replace(minute=0, second=0, microsecond=0) + dt.timedelta(hours=1)
    return int((nxt - now).total_seconds()) + WAKE_AFTER_HOUR_S


def cmd_render(args):
    if args.cache:
        data, stale = load_cache(), False
    else:
        data, stale = current(force=True)
    render(data, stale).save(args.out)
    print("wrote", args.out)


def cmd_serve(args):
    lock = threading.Lock()

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.split("?")[0] not in ("/", "/fabricate.png", "/screen.png"):
                self.send_error(404)
                return
            with lock:
                data, stale = current()
                buf = io.BytesIO()
                render(data, stale).save(buf, "PNG")
            body = buf.getvalue()
            etag = '"' + hashlib.sha1(body).hexdigest()[:16] + '"'
            if self.headers.get("X-Battery-mV"):
                sys.stderr.write(f"  device battery {self.headers.get('X-Battery-mV')} mV "
                                 f"({self.headers.get('X-Battery-Pct')}%), rssi {self.headers.get('X-RSSI')}, "
                                 f"wake {self.headers.get('X-Wake-Reason')}\n")
            # a failed fetch retries in 10 min instead of an hour
            nxt = 600 if stale else seconds_to_next_hour()
            unchanged = self.headers.get("If-None-Match") == etag
            self.send_response(304 if unchanged else 200)
            self.send_header("ETag", etag)
            self.send_header("X-Next-Refresh", str(nxt))
            if unchanged:
                self.end_headers()
                return
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, f, *a):
            sys.stderr.write(f"{self.log_date_time_string()} {self.address_string()} {f % a}\n")

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), H)
    print(f"serving /fabricate.png on :{args.port}", flush=True)
    srv.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("render", help="fetch and write the screen to a PNG")
    p.add_argument("out")
    p.add_argument("--cache", action="store_true", help="use the last fetched data, no network")
    p.set_defaults(fn=cmd_render)
    p = sp.add_parser("serve", help="pull-mode server for the device")
    p.add_argument("--port", type=int, default=8080)
    p.set_defaults(fn=cmd_serve)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
