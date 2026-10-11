#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy"]
# ///
"""Record a sap_mic RTP stream straight off the network and report on it.

    tools/capture.py 239.69.0.54 10 out.wav [--channels 3] [--names mic1,mic2,ref]

Bypasses PipeWire entirely, so what it measures is what the device sent:
packet rate, loss, arrival jitter, then per channel RMS / peak / DC, mains hum
(60/120 Hz above their neighbours), low-frequency and speech-band energy.
"""
import argparse, socket, struct, time, wave
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("group"); ap.add_argument("seconds", type=float); ap.add_argument("out")
ap.add_argument("--port", type=int, default=46000)
ap.add_argument("--channels", type=int, default=3)
ap.add_argument("--names", default="mic1,mic2,ref")
ap.add_argument("--rate", type=int, default=16000)
a = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((a.group, a.port))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack("4s4s", socket.inet_aton(a.group), socket.inet_aton("0.0.0.0")))
s.settimeout(2)
chunks, arrivals, seqs = [], [], []
t0 = time.monotonic()
while time.monotonic() - t0 < a.seconds:
    try: d = s.recv(4000)
    except socket.timeout: print("no packets for 2 s"); continue
    arrivals.append(time.monotonic()); seqs.append(struct.unpack("!H", d[2:4])[0])
    chunks.append(np.frombuffer(d[12:], dtype=">i2"))
if not chunks: raise SystemExit("nothing received")
x = np.concatenate(chunks).astype(np.int16).reshape(-1, a.channels)
with wave.open(a.out, "wb") as w:
    w.setnchannels(a.channels); w.setsampwidth(2); w.setframerate(a.rate); w.writeframes(x.tobytes())

lost = sum(((b - p) & 0xFFFF) - 1 for p, b in zip(seqs, seqs[1:]))
gaps = np.diff(arrivals) * 1000
spp = len(chunks[0]) // a.channels
print(f"{len(seqs)} packets ({len(seqs)/a.seconds:.1f}/s, {spp} frames = {1000*spp/a.rate:.0f} ms each), lost {lost}, "
      f"arrival gap median {np.median(gaps):.1f} / p95 {np.percentile(gaps,95):.1f} / max {gaps.max():.1f} ms")
print(f"saved {a.out}: {len(x)/a.rate:.1f} s x {a.channels} ch")

f = np.fft.rfftfreq(len(x), 1 / a.rate)
db = lambda v: 10 * np.log10(v + 1e-30)
print(f"{'ch':5s} {'rms':>6s} {'peak':>6s} {'dc':>6s} | {'60Hz +nb':>9s} {'120Hz +nb':>9s} | {'<200Hz':>7s} {'speech':>7s}  (dBFS / dB)")
for c, name in enumerate(a.names.split(",")[:a.channels]):
    v = x[:, c].astype(float) / 32768
    P = np.abs(np.fft.rfft((v - v.mean()) * np.hanning(len(v)))) ** 2 / len(v)
    def tone(fr):
        pk = P[(f > fr - 1) & (f < fr + 1)].max()
        nb = np.median(P[((f > fr - 20) & (f < fr - 4)) | ((f > fr + 4) & (f < fr + 20))])
        return db(pk / nb)
    band = lambda lo, hi: db(P[(f >= lo) & (f < hi)].sum())
    print(f"{name:5s} {db(np.mean((v-v.mean())**2)):6.1f} {db(np.max(np.abs(v))**2):6.1f} {db(v.mean()**2):6.1f} | "
          f"{tone(60):9.1f} {tone(120):9.1f} | {band(20,200):7.1f} {band(300,3400):7.1f}")
