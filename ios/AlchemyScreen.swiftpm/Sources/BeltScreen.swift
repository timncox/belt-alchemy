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
        VStack(spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                PageChips(st: st)
                Spacer()
                Text("\(BeltTheory.keys[belt.key % 12]) \(BeltTheory.scales[min(belt.scale, BeltTheory.scales.count - 1)])")
                    .font(Theme.slab(19))
                    .foregroundStyle(Theme.key)
            }
            HStack(alignment: .center, spacing: 14) {
                Tuner(belt: belt)
                    .frame(width: 168, height: 168)
                Voices(belt: belt)
            }
            ZStack(alignment: .top) {
                PitchGraph(lines: [PitchLine(trace: store.trace, belt: belt, color: Theme.key)])
                    .frame(minHeight: 160)
                if let f = store.touchedField {
                    HStack(spacing: 8) {
                        Text(f.name).font(.headline).foregroundStyle(Theme.silk)
                        Text(f.show(store.value(f))).font(Theme.slab(18)).foregroundStyle(KnobLook.of(f, pageColor: nil).color)
                    }
                    .padding(.horizontal, 14).padding(.vertical, 8)
                    .background(Theme.raised, in: Capsule())
                    .overlay(Capsule().stroke(Theme.silk.opacity(0.12)))
                    .padding(.top, 8)
                    .transition(.opacity)
                }
            }
            .animation(.easeOut(duration: 0.15), value: store.touchedField?.id)
            HStack(spacing: 12) {
                BeltButton(title: "HARD", on: belt.hard, latched: belt.hardLatched, button: 0)
                BeltButton(title: "MUTE", on: belt.muted, latched: belt.muteLatched, button: 1)
            }
            Meters(st: st)
        }
        .padding(.horizontal).padding(.bottom, 8)
        .background(Theme.plate)
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
        HStack(spacing: 6) {
            Circle().fill(on ? color : Theme.ledOff).frame(width: 8, height: 8)
                .shadow(color: on ? color.opacity(0.8) : .clear, radius: 4)
            Text(t).font(.subheadline.weight(on ? .semibold : .regular))
                .foregroundStyle(on ? Theme.silk : Theme.silkDim)
        }
    }
}

/// The note the voice is pulled to, inside a ring of LEDs: one lights where
/// the pitch is, the middle one is dead on, ±50 cents at the ends. Green
/// within 10 cents, amber outside, dark when nobody's singing.
struct Tuner: View {
    let belt: BeltState
    private let count = 21

    var body: some View {
        let inTune = abs(belt.cents) <= 10
        let lamp = belt.voiced ? (inTune ? Theme.inTune : Theme.key) : Theme.silkDim
        GeometryReader { g in
            let d = min(g.size.width, g.size.height)
            let led = d * 0.06
            let r = d / 2 - led
            let c = CGPoint(x: g.size.width / 2, y: g.size.height / 2)
            let pos = (Double(max(-50, min(50, belt.cents))) + 50) / 100 * Double(count - 1)
            ZStack {
                ForEach(0..<count, id: \.self) { i in
                    let a = (135 + 270 * Double(i) / Double(count - 1)) * .pi / 180
                    let centre = i == count / 2
                    let near = abs(Double(i) - pos) < 0.5 && belt.voiced
                    let fill: Color = near ? lamp : centre ? Theme.inTune.opacity(0.28) : Theme.ledOff
                    Circle().fill(fill)
                        .frame(width: centre ? led * 1.25 : led, height: centre ? led * 1.25 : led)
                        .shadow(color: near ? lamp.opacity(0.9) : .clear, radius: led)
                        .position(x: c.x + r * cos(a), y: c.y + r * sin(a))
                        .animation(.easeOut(duration: 0.08), value: near)
                }
                VStack(spacing: 2) {
                    Text(belt.target.map(BeltTheory.name) ?? "–")
                        .font(Theme.slab(d * 0.25))
                        .foregroundStyle(lamp)
                        .minimumScaleFactor(0.5).lineLimit(1)
                    Text(belt.voiced ? "\(belt.cents > 0 ? "+" : "")\(belt.cents) cents" : "not singing")
                        .font(.caption.monospacedDigit()).foregroundStyle(Theme.silkDim)
                }
                .frame(width: d * 0.62)
                .position(c)
                .transaction { $0.animation = nil }
            }
        }
        .accessibilityElement()
        .accessibilityLabel("Tuner")
        .accessibilityValue(belt.voiced ? "\(belt.target.map(BeltTheory.name) ?? ""), \(belt.cents) cents" : "not singing")
    }
}

struct PitchLine {
    var trace: [Double?]
    var belt: BeltState
    var color: Color
}

/// The last few seconds of sung pitch over the key's note lanes. Scale
/// notes are brighter, the root brightest; the target lane is lit; the
/// harmony voices sit at the right edge. With two modules, both lines share
/// the lanes (the first module's key).
struct PitchGraph: View {
    let lines: [PitchLine]

    var body: some View {
        Canvas { ctx, size in
            guard let first = lines.first else { return }
            let key = first.belt.key, scale = first.belt.scale
            let centre: Double = {
                for l in lines { if let t = l.belt.target, l.belt.voiced { return Double(t) } }
                for l in lines { if let r = l.trace.suffix(120).compactMap({ $0 }).last { return r.rounded() } }
                return 64
            }()
            let span = 9.0                                   // semitones each side
            let lo = centre - span, hi = centre + span
            let y = { (n: Double) in size.height * CGFloat((hi - n) / (hi - lo)) }
            let plotW = size.width - 34

            for n in Int(lo)...Int(hi) {
                let inScale = BeltTheory.inScale(n, key: key, scale: scale)
                let root = ((n - key) % 12 + 12) % 12 == 0
                let yy = y(Double(n))
                var p = Path(); p.move(to: CGPoint(x: 0, y: yy)); p.addLine(to: CGPoint(x: plotW, y: yy))
                let isTarget = lines.contains { $0.belt.target == n && $0.belt.voiced }
                let a = isTarget ? 0.55 : root ? 0.3 : inScale ? 0.14 : 0.04
                ctx.stroke(p, with: .color(isTarget ? Theme.inTune.opacity(a) : Theme.silk.opacity(a)), lineWidth: isTarget ? 2 : 1)
                if inScale {
                    ctx.draw(Text(BeltTheory.name(n)).font(.system(size: 9, weight: root ? .bold : .regular)).foregroundColor(root ? Theme.silk : Theme.silkDim),
                             at: CGPoint(x: plotW + 17, y: yy))
                }
            }

            let dx = plotW / CGFloat(ModuleStore.traceLength - 1)
            for line in lines {
                let trace = line.trace
                guard trace.count > 1 else { continue }
                // The trace, newest at the right; gaps where nothing was sung.
                let x0 = plotW - CGFloat(trace.count - 1) * dx
                var path = Path(), pen = false
                for (i, v) in trace.enumerated() {
                    guard let n = v, n > lo - 1, n < hi + 1 else { pen = false; continue }
                    let pt = CGPoint(x: x0 + CGFloat(i) * dx, y: y(n))
                    if pen { path.addLine(to: pt) } else { path.move(to: pt); pen = true }
                }
                ctx.stroke(path, with: .color(line.color),
                           style: StrokeStyle(lineWidth: 2.5, lineCap: .round, lineJoin: .round))

                if let last = trace.last ?? nil, line.belt.voiced {
                    let p = CGPoint(x: plotW, y: y(last))
                    ctx.fill(Path(ellipseIn: CGRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10)), with: .color(line.color))
                    if !line.belt.muted {
                        for (vi, h) in line.belt.harmonyNotes.enumerated() {
                            guard let h, Double(h) > lo, Double(h) < hi else { continue }
                            let q = CGPoint(x: plotW, y: y(Double(h)))
                            let vc = lines.count > 1 ? line.color : Theme.voices[vi % 4]
                            ctx.stroke(Path(ellipseIn: CGRect(x: q.x - 5, y: q.y - 5, width: 10, height: 10)),
                                       with: .color(vc), lineWidth: 2)
                        }
                    }
                }
            }
        }
        .background(Theme.raised.opacity(0.6), in: RoundedRectangle(cornerRadius: 4))
        .accessibilityLabel("Pitch graph")
    }
}

struct Voices: View {
    let belt: BeltState
    var body: some View {
        Grid(horizontalSpacing: 10, verticalSpacing: 10) {
            GridRow { cell(0); cell(1) }
            GridRow { cell(2); cell(3) }
        }
    }

    private func cell(_ i: Int) -> some View {
        let itv = belt.voices[i]
        let on = itv != 0 && !belt.muted
        let col = Theme.voices[i]
        return VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 6) {
                Circle().fill(on ? col : Theme.ledOff).frame(width: 8, height: 8)
                    .shadow(color: on ? col.opacity(0.8) : .clear, radius: 4)
                Text("Voice \(i + 1)").font(.caption).foregroundStyle(Theme.silkDim)
            }
            Text(BeltTheory.intervals[min(itv, BeltTheory.intervals.count - 1)])
                .font(Theme.slab(22))
                .foregroundStyle(itv == 0 ? Theme.silkDim : Theme.silk)
                .lineLimit(1).minimumScaleFactor(0.6)
            Text(itv == 0 ? " " : (belt.harmonyNotes[i].map(BeltTheory.name) ?? "–"))
                .font(.subheadline.weight(.semibold).monospacedDigit())
                .foregroundStyle(on ? col : Theme.silkDim)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.leading, 12)
        .overlay(alignment: .leading) {
            Rectangle().fill(on ? col.opacity(0.6) : Theme.ledOff).frame(width: 2)
        }
        .opacity(itv == 0 ? 0.6 : 1)
        .accessibilityElement(children: .combine)
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
        let lamp = button == 0 ? Theme.silk : Theme.alarm
        VStack(spacing: 6) {
            HStack(spacing: 5) {
                ForEach(0..<2, id: \.self) { _ in
                    Circle().fill(on ? lamp : Theme.ledOff).frame(width: 7, height: 7)
                        .shadow(color: on ? lamp.opacity(0.9) : .clear, radius: 5)
                }
            }
            HStack(spacing: 6) {
                Text(title).font(Theme.slab(17)).tracking(0.5)
                if latched { Image(systemName: "lock.fill").font(.caption2) }
            }
            .foregroundStyle(on ? Theme.silk : Theme.silkDim)
        }
            .frame(maxWidth: .infinity, minHeight: 58)
            .background(Theme.raised, in: RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(on ? lamp.opacity(0.5) : Theme.silk.opacity(0.08), lineWidth: 1))
            .contentShape(RoundedRectangle(cornerRadius: 8))
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

struct Meters: View {
    let st: ModuleState
    var body: some View {
        HStack(spacing: 14) {
            meter("In", st.inLevel)
            meter("Out", st.outLevel)
            Text("CPU \(Int((st.cpu * 100).rounded()))%")
                .font(.caption.monospacedDigit())
                .foregroundStyle(st.cpu > 0.85 ? Theme.alarm : Theme.silkDim)
        }
    }
    private func meter(_ t: String, _ db: Double) -> some View {
        HStack(spacing: 6) {
            Text(t).font(.caption).foregroundStyle(Theme.silkDim)
            LEDLadder(db: db).frame(height: 7)
        }
    }
}
