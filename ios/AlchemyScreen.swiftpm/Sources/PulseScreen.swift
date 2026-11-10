// Pulse's own screen: the clock and its eight outputs as a touch display.
// The tempo in large type (drag it), start/stop, tap tempo, half and
// double time; then a lane per output drawing the voltage it plays and its
// euclidean pattern. Tap a lane for every setting of that output.
//
// Pulse has no STATE of its own, so what is live comes from two places:
// the knob values (read back every 400 ms, like every screen) and the
// panel's mirrored LEDs: B2 flashes on the beat (cyan while following J1)
// and B3 is green while running, red while stopped (pulse_alchemy.cpp).

import SwiftUI
import AlchemyLink

struct PulseScreen: View {
    @EnvironmentObject var store: ModuleStore
    @Binding var showPanel: Bool

    @State private var editing: Int?
    @State private var sheetOpen = false
    @State private var muted: [Int: Double] = [:]      // output -> the level it had
    @State private var clip: Pulse.Out?
    @State private var tempoStart: Double?

    var body: some View {
        ZStack {
            Theme.ground.ignoresSafeArea()
            VStack(spacing: 0) {
                transport
                    .padding(.horizontal, 20).padding(.top, 6).padding(.bottom, 14)
                ScrollView {
                    VStack(spacing: 2) {
                        ForEach(0..<Pulse.outs, id: \.self) { i in
                            PulseLane(index: i, out: out(i), color: color(i), ratio: ratioLabel(i),
                                      cycleSeconds: cycleSeconds(i), isMuted: muted[i] != nil,
                                      isSelected: selected == i)
                                .contentShape(Rectangle())
                                .onTapGesture { open(i) }
                                .onLongPressGesture(minimumDuration: 0.35) { toggleMute(i) }
                                .accessibilityIdentifier("pulse-lane-\(i + 1)")
                                .accessibilityLabel("Output \(i + 1), \(ratioLabel(i)), \(waveName(i))")
                                .accessibilityHint("Tap to edit, hold to mute")
                        }
                    }
                    .padding(.horizontal, 12)
                    Text("Tap an output to edit it. Hold to mute.")
                        .font(.footnote).foregroundStyle(Theme.fog)
                        .padding(.top, 10).padding(.bottom, 120)   // clear of the tab bar
                }
            }
        }
        .sheet(isPresented: $sheetOpen, onDismiss: { editing = nil }) {
            if let i = editing {
                PulseOutEditor(index: Binding(get: { editing ?? i }, set: { editing = $0; select($0) }),
                               muted: $muted, clip: $clip, colorOf: color)
                    .environmentObject(store)
                    .presentationDetents([.fraction(0.72), .large])
                    .presentationDragIndicator(.visible)
                    .preferredColorScheme(.dark)
            }
        }
    }

    // MARK: the clock

    private var transport: some View {
        HStack(alignment: .top, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text("\(Int(bpm.rounded()))")
                    .font(Theme.display(76)).monospacedDigit()
                    .foregroundStyle(running ? Theme.mist : Theme.fog)
                    .minimumScaleFactor(0.6).lineLimit(1)
                    .contentShape(Rectangle())
                    .gesture(tempoDrag)
                    .accessibilityIdentifier("pulse-tempo")
                    .accessibilityLabel("Tempo")
                    .accessibilityValue("\(Int(bpm.rounded())) BPM")
                    .accessibilityAdjustableAction { dir in setBpm(bpm + (dir == .increment ? 1 : -1)) }
                HStack(spacing: 8) {
                    Circle().fill(beatColor.opacity(beatOn ? 1 : 0.22)).frame(width: 10, height: 10)
                        .shadow(color: beatOn ? beatColor : .clear, radius: 6)
                    Text(following ? "FOLLOWING J1" : "BPM  ·  DRAG TO SET")
                        .font(.caption.weight(.semibold)).tracking(1.2).foregroundStyle(Theme.fog)
                }
            }
            Spacer(minLength: 0)
            VStack(alignment: .trailing, spacing: 10) {
                HStack(spacing: 10) {
                    Button { showPanel = true } label: {
                        Text("PANEL").font(.caption.weight(.semibold)).tracking(1.2).foregroundStyle(Theme.fog)
                            .padding(.vertical, 8).padding(.horizontal, 4)
                    }
                    .buttonStyle(.plain).accessibilityIdentifier("pulse-panel")
                    Button { store.press(button: 2, .tap) } label: {
                        ZStack {
                            Circle().fill(running ? Theme.inTune.opacity(0.18) : Theme.alarm.opacity(0.18))
                            Circle().strokeBorder(running ? Theme.inTune : Theme.alarm, lineWidth: 2)
                            Image(systemName: running ? "stop.fill" : "play.fill")
                                .font(.system(size: 22, weight: .bold))
                                .foregroundStyle(running ? Theme.inTune : Theme.alarm)
                        }
                        .frame(width: 60, height: 60)
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("pulse-run")
                    .accessibilityLabel(running ? "Stop" : "Start")
                }
                HStack(spacing: 8) {
                    PulseChip(title: "÷2") { setBpm(bpm / 2) }.accessibilityIdentifier("pulse-half")
                    PulseChip(title: "×2") { setBpm(bpm * 2) }.accessibilityIdentifier("pulse-double")
                    PulseChip(title: "TAP", strong: true) { store.press(button: 1, .tap) }
                        .accessibilityIdentifier("pulse-tap")
                }
            }
        }
    }

    private var tempoDrag: some Gesture {
        DragGesture(minimumDistance: 3)
            .onChanged { g in
                if tempoStart == nil { tempoStart = bpm }
                setBpm(((tempoStart ?? bpm) - Double(g.translation.height) / 3).rounded())
            }
            .onEnded { _ in tempoStart = nil }
    }

    private var bpm: Double { Pulse.bpm(fromKnob: store.values[Pulse.tempoId] ?? Pulse.knob(fromBpm: 120)) }

    private func setBpm(_ b: Double) {
        guard let f = store.desc?.field(Pulse.tempoId) else { return }
        store.set(f, Pulse.knob(fromBpm: b))
    }

    // MARK: what the LEDs say

    private func led(_ i: Int) -> (r: Double, g: Double, b: Double) {
        let s = store.screen, k = i * 3
        guard s.count >= k + 3 else { return (0, 0, 0) }
        return (Double(s[k]), Double(s[k + 1]), Double(s[k + 2]))
    }
    private func pair(_ button: Int) -> (r: Double, g: Double, b: Double) {
        let ids = PanelLayout.buttonLEDs(button)
        let a = led(ids.bottom), b = led(ids.top)
        return (a.r + b.r, a.g + b.g, a.b + b.b)
    }
    /// B3: green while running. With no LED frame yet, assume running.
    private var running: Bool {
        let p = pair(2)
        return p.r + p.g + p.b == 0 ? true : p.g >= p.r
    }
    /// B2: bright on the beat, a tenth as bright between.
    private var beatOn: Bool {
        let p = pair(1), top = max(p.r, p.g, p.b)
        let ring = pair(2), ref = max(ring.r, ring.g, ring.b)     // B3 is steady: the brightness to compare with
        return top > max(12, ref * 0.6)
    }
    /// B2 is cyan while an external clock on J1 is driving.
    private var following: Bool { let p = pair(1); return p.b > p.r && p.b > 0 }
    private var beatColor: Color { following ? Theme.led(0x20C0FF) : Theme.key }

    // MARK: the outputs

    private func out(_ i: Int) -> Pulse.Out { Pulse.Out(i, values: store.values) }
    private var selected: Int { Int(store.values[Pulse.selectedId] ?? 0) }

    private func color(_ i: Int) -> Color {
        let hex = store.pages.first { $0.name == "Out \(i + 1)" }?.color
        return Color(hex: hex) ?? Theme.key
    }
    private func ratioLabel(_ i: Int) -> String {
        guard let f = store.desc?.field(Pulse.id(i, .ratio)) else { return "" }
        return f.show(store.value(f))
    }
    private func waveName(_ i: Int) -> String {
        guard let f = store.desc?.field(Pulse.id(i, .wave)) else { return "" }
        return f.show(store.value(f))
    }
    private func cycleSeconds(_ i: Int) -> Double {
        let rate = Pulse.cyclesPerBeat(ratioLabel(i)) ?? 1
        return 60 / max(1, bpm) / rate
    }

    private func select(_ i: Int) {
        guard let f = store.desc?.field(Pulse.selectedId) else { return }
        store.set(f, Double(i))
    }
    private func open(_ i: Int) {
        select(i)
        editing = i
        sheetOpen = true
    }
    private func toggleMute(_ i: Int) {
        guard let f = store.desc?.field(Pulse.id(i, .level)) else { return }
        if let was = muted[i] {
            store.set(f, was); muted[i] = nil
        } else {
            muted[i] = store.value(f); store.set(f, 0)
        }
        UIImpactFeedbackGenerator(style: .medium).impactOccurred()
    }
}

/// One output on the main screen: its number, ratio and wave, the voltage
/// it plays through a few cycles, and its pattern.
private struct PulseLane: View {
    var index: Int
    var out: Pulse.Out
    var color: Color
    var ratio: String
    var cycleSeconds: Double
    var isMuted: Bool
    var isSelected: Bool

    var body: some View {
        HStack(spacing: 12) {
            Text("\(index + 1)")
                .font(Theme.display(26)).foregroundStyle(color.opacity(isMuted ? 0.35 : 1))
                .frame(width: 28, alignment: .leading)
            VStack(alignment: .leading, spacing: 1) {
                Text(isMuted ? "MUTED" : ratio)
                    .font(Theme.display(isMuted ? 13 : 19, weight: .bold))
                    .foregroundStyle(isMuted ? Theme.fog : Theme.mist).lineLimit(1).minimumScaleFactor(0.7)
                Text(out.steps > 0 ? "\(out.fills) in \(out.steps)" : " ")
                    .font(.caption2.weight(.medium)).foregroundStyle(Theme.fog)
            }
            .frame(width: 62, alignment: .leading)
            PulseWave(out: out, color: color.opacity(isMuted ? 0.3 : 1), cycles: out.steps > 0 ? min(out.steps, 8) : 2,
                      cycleSeconds: cycleSeconds)
                .frame(height: 46)
            if out.steps > 0 {
                PulseRing(hits: out.hits, color: color.opacity(isMuted ? 0.3 : 1), dot: 2.2)
                    .frame(width: 40, height: 40)
            }
        }
        .padding(.vertical, 9).padding(.horizontal, 8)
        .background(isSelected ? color.opacity(0.09) : Color.clear, in: RoundedRectangle(cornerRadius: 10))
    }
}

/// The output's voltage over a few cycles, on the jack's −5…+5 V scale,
/// with a hairline at 0 V.
struct PulseWave: View {
    var out: Pulse.Out
    var color: Color
    var cycles: Int
    var cycleSeconds: Double
    var width: CGFloat = 2

    var body: some View {
        Canvas { ctx, size in
            let n = max(32, Int(size.width))
            let volts = out.curve(points: n, cycles: cycles, cycleSeconds: cycleSeconds)
            guard volts.count > 1 else { return }
            let y = { (v: Double) in size.height * (1 - (v + 5) / 10) * 0.92 + size.height * 0.04 }
            var zero = Path()
            zero.move(to: CGPoint(x: 0, y: y(0))); zero.addLine(to: CGPoint(x: size.width, y: y(0)))
            ctx.stroke(zero, with: .color(Theme.line), lineWidth: 1)
            var p = Path()
            for (i, v) in volts.enumerated() {
                let pt = CGPoint(x: size.width * Double(i) / Double(volts.count - 1), y: y(v))
                if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
            }
            var glow = ctx
            glow.addFilter(.blur(radius: 4))
            glow.stroke(p, with: .color(color.opacity(0.6)), style: StrokeStyle(lineWidth: width + 2, lineJoin: .round))
            ctx.stroke(p, with: .color(color), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
        }
        .accessibilityHidden(true)
    }
}

/// An euclidean pattern as a ring of steps, the first at 12 o'clock.
struct PulseRing: View {
    var hits: [Bool]
    var color: Color
    var dot: CGFloat = 4

    var body: some View {
        Canvas { ctx, size in
            let n = hits.count
            guard n > 0 else { return }
            let r = min(size.width, size.height) / 2 - dot - 1
            let c = CGPoint(x: size.width / 2, y: size.height / 2)
            for (i, hit) in hits.enumerated() {
                let a = Double(i) / Double(n) * 2 * Double.pi
                let p = CGPoint(x: c.x + r * sin(a), y: c.y - r * cos(a))
                let d = hit ? dot : dot * 0.55
                let rect = CGRect(x: p.x - d, y: p.y - d, width: 2 * d, height: 2 * d)
                if hit {
                    var glow = ctx
                    glow.addFilter(.blur(radius: dot))
                    glow.fill(Path(ellipseIn: rect.insetBy(dx: -dot * 0.5, dy: -dot * 0.5)), with: .color(color.opacity(0.5)))
                    ctx.fill(Path(ellipseIn: rect), with: .color(color))
                } else {
                    ctx.fill(Path(ellipseIn: rect), with: .color(Theme.ledOff))
                }
            }
        }
        .accessibilityHidden(true)
    }
}

struct PulseChip: View {
    var title: String
    var strong = false
    var tint: Color = Theme.mist
    var action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.subheadline.weight(.bold)).tracking(0.8)
                .lineLimit(1).fixedSize()
                .foregroundStyle(strong ? Color.black : tint)
                .padding(.horizontal, 14).frame(minWidth: 48, minHeight: 44)
                .background(strong ? AnyShapeStyle(tint) : AnyShapeStyle(Theme.surface), in: Capsule())
        }
        .buttonStyle(.plain)
    }
}
