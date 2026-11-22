import sys, statistics as st
# analyze.py MIDFILE CVLOG BASEFRAME BPM [jobs]   jobs: default J3 gateA J4 gateB J5 clk16 J6 reset J7 trig1 J8 trig2
mid, cvp, base, bpm = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
jobs = (sys.argv[5] if len(sys.argv) > 5 else "gateA,gateB,clk16,reset,trig1,trig2").split(",")
ev = []
for l in open(mid):
    t, a, b, c = l.split(); ev.append((float(t), int(a, 16), int(b, 16), int(c, 16)))
cv = []
for l in open(cvp):
    f = l.split(); cv.append(((int(f[0]) - base) / 48.0, [float(x) for x in f[1:]]))
def at(t):            # the block in effect at time t (ms)
    lo, hi = 0, len(cv) - 1
    while lo < hi:
        m = (lo + hi + 1) // 2
        if cv[m][0] <= t: lo = m
        else: hi = m - 1
    return cv[lo][1]
def rises(j, thr=2.5):
    r = []; prev = cv[0][1][j]
    for t, v in cv:
        if prev < thr <= v[j]: r.append(t)
        prev = v[j]
    return r
def first_after(lst, t, within):
    for x in lst:
        if x >= t - 0.01 and x <= t + within: return x
    return None
fa = next(t for t, s, a, b in ev if s == 0xFA); fc = next((t for t, s, a, b in ev if s == 0xFC), 1e9)
step = 60000.0 / bpm / 4
out = {"fails": 0}
def report(name, ok, msg):
    print(("  ok   " if ok else "FAIL ") + name + ": " + msg)
    if not ok: out["fails"] += 1
def lat_str(l): return "none" if not l else f"{st.mean(l):.2f} ms avg, {min(l):.2f}-{max(l):.2f}"
# --- voices: pitch and gate
for vi, (ch, gj, pj) in enumerate(((4, jobs.index("gateA") if "gateA" in jobs else None, 6), (5, jobs.index("gateB") if "gateB" in jobs else None, 7))):
    held = []; pitch = 0.0; bad_p = 0; chk_p = 0; bad_g = 0; chk_g = 0; lats = []; ons = 0; wrongs = []
    evs = [(t, s, a, b) for t, s, a, b in ev if (s & 0xF0) in (0x80, 0x90) and (s & 15) == ch]
    model = []   # (t, pitch, gate)
    for t, s, a, b in evs:
        if s & 0xF0 == 0x90 and b > 0:
            if a in held: held.remove(a)
            held.append(a); pitch = (a - 60) / 12.0; ons += 1
        else:
            if a in held: held.remove(a)
            if held: pitch = (held[-1] - 60) / 12.0
        model.append((t, pitch, bool(held)))
    gr = rises(gj) if gj is not None else []
    for i, (t, p, g) in enumerate(model):
        nxt = model[i + 1][0] if i + 1 < len(model) else t + 50
        # latency: the first block at or after t where pitch matches (and, for a note on, the gate is up)
        if i == 0 or model[i - 1][1] != p:
            k = next((tc for tc, v in cv if tc >= t - 0.01 and abs(v[pj] - p) < 0.002), None)
            if k is not None and k - t < 20: lats.append(k - t)
        # settled: 4 ms after the event to the next event
        tt = t + 4.0
        while tt < nxt - 0.5:
            v = at(tt); chk_p += 1
            if abs(v[pj] - p) > 0.002:
                bad_p += 1
                if len(wrongs) < 3: wrongs.append(f"t={tt:.1f} want {p:+.3f} got {v[pj]:+.3f}")
            if gj is not None:
                chk_g += 1
                if (v[gj] >= 2.5) != g: bad_g += 1
            tt += 1.0
    vname = "A (BASS ch5) J9" if vi == 0 else "B (LEAD ch6) J10"
    report(f"pitch {vname}", bad_p == 0 and ons > 0, f"{ons} notes, {chk_p} checks, {bad_p} wrong; reaches pitch {lat_str(lats)} " + "; ".join(wrongs))
    if gj is not None:
        report(f"gate {vname[:1]} J{gj+3}", bad_g == 0, f"{len(gr)} rises, {chk_g} checks, {bad_g} wrong")
# --- clock
ticks = [t for t, s, a, b in ev if s == 0xF8 and fa <= t < fc]
if "clk16" in jobs:
    j = jobs.index("clk16"); r = [x for x in rises(j)]
    want = ticks[::6]
    lat = [first_after(r, t, 5) - t for t in want if first_after(r, t, 5) is not None]
    report(f"16th clock J{j+3}", len(r) == len(want) and len(lat) == len(want), f"{len(r)} pulses for {len(ticks)} ticks (want {len(want)}); after its tick {lat_str(lat)}")
    grid = [x - fa - round((x - fa) / step) * step for x in r]
    iv = [b - a for a, b in zip(r, r[1:])]
    report(f"16th clock J{j+3} on the beat", abs(st.mean(grid)) < 3 if grid else False,
           f"vs the song's true 16ths: {st.mean(grid):+.2f} ms avg, {min(grid):+.2f} to {max(grid):+.2f}; intervals {min(iv):.2f}-{max(iv):.2f} ms (ideal {step:.2f})")
if "reset" in jobs:
    j = jobs.index("reset"); r = rises(j)
    report(f"reset J{j+3}", len(r) == 1 and r[0] - fa < 3, f"{len(r)} pulse(s), {r[0]-fa:.2f} ms after Start" if r else "no pulse")
for k in range(8):
    nm = f"trig{k+1}"
    if nm not in jobs: continue
    j = jobs.index(nm); r = rises(j)
    ons = [t for t, s, a, b in ev if s == 0x90 + k and b > 0]
    lat = [first_after(r, t, 6) - t for t in ons if first_after(r, t, 6) is not None]   # 6: a hit whose last note ends as it starts waits 3 ms low first
    grid = [x - fa - round((x - fa) / step) * step for x in r]
    report(f"trig {k+1} J{j+3}", len(r) == len(ons) and len(lat) == len(ons),
           f"{len(r)} trigs for {len(ons)} notes; after its note {lat_str(lat)}; vs true 16ths {st.mean(grid):+.2f} ms avg, {min(grid):+.2f} to {max(grid):+.2f}" if r else "none")
if "section" in jobs:
    j = jobs.index("section")
    for t, s, a, b in ev:
        if s == 0xBF and a == 85:
            v = at(t + 3)[j]
            report(f"section J{j+3} after CC85={b} at {t:.0f} ms", abs(v - b) < 0.02, f"{v:.3f} V (want {b} V)")
if "run" in jobs:
    j = jobs.index("run")
    report(f"run J{j+3}", at(fa + 3)[j] > 2.5 and at(fa - 3)[j] < 2.5 and at(fc + 3)[j] < 2.5 and at(fc - 3)[j] > 2.5, "high from Start to Stop")
# all quiet after stop
v = at(fc + 50)
report("after Stop", all(v[jj] < 2.5 for jj in range(6) if jobs[jj] in ("gateA","gateB","trig1","trig2","clk16")), "gates and trigs low 50 ms after Stop")
print("FAILS", out["fails"])
