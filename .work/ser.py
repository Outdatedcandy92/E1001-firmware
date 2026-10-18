#!/home/rudy/.local/share/uv/tools/esptool/bin/python
"""ser.py SECONDS [line ...]  -- open serial (this resets the board), wait for boot to finish,
send lines, then print output for SECONDS."""
import sys, time, serial
args = sys.argv[1:]
secs = float(args[0]); lines = args[1:]
for _ in range(60):
    try:
        s = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.1); break
    except serial.SerialException:
        time.sleep(0.5)
buf = ""
t0 = time.time()
# wait for setup() to finish: it prints one of these, then possibly a refresh line
while time.time() - t0 < 25:
    d = s.read(4096).decode(errors="replace")
    buf += d; sys.stdout.write(d); sys.stdout.flush()
    if ("refresh " in buf or "pull:" in buf) and time.time() - t0 > 2:
        break
time.sleep(0.5)
for l in lines:
    s.write((l + "\n").encode()); time.sleep(0.5)
end = time.time() + secs
while time.time() < end:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
s.close()
