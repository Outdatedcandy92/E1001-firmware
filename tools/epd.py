#!/usr/bin/env python3
"""epd - control a reTerminal E1001 running the custom firmware.

Push (device awake: on USB power, or within the awake window after a button press):
  epd.py push photo.jpg            # fit + threshold to pure 1-bit (no dithering)
  epd.py push photo.jpg --dither   # Floyd-Steinberg, only when you want it
  epd.py text "Hello" --size 96    # render text locally and push it
  epd.py status                    # battery, rssi, config, ...
  epd.py config interval=900 image_url=http://host:8080/screen.png
  epd.py refresh | sleep | clear | reboot
  epd.py ota firmware.bin
  epd.py screenshot now.png        # what is on the glass right now

Pull (device on battery, wakes on its own schedule):
  epd.py serve ./screens            # serves screen.png with ETag; 304 when unchanged
  epd.py render-dashboard out.png   # example dashboard renderer

Device address: --host or $EPD_HOST (default reterminal.local).
"""
import argparse
import hashlib
import http.server
import io
import json
import os
import sys
import time
import urllib.error
import urllib.request

from PIL import Image, ImageDraw, ImageFont, ImageOps

W, H = 800, 480
FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
]


def font(size):
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default(size)


def to_1bit(img, dither=False, threshold=128, fit="contain", invert=False):
    """Scale to 800x480 and convert to pure black/white. Solid colours stay solid."""
    img = ImageOps.exif_transpose(img).convert("L")
    if img.size != (W, H):
        if fit == "cover":
            img = ImageOps.fit(img, (W, H), Image.LANCZOS)
        elif fit == "stretch":
            img = img.resize((W, H), Image.LANCZOS)
        else:
            img = ImageOps.pad(img, (W, H), Image.LANCZOS, color=255)
    if invert:
        img = ImageOps.invert(img)
    if dither:
        return img.convert("1", dither=Image.FLOYDSTEINBERG)
    return img.point(lambda v: 255 if v >= threshold else 0, mode="1")


def pack(img1):
    """Pack to the device wire format: 48000 bytes, row-major, MSB first, bit=1 means BLACK."""
    return ImageOps.invert(img1.convert("L")).convert("1").tobytes()


def host_url(args, path):
    host = args.host or os.environ.get("EPD_HOST", "reterminal.local")
    return f"http://{host}{path}"


def req(args, path, data=None, ctype="application/octet-stream", method=None, timeout=30, headers=None):
    r = urllib.request.Request(host_url(args, path), data=data, method=method or ("POST" if data is not None else "GET"),
                               headers=headers or {})
    if data is not None:
        r.add_header("Content-Type", ctype)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        sys.exit(f"device said {e.code}: {e.read().decode(errors='replace')}")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        sys.exit(f"can't reach {r.host} ({getattr(e, 'reason', e)}). On battery the device sleeps between pulls:\n"
                 "press a button to wake it, or run `serve --stay-awake 300` so it stays up after its next pull.")


def cmd_push(args):
    img = to_1bit(Image.open(args.image), args.dither, args.threshold, args.fit, args.invert)
    if args.preview:
        img.save(args.preview)
    print(req(args, "/image", pack(img), headers={"X-Mode": args.mode}))


def render_text(text, size, align="center"):
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    f = font(size)
    # shrink until it fits
    while size > 10:
        box = d.multiline_textbbox((0, 0), text, font=f, align=align, spacing=size // 4)
        if box[2] - box[0] <= W - 40 and box[3] - box[1] <= H - 40:
            break
        size -= 4
        f = font(size)
    box = d.multiline_textbbox((0, 0), text, font=f, align=align, spacing=size // 4)
    x = (W - (box[2] - box[0])) / 2 - box[0]
    y = (H - (box[3] - box[1])) / 2 - box[1]
    d.multiline_text((x, y), text, font=f, fill=0, align=align, spacing=size // 4)
    return img.point(lambda v: 255 if v >= 128 else 0, mode="1")


def cmd_text(args):
    text = args.text.replace("\\n", "\n")
    img = render_text(text, args.size)
    if args.preview:
        img.save(args.preview)
    print(req(args, "/image", pack(img), headers={"X-Mode": args.mode}))


def cmd_status(args):
    print(json.dumps(json.loads(req(args, "/status")), indent=2))


def cmd_config(args):
    cfg = {}
    for kv in args.pairs:
        k, _, v = kv.partition("=")
        cfg[k] = int(v) if v.lstrip("-").isdigit() else v
    print(req(args, "/config", json.dumps(cfg).encode(), "application/json"))


def cmd_simple(args):
    print(req(args, "/" + args.cmd, b""))


def cmd_ota(args):
    with open(args.firmware, "rb") as f:
        print(req(args, "/ota", f.read(), timeout=180))


def cmd_screenshot(args):
    r = urllib.request.urlopen(host_url(args, "/screen.bin"), timeout=30).read()
    img = Image.frombytes("1", (W, H), r)
    ImageOps.invert(img.convert("L")).save(args.out)
    print("wrote", args.out)


def render_dashboard(path=None):
    """Example pull-mode screen: big clock/date + whatever you want to add."""
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    now = time.localtime()
    d.rectangle((0, 0, W, 90), fill=0)
    d.text((30, 18), time.strftime("%A", now), font=font(48), fill=255)
    d.text((W - 30, 18), time.strftime("%b %d", now), font=font(48), fill=255, anchor="ra")
    d.text((W / 2, 260), time.strftime("%H:%M", now), font=font(200), fill=0, anchor="mm")
    d.text((W / 2, 440), time.strftime("%Y-%m-%d", now), font=font(24), fill=0, anchor="mm")
    img = img.point(lambda v: 255 if v >= 128 else 0, mode="1")
    if path:
        img.save(path)
    return img


def cmd_render_dashboard(args):
    render_dashboard(args.out)
    print("wrote", args.out)


def cmd_serve(args):
    """Serve <dir>/screen.png (or a live dashboard) to devices in pull mode, with ETag/304 support."""
    root = os.path.abspath(args.dir)

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            name = self.path.split("?")[0].lstrip("/") or "screen.png"
            if name == "dashboard.png":
                buf = io.BytesIO()
                render_dashboard().save(buf, "PNG")
                body = buf.getvalue()
            else:
                p = os.path.abspath(os.path.join(root, name))
                if not p.startswith(root) or not os.path.isfile(p):
                    self.send_error(404)
                    return
                buf = io.BytesIO()
                to_1bit(Image.open(p)).save(buf, "PNG")
                body = buf.getvalue()
            etag = '"' + hashlib.sha1(body).hexdigest()[:16] + '"'
            batt = self.headers.get("X-Battery-mV")
            if batt:
                sys.stderr.write(f"  device {self.headers.get('X-Device-Id')} battery {batt} mV, "
                                 f"rssi {self.headers.get('X-RSSI')}, wake {self.headers.get('X-Wake-Reason')}\n")
            nxt = args.next
            if name == "dashboard.png" and not nxt:
                nxt = 61 - time.localtime().tm_sec  # wake just after the minute ticks over
            unchanged = self.headers.get("If-None-Match") == etag
            self.send_response(304 if unchanged else 200)
            self.send_header("ETag", etag)
            if nxt:
                self.send_header("X-Next-Refresh", str(nxt))
            if args.stay_awake:
                self.send_header("X-Stay-Awake", str(args.stay_awake))
            if unchanged:
                self.end_headers()
                return
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), H)
    print(f"serving {root} on :{args.port}  (screen.png, dashboard.png)")
    srv.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="device host/IP (default $EPD_HOST or reterminal.local)")
    sp = ap.add_subparsers(dest="cmd", required=True)

    p = sp.add_parser("push", help="push an image file")
    p.add_argument("image")
    p.add_argument("--dither", action="store_true")
    p.add_argument("--threshold", type=int, default=128)
    p.add_argument("--fit", choices=["contain", "cover", "stretch"], default="contain")
    p.add_argument("--invert", action="store_true")
    p.add_argument("--mode", choices=["auto", "full", "fast", "partial"], default="auto")
    p.add_argument("--preview", help="also save the 1-bit result here")
    p.set_defaults(fn=cmd_push)

    p = sp.add_parser("text", help="render and push text")
    p.add_argument("text")
    p.add_argument("--size", type=int, default=96)
    p.add_argument("--mode", choices=["auto", "full", "fast", "partial"], default="auto")
    p.add_argument("--preview")
    p.set_defaults(fn=cmd_text)

    sp.add_parser("status").set_defaults(fn=cmd_status)
    p = sp.add_parser("config", help="set config: key=value ...")
    p.add_argument("pairs", nargs="*")
    p.set_defaults(fn=cmd_config)
    for c in ("refresh", "sleep", "clear", "reboot"):
        sp.add_parser(c).set_defaults(fn=cmd_simple)
    p = sp.add_parser("ota", help="upload new firmware over WiFi")
    p.add_argument("firmware")
    p.set_defaults(fn=cmd_ota)
    p = sp.add_parser("screenshot", help="save what is currently on the glass")
    p.add_argument("out")
    p.set_defaults(fn=cmd_screenshot)
    p = sp.add_parser("serve", help="pull-mode image server")
    p.add_argument("dir", nargs="?", default=".")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--next", type=int, help="tell the device how many seconds to sleep before next fetch")
    p.add_argument("--stay-awake", type=int, help="tell the device to keep its HTTP API up this many seconds after a pull")
    p.set_defaults(fn=cmd_serve)
    p = sp.add_parser("render-dashboard")
    p.add_argument("out")
    p.set_defaults(fn=cmd_render_dashboard)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
