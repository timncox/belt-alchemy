// Belt's screen on the phone: the voice drawn across the whole view, and
// over it what a singer wants to see. The note it's pulled to, how far off
// in cents, the harmony voices, the knob a hand is on, the page, HARD and
// the mute as real buttons, and the levels. All from STATE, polled as fast
// as the link allows (about 15 a second over USB, 6 over Bluetooth); the
// graph itself carries every frame the module saw, 60 a second.

import SwiftUI
import AlchemyLink

struct BeltScreen: View {
    @EnvironmentObject var store: ModuleStore
    let st: ModuleState
    let belt: BeltState

    var body: some View {
        ZStack {
            PitchGraph(lines: [PitchLine(trace: store.trace, belt: belt, color: Theme.key)], fieldStyle: true)
                .mask(LinearGradient(stops: [.init(color: .clear, location: 0), .init(color: .black, location: 0.28),
                                             .init(color: .black, location: 0.72), .init(color: .clear, location: 1)],
                                     startPoint: .top, endPoint: .bottom))
                .padding(.vertical, 8)

            VStack(alignment: .leading, spacing: 0) {
                NoteReadout(belt: belt)
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Text("\(BeltTheory.keys[belt.key % 12]) \(BeltTheory.scales[min(belt.scale, BeltTheory.scales.count - 1)].lowercased())")
                        .font(Theme.display(18, weight: .semibold))
                        .foregroundStyle(Theme.mist)
                    PageLine(st: st)
                }
                .padding(.top, 10)
                Spacer(minLength: 20)
                Voices(belt: belt)
                    .padding(.bottom, 16)
                HStack(spacing: 10) {
                    BeltButton(title: "Hard", on: belt.hard, latched: belt.hardLatched, button: 0)
                    BeltButton(title: "Mute", on: belt.muted, latched: belt.muteLatched, button: 1)
                }
                .padding(.bottom, 14)
                Meters(st: st)
            }
            .padding(.horizontal, 20).padding(.top, 6).padding(.bottom, 12)

            if let f = store.touchedField {
                let look = KnobLook.of(f, pageColor: nil)
                VStack(spacing: 4) {
                    Text(f.show(store.value(f))).font(Theme.display(34)).foregroundStyle(look.color)
                    Text(f.name).font(.subheadline.weight(.medium)).foregroundStyle(Theme.mist)
                }
                .padding(.horizontal, 26).padding(.vertical, 16)
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
            }
        }
        .animation(.easeOut(duration: 0.15), value: store.touchedField?.id)
        .background(Theme.ground)
    }
}

/// The page the panel is on, as words.
private struct PageLine: View {
    let st: ModuleState
    var body: some View {
        let page = st.firmwareBusy ? "Switching firmware" : st.settingsOpen ? "Settings" : st.page == 1 ? "Setup page" : "Play page"
        Text(page)
            .font(.footnote)
            .foregroundStyle(st.firmwareBusy ? Theme.alarm : Theme.fog)
    }
}

/// The note the voice is pulled to, very large, and a hairline for the cents.
private struct NoteReadout: View {
    let belt: BeltState

    var body: some View {
        let inTune = abs(belt.cents) <= 10
        let colour = belt.voiced ? (inTune ? Theme.inTune : Theme.key) : Theme.fog.opacity(0.6)
        VStack(alignment: .leading, spacing: 6) {
            Text(belt.target.map(BeltTheory.name) ?? "–")
                .font(Theme.display(76, weight: .black))
                .foregroundStyle(colour)
                .lineLimit(1).minimumScaleFactor(0.5)
                .contentTransition(.identity)
            HStack(spacing: 10) {
                CentsRail(cents: belt.cents, voiced: belt.voiced, colour: colour)
                    .frame(width: 120, height: 10)
                Text(belt.voiced ? "\(belt.cents > 0 ? "+" : "")\(belt.cents) cents" : "not singing")
                    .font(.footnote.monospacedDigit()).foregroundStyle(Theme.fog)
                    .fixedSize()
            }
        }
        .accessibilityElement()
        .accessibilityLabel("Target note")
        .accessibilityValue(belt.voiced ? "\(belt.target.map(BeltTheory.name) ?? ""), \(belt.cents) cents" : "not singing")
    }
}

/// ±50 cents on a hairline, the middle marked.
private struct CentsRail: View {
    let cents: Int
    let voiced: Bool
    let colour: Color

    var body: some View {
        GeometryReader { g in
            let w = g.size.width, mid = w / 2
            let x = mid + CGFloat(max(-50, min(50, cents))) / 50 * (mid - 4)
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.14)).frame(height: 2)
                Rectangle().fill(Theme.inTune.opacity(0.5)).frame(width: w * 0.2, height: 2).offset(x: w * 0.4)
                Rectangle().fill(Theme.mist.opacity(0.6)).frame(width: 1, height: 10).offset(x: mid)
                if voiced {
                    Circle().fill(colour).frame(width: 8, height: 8).shadow(color: colour, radius: 4).offset(x: x - 4)
                        .animation(.easeOut(duration: 0.08), value: cents)
                }
            }
            .frame(maxHeight: .infinity)
        }
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
/// the lanes (the first module's key). `fieldStyle` draws it bare, to sit
/// behind the Belt screen.
struct PitchGraph: View {
    let lines: [PitchLine]
    var fieldStyle = false

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
                let a = isTarget ? 0.5 : root ? 0.18 : inScale ? 0.08 : 0.025
                ctx.stroke(p, with: .color(isTarget ? Theme.inTune.opacity(a) : Theme.mist.opacity(a)), lineWidth: isTarget ? 1.5 : 1)
                if inScale {
                    ctx.draw(Text(BeltTheory.name(n)).font(.system(size: 9, weight: root ? .semibold : .regular).width(.expanded))
                                .foregroundColor(root ? Theme.mist : Theme.fog),
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
                var glow = ctx
                glow.addFilter(.blur(radius: 6))
                glow.stroke(path, with: .color(line.color.opacity(0.55)), style: StrokeStyle(lineWidth: 5, lineCap: .round, lineJoin: .round))
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
        .background(fieldStyle ? Color.clear : Theme.surface, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .accessibilityLabel("Pitch graph")
    }
}

/// The four harmony voices in a row: a dot in the voice's colour, the
/// interval, and the note it sings.
struct Voices: View {
    let belt: BeltState
    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            ForEach(0..<4, id: \.self) { i in cell(i).frame(maxWidth: .infinity, alignment: .leading) }
        }
    }

    private func cell(_ i: Int) -> some View {
        let itv = belt.voices[i]
        let on = itv != 0 && !belt.muted
        let col = Theme.voices[i]
        return VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Circle().fill(on ? col : Color.white.opacity(0.14)).frame(width: 7, height: 7)
                    .shadow(color: on ? col : .clear, radius: 4)
                Text(itv == 0 ? "Off" : BeltTheory.intervals[min(itv, BeltTheory.intervals.count - 1)])
                    .font(Theme.display(17, weight: .bold))
                    .foregroundStyle(itv == 0 ? Theme.fog.opacity(0.6) : Theme.mist)
                    .lineLimit(1).minimumScaleFactor(0.6)
            }
            Text(itv == 0 ? "Voice \(i + 1)" : (belt.harmonyNotes[i].map(BeltTheory.name) ?? "–"))
                .font(.footnote.monospacedDigit())
                .foregroundStyle(on ? col : Theme.fog.opacity(0.7))
                .padding(.leading, 13)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Voice \(i + 1)")
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
        let tint = button == 0 ? Theme.mist : Theme.alarm
        HStack(spacing: 8) {
            Text(title).font(Theme.display(16, weight: .bold))
            if latched { Image(systemName: "lock.fill").font(.caption2.weight(.bold)) }
        }
            .foregroundStyle(on ? Color.black : Theme.mist)
            .frame(maxWidth: .infinity, minHeight: 52)
            .background {
                if on { Capsule().fill(tint).shadow(color: tint.opacity(0.5), radius: 12) }
                else { Capsule().fill(.ultraThinMaterial) }
            }
            .overlay(Capsule().stroke(Color.white.opacity(on ? 0 : 0.10)))
            .contentShape(Capsule())
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
        HStack(spacing: 16) {
            meter("In", st.inLevel)
            meter("Out", st.outLevel)
            Text("CPU \(Int((st.cpu * 100).rounded()))%")
                .font(.caption.monospacedDigit())
                .foregroundStyle(st.cpu > 0.85 ? Theme.alarm : Theme.fog)
        }
    }
    private func meter(_ t: String, _ db: Double) -> some View {
        HStack(spacing: 8) {
            Text(t).font(.caption).foregroundStyle(Theme.fog)
            LevelLine(db: db)
        }
    }
}

/// The tuner on its own, for the combined screen: note, rail and cents.
struct Tuner: View {
    let belt: BeltState
    var body: some View {
        NoteReadout(belt: belt)
    }
}
