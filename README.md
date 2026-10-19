# reTerminal E1001 custom firmware

Replaces the stock SenseCraft firmware on the Seeed reTerminal E1001 (ESP32-S3, 7.5" 800×480 B/W e-paper).

- **Push** images/text straight to the device over WiFi: web UI at `http://reterminal.local`, a CLI, or plain `curl`.
- **Pull** on any schedule (seconds to days, not just 5 minutes). The server can change the next wake per response, and an unchanged image (ETag/304) costs a ~1–2 s wake with no redraw.
- **True black**: everything is hard-thresholded to 1-bit. Nothing on the device dithers; `--dither` exists for photos if you want it.
- **Battery**: deep sleep between pulls (~14 µA board), fast WiFi reconnect from cached BSSID/channel, peripherals powered down, exponential back-off when WiFi/server is down, low-battery screen and stop below 3.4 V.
- **Refresh control**: `full` (real temperature-compensated OTP waveform, deepest blacks), `fast` (short full refresh), `partial` (~0.5 s, no flash), `auto` (partial, with a full every `full_every` updates to clear ghosting). Partial refresh works even right after deep sleep: the last frame is kept in flash.
- **OTA** firmware updates over WiFi, AP setup mode if WiFi fails, a serial console, and on-device info screen with battery/temperature/humidity.

## Buttons

| Button | Action (also wakes from sleep) |
|---|---|
| Green (top) | Fetch `url` now |
| Left white | Info screen (address, battery, room temp, config) |
| Right white | Just stay awake for `awake` seconds (push window) |

On USB power the device never sleeps (`usb_awake=1`), so pushes are instant. On battery it is reachable for `awake` seconds after boot or any button press.

## CLI (`tools/epd.py`)

```sh
export EPD_HOST=reterminal.local          # or the IP
python3 tools/epd.py status
python3 tools/epd.py push picture.png     # fit + threshold (solid black stays solid)
python3 tools/epd.py push photo.jpg --dither --fit cover
python3 tools/epd.py text "Back in 5\nminutes" --mode partial
python3 tools/epd.py config interval=600 url=http://my-server:8080/dashboard.png
python3 tools/epd.py refresh | clear | sleep | reboot
python3 tools/epd.py ota firmware/.pio/build/reterminal/firmware.bin
python3 tools/epd.py screenshot now.png   # what is on the glass right now
```

## HTTP API

| Method | Path | Body / params |
|---|---|---|
| GET | `/` | web UI |
| GET | `/status` | JSON: battery, usb, charging, temp/humidity, rssi, last pull, config |
| POST | `/image` (header `X-Mode: auto\|full\|fast\|partial`) | PNG (any size, centred, thresholded) or raw 48000-byte frame (row-major, MSB first, **1 = black**) |
| POST | `/text` (headers `X-Size: 1..4`, `X-Mode`) | plain text, `\n` for new lines |
| GET/POST | `/config` | JSON: `url`, `interval`, `awake`, `usb_awake`, `full_every`, `host`, `ssid`, `pass`, `ssid2`, `pass2` (fallback network) |
| POST | `/refresh` `/clear` `/info` `/beep` `/sleep` `/reboot` | — |
| GET | `/screen.bin` | the current frame (raw format) |
| POST | `/ota` | firmware .bin |

```sh
curl --data-binary @frame.png http://reterminal.local/image
curl -H 'X-Size: 3' --data-binary 'Hello' http://reterminal.local/text
```

(Parameters go in `X-…` headers because the ESP32 WebServer doesn't parse query strings on raw-body POSTs.)

## Pull mode

On each wake the device does `GET <url>` with headers `If-None-Match`, `X-Battery-mV`, `X-Battery-Pct`, `X-Device-Id`, `X-Wake-Reason`, `X-RSSI`, `X-Firmware`. Reply with:

- `200` + PNG (or raw 48000-byte frame) + `ETag`, or `304` if unchanged
- optional `X-Next-Refresh: <seconds>` to override the sleep interval just for this cycle (e.g. sleep all night)
- optional `X-Stay-Awake: <seconds>` to keep the HTTP API up after this pull, so you can push/OTA a battery-powered device without touching it

`python3 tools/epd.py serve ./screens` is a ready-made server: it serves `screens/screen.png` (or any file in that dir, auto-converted to 1-bit) and a live example clock `dashboard.png` whose wakes are aligned to the minute. `--next N` forces the sleep, `--stay-awake N` keeps the device reachable after its next pull.

Measured: a 304 wake costs ~1–2 s awake; full refresh 5.2 s, fast 2.0 s, partial 1.3 s.

## Fabricate metrics dashboard (`dashboard/`)

`dashboard/fabricate.py` renders Fabricate admin metrics (activity, signups, hours, ships, funnel, review ops, grants/scraps, countries, audience) as a 1-bit 800×480 screen, pulled straight from the Fabricate admin MCP over HTTP. The token lives in `~/.config/fabricate-epd/token` (not in the repo).

```sh
python3 dashboard/fabricate.py render out.png          # fetch + render once
python3 dashboard/fabricate.py render out.png --cache  # re-render last data, offline
systemctl --user status fabricate-epd                  # the server (dashboard/fabricate-epd.service), :8080
python3 tools/epd.py config url=http://<laptop-ip>:8080/fabricate.png
```

The server answers `/fabricate.png` (also `/` and `/screen.png`) and sets `X-Next-Refresh` so the device wakes ~20 s after every full hour; metrics are fetched on that wake. If the fetch fails it draws the last good data marked `STALE` and retries in 10 minutes.

## Build / flash

```sh
cd firmware && pio run -t upload        # USB (CH340, auto-reset works; >230400 baud is unreliable)
python3 tools/epd.py ota firmware/.pio/build/reterminal/firmware.bin   # or over WiFi
pio device monitor                      # serial console: status | pull | info | sleep | reboot | {"ssid":"..","pass":".."}
```

Opening the serial port resets the board (DTR/RTS are wired to EN/BOOT), so wait for the boot log before typing.

Current setup: joins `Hack Club`, falls back to `Hack Club (guest)`. The guest network isolates clients, so push from a laptop only works on the main network; pull works from either.

## Restoring the stock firmware

`backup/stock_full_32MB.bin` is a full dump of the original flash:

```sh
esptool --port /dev/ttyUSB0 -b 230400 write-flash 0 backup/stock_full_32MB.bin
# sha256 e55f8714ee1b9054a124e24a7497c09dc6c35594cbd40dd9dc6c48df0dd6d168, verified against the chip before erasing
```
