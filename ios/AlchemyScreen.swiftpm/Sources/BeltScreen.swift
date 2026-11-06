// Belt's screen on the phone: not a copy of the LED rings but what a singer
// wants to see. The note being sung against the note it's pulled to, how
// far off in cents, a scrolling pitch graph over the key's lanes with the
// harmony voices beside it, the knob a hand is on, the page, HARD and the
// mute as real buttons, and the levels. All from STATE, polled as fast as
// the link allows (about 15 a second over USB, 6 over Bluetooth); the
// graph itself carries every frame the module saw, 60 a second.

import SwiftUI
import AlchemyLink

struct BeltScreen: View {
    @EnvironmentObject var store: ModuleStore
    let st: ModuleState
    let belt: BeltState

    var body: some View {
        VStack(spacing: 10) {
            HStack {
                PageChips(st: st)
                Spacer()
                Text("\(BeltTheory.keys[belt.key % 12]) \(BeltTheory.scales[min(belt.scale, BeltTheory.scales.count - 1)])")
                    .font(.subheadline.weight(.semibold))
                    .padding(.horizontal, 10).padding(.vertical, 5)
                    .background(Color.white.opacity(0.07), in: Capsule())
            }
            Tuner(belt: belt)
            ZStack(alignment: .top) {
                PitchGraph(trace: store.trace, belt: belt)
                    .frame(minHeight: 160)
                if let f = store.touchedField {
                    HStack(spacing: 8) {
                        Text(f.name).font(.headline)
                        Text(f.show(store.value(f))).font(.headline.monospacedDigit()).foregroundStyle(.orange)
                    }
                    .padding(.horizontal, 14).padding(.vertical, 8)
                    .background(.ultraThinMaterial, in: Capsule())
                    .padding(.top, 8)
                    .transition(.opacity)
                }
            }
            .animation(.easeOut(duration: 0.15), value: store.touchedField?.id)
            Voices(belt: belt)
            HStack(spacing: 12) {
                BeltButton(title: "HARD", on: belt.hard, latched: belt.hardLatched, button: 0)
                BeltButton(title: "MUTE", on: belt.muted, latched: belt.muteLatched, button: 1)
            }
            Meters(st: st)
        }
        .padding(.horizontal).padding(.bottom, 8)
    }
}

private struct PageChips: View {
    let st: ModuleState
    var body: some View {
        HStack(spacing: 6) {
            chip("Play", on: !st.settingsOpen && st.page == 0, color: Color(hex: "#ff9020")!)
            chip("Setup", on: !st.settingsOpen && st.page == 1, color: Color(hex: "#c060ff")!)
            if st.settingsOpen { chip("Settings", on: true, color: .gray) }
            if st.firmwareBusy { chip("Switching firmware", on: true, color: .red) }
        }
    }
    private func chip(_ t: String, on: Bool, color: Color) -> some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 7, height: 7)
            Text(t).font(.caption.weight(.semibold))
        }
        .padding(.horizontal, 9).padding(.vertical, 5)
        .background(on ? color.opacity(0.28) : Color.white.opacity(0.05), in: Capsule())
        .opacity(on ? 1 : 0.55)
    }
}

/// The sung note, its target, and a needle for the cents between them.
private struct Tuner: View {
    let belt: BeltState

    var body: some View {
        let inTune = abs(belt.cents) <= 10
        HStack(alignment: .center, spacing: 16) {
            VStack(spacing: 0) {
                Text(belt.target.map(BeltTheory.name) ?? "–")
                    .font(.system(size: 52, weight: .bold, design: .rounded).monospacedDigit())
                    .foregroundStyle(belt.voiced ? (inTune ? Color.green : Color.orange) : Color.secondary)
                Text(belt.voiced ? "\(belt.cents > 0 ? "+" : "")\(belt.cents) cents" : "not singing")
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
            .frame(minWidth: 110)
            GeometryReader { g in
                let w = g.size.width, mid = w / 2
                let x = mid + CGFloat(max(-50, min(50, belt.cents))) / 50 * (mid - 6)
                ZStack(alignment: .leading) {
                    Capsule().fill(Color.white.opacity(0.07)).frame(height: 10)
                    Rectangle().fill(Color.green.opacity(0.35)).frame(width: w * 0.2, height: 10).offset(x: w * 0.4)
                    Rectangle().fill(Color.white.opacity(0.5)).frame(width: 2, height: 22).offset(x: mid - 1)
                    if belt.voiced {
                        Circle().fill(inTune ? Color.green : .orange).frame(width: 14, height: 14).offset(x: x - 7)
                            .animation(.easeOut(duration: 0.08), value: belt.cents)
                    }
                }
                .frame(maxHeight: .infinity)
            }
            .frame(height: 44)
        }
    }
}

/// The last few seconds of sung pitch over the key's note lanes. Scale
/// notes are brighter, the root brightest; the target lane is lit; the
/// harmony voices sit at the right edge.
private struct PitchGraph: View {
    let trace: [Double?]
    let belt: BeltState

    var body: some View {
        Canvas { ctx, size in
            let recent = trace.suffix(120).compactMap { $0 }
            let centre = (belt.target.map(Double.init) ?? recent.last ?? 64).rounded()
            let span = 9.0                                   // semitones each side
            let lo = centre - span, hi = centre + span
            let y = { (n: Double) in size.height * CGFloat((hi - n) / (hi - lo)) }
            let plotW = size.width - 34

            for n in Int(lo)...Int(hi) {
                let inScale = BeltTheory.inScale(n, key: belt.key, scale: belt.scale)
                let root = ((n - belt.key) % 12 + 12) % 12 == 0
                let yy = y(Double(n))
                var p = Path(); p.move(to: CGPoint(x: 0, y: yy)); p.addLine(to: CGPoint(x: plotW, y: yy))
                let isTarget = belt.target == n && belt.voiced
                let a = isTarget ? 0.55 : root ? 0.3 : inScale ? 0.14 : 0.04
                ctx.stroke(p, with: .color(isTarget ? .green.opacity(a) : .white.opacity(a)), lineWidth: isTarget ? 2 : 1)
                if inScale {
                    ctx.draw(Text(BeltTheory.name(n)).font(.system(size: 9)).foregroundColor(.secondary),
                             at: CGPoint(x: plotW + 17, y: yy))
                }
            }

            // The trace, newest at the right; gaps where nothing was sung.
            guard trace.count > 1 else { return }
            let dx = plotW / CGFloat(ModuleStore.traceLength - 1)
            let x0 = plotW - CGFloat(trace.count - 1) * dx
            var path = Path(), pen = false
            for (i, v) in trace.enumerated() {
                guard let n = v, n > lo - 1, n < hi + 1 else { pen = false; continue }
                let pt = CGPoint(x: x0 + CGFloat(i) * dx, y: y(n))
                if pen { path.addLine(to: pt) } else { path.move(to: pt); pen = true }
            }
            ctx.stroke(path, with: .color(.orange), style: StrokeStyle(lineWidth: 2.5, lineCap: .round, lineJoin: .round))

            if let last = trace.last ?? nil, belt.voiced {
                let p = CGPoint(x: plotW, y: y(last))
                ctx.fill(Path(ellipseIn: CGRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10)), with: .color(.orange))
                if !belt.muted {
                    for h in belt.harmonyNotes.compactMap({ $0 }) where Double(h) > lo && Double(h) < hi {
                        let q = CGPoint(x: plotW, y: y(Double(h)))
                        ctx.fill(Path(ellipseIn: CGRect(x: q.x - 4, y: q.y - 4, width: 8, height: 8)),
                                 with: .color(Color(hex: "#c060ff")!))
                    }
                }
            }
        }
        .background(Color.white.opacity(0.03), in: RoundedRectangle(cornerRadius: 10))
        .accessibilityLabel("Pitch graph")
    }
}

private struct Voices: View {
    let belt: BeltState
    var body: some View {
        HStack(spacing: 8) {
            ForEach(0..<4, id: \.self) { i in
                let itv = belt.voices[i]
                let note = belt.harmonyNotes[i]
                VStack(spacing: 2) {
                    Text("Voice \(i + 1)").font(.caption2).foregroundStyle(.secondary)
                    Text(BeltTheory.intervals[min(itv, BeltTheory.intervals.count - 1)])
                        .font(.subheadline.weight(.semibold))
                    Text(itv == 0 ? " " : (note.map(BeltTheory.name) ?? "–"))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(Color(hex: "#c060ff")!)
                }
                .frame(maxWidth: .infinity).padding(.vertical, 6)
                .background(Color.white.opacity(itv == 0 || belt.muted ? 0.03 : 0.08), in: RoundedRectangle(cornerRadius: 8))
                .opacity(itv == 0 || belt.muted ? 0.5 : 1)
            }
        }
    }
}

/// B1 (HARD) and B2 (mute) as on the panel: a tap latches, a hold works
/// while held.
private struct BeltButton: View {
    @EnvironmentObject var store: ModuleStore
    let title: String
    let on: Bool
    let latched: Bool
    let button: Int
    @State private var downAt: Date?
    @State private var held = false

    var body: some View {
        Text(title)
            .font(.headline)
            .frame(maxWidth: .infinity, minHeight: 50)
            .background(on ? Color.orange.opacity(0.85) : Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
            .overlay(alignment: .topTrailing) {
                if latched { Image(systemName: "lock.fill").font(.caption2).padding(6) }
            }
            .foregroundStyle(on ? Color.black : Color.primary)
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        guard downAt == nil else { return }
                        let t = Date()
                        downAt = t
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
                            if downAt == t { held = true; store.press(button: button, .down) }
                        }
                    }
                    .onEnded { _ in
                        if held { store.press(button: button, .up) } else { store.press(button: button, .tap) }
                        downAt = nil; held = false
                    }
            )
            .accessibilityLabel(title)
            .accessibilityAddTraits(.isButton)
            .accessibilityAction { store.press(button: button, .tap) }
    }
}

private struct Meters: View {
    let st: ModuleState
    var body: some View {
        HStack(spacing: 14) {
            meter("In", st.inLevel)
            meter("Out", st.outLevel)
            Text("CPU \(Int((st.cpu * 100).rounded()))%")
                .font(.caption.monospacedDigit())
                .foregroundStyle(st.cpu > 0.85 ? Color.red : Color.secondary)
        }
    }
    private func meter(_ t: String, _ db: Double) -> some View {
        HStack(spacing: 6) {
            Text(t).font(.caption).foregroundStyle(.secondary)
            GeometryReader { g in
                let f = CGFloat(max(0, min(1, (db + 60) / 60)))
                ZStack(alignment: .leading) {
                    Capsule().fill(Color.white.opacity(0.07))
                    Capsule().fill(db > -3 ? Color.red : db > -12 ? .yellow : .green).frame(width: g.size.width * f)
                }
            }
            .frame(height: 6)
        }
    }
}
