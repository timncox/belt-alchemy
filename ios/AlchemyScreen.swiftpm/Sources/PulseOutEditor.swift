// Every setting of one Pulse output, as a sheet over the main screen: the
// panel's OUTPUT layer (ratio, wave, width, level, offset, phase) and its
// PATTERN layer (steps, fills, rotate, chance, swing, slew), with the
// voltage and the pattern redrawn as they change. Plus what a touch screen
// can do that six pots can't: step to the next output, mute, randomize the
// pattern, copy one output onto another, put it back to its defaults.

import SwiftUI
import AlchemyLink

struct PulseOutEditor: View {
    @EnvironmentObject var store: ModuleStore
    @Binding var index: Int
    @Binding var muted: [Int: Double]
    @Binding var clip: Pulse.Out?
    var colorOf: (Int) -> Color

    private var out: Pulse.Out { Pulse.Out(index, values: store.values) }
    private var color: Color { colorOf(index) }
    private var isMuted: Bool { muted[index] != nil }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                header
                PulseWave(out: out, color: color, cycles: out.steps > 0 ? min(out.steps, 8) : 2,
                          cycleSeconds: cycleSeconds, width: 2.5)
                    .frame(height: 110)
                    .overlay {
                        // With STATE, where the module is; without, free-running at this output's rate.
                        let st = store.state?.pulse
                        let moving = st?.running ?? true
                        PulsePoint(out: out, live: st.flatMap { $0.outs.indices.contains(index) ? $0.outs[index] : nil },
                                   at: store.stateDate, cyclesPerSecond: moving ? 1 / max(0.0005, cycleSeconds) : 0,
                                   cycleSeconds: cycleSeconds, color: Theme.mist, radius: 5)
                    }
                    .overlay(alignment: .topLeading) { scaleLabel("+5 V") }
                    .overlay(alignment: .bottomLeading) { scaleLabel("−5 V") }
                wavePicker
                ratioPicker
                VStack(spacing: 16) {
                    slider(.width, title: out.wave.widthName, dim: !out.wave.usesWidth)
                    slider(.level, title: "Level")
                    slider(.offset, title: "Offset", centre: true)
                    slider(.phase, title: "Phase")
                    slider(.slew, title: "Slew")
                }
                pattern
                actions
            }
            .padding(.horizontal, 20).padding(.top, 22).padding(.bottom, 40)
        }
        .background(Theme.ground.ignoresSafeArea())
    }

    // MARK: pieces

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text("OUT \(index + 1)").font(Theme.display(30)).foregroundStyle(color)
                .accessibilityIdentifier("pulse-editor-title")
            Text(isMuted ? "muted" : store.pages.isEmpty ? "" : "J\(index + 3)")
                .font(.subheadline.weight(.semibold)).foregroundStyle(Theme.fog)
            Spacer()
            stepButton("chevron.left", label: "Previous output") { index = (index + Pulse.outs - 1) % Pulse.outs }
            stepButton("chevron.right", label: "Next output") { index = (index + 1) % Pulse.outs }
                .accessibilityIdentifier("pulse-editor-next")
        }
    }

    private func stepButton(_ symbol: String, label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 17, weight: .bold)).foregroundStyle(Theme.mist)
                .frame(width: 44, height: 44).background(Theme.surface, in: Circle())
        }
        .buttonStyle(.plain).accessibilityLabel(label)
    }

    private func scaleLabel(_ s: String) -> some View {
        Text(s).font(.caption2).foregroundStyle(Theme.fog.opacity(0.7))
    }

    private var wavePicker: some View {
        let labels = field(.wave)?.disp.labels ?? []
        return VStack(alignment: .leading, spacing: 8) {
            sectionTitle("WAVE", value: labels.indices.contains(out.wave.rawValue) ? labels[out.wave.rawValue] : "")
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(Pulse.Wave.allCases, id: \.rawValue) { w in
                        let on = w == out.wave
                        Button { set(.wave, Double(w.rawValue)) } label: {
                            VStack(spacing: 5) {
                                PulseWave(out: glyph(w), color: on ? color : Theme.fog, cycles: 1, cycleSeconds: 0.5, width: 1.8)
                                    .frame(width: 46, height: 26)
                                Text(labels.indices.contains(w.rawValue) ? labels[w.rawValue] : "\(w.rawValue)")
                                    .font(.caption2.weight(on ? .bold : .regular))
                                    .foregroundStyle(on ? Theme.mist : Theme.fog)
                            }
                            .padding(.vertical, 8).padding(.horizontal, 6)
                            .background(on ? color.opacity(0.14) : Theme.surface, in: RoundedRectangle(cornerRadius: 10))
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(labels.indices.contains(w.rawValue) ? labels[w.rawValue] : "Wave \(w.rawValue)")
                        .accessibilityAddTraits(on ? .isSelected : [])
                    }
                }
            }
        }
    }

    /// A wave on its own, full height, for the picker's buttons.
    private func glyph(_ w: Pulse.Wave) -> Pulse.Out {
        var o = Pulse.Out()
        o.wave = w; o.level = 1; o.offset = 0; o.width = w == .trig ? 1 : 0.5
        return o
    }

    private var ratioPicker: some View {
        let labels = field(.ratio)?.disp.labels ?? []
        return VStack(alignment: .leading, spacing: 8) {
            sectionTitle("RATIO", value: labels.indices.contains(out.ratio) ? ratioWords(labels[out.ratio]) : "")
            ScrollViewReader { proxy in
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(Array(labels.enumerated()), id: \.offset) { i, l in
                            let on = i == out.ratio
                            Button { set(.ratio, Double(i)) } label: {
                                Text(l).font(Theme.display(15, weight: .bold))
                                    .foregroundStyle(on ? Color.black : Theme.mist)
                                    .padding(.horizontal, 12).frame(minWidth: 46, minHeight: 44)
                                    .background(on ? AnyShapeStyle(color) : AnyShapeStyle(Theme.surface), in: Capsule())
                            }
                            .buttonStyle(.plain).id(i)
                            .accessibilityAddTraits(on ? .isSelected : [])
                        }
                    }
                }
                .onAppear { proxy.scrollTo(out.ratio, anchor: .center) }
                .onChange(of: index) { _ in proxy.scrollTo(out.ratio, anchor: .center) }
            }
        }
    }

    private func ratioWords(_ l: String) -> String {
        guard let r = Pulse.cyclesPerBeat(l) else { return l }
        if r == 1 { return "one cycle a beat" }
        let n = r > 1 ? r : 1 / r
        let s = n == n.rounded() ? String(Int(n)) : String(n)
        return r > 1 ? "\(s) cycles a beat" : "one cycle in \(s) beats"
    }

    private var pattern: some View {
        VStack(alignment: .leading, spacing: 14) {
            sectionTitle("PATTERN", value: out.steps > 0 ? "\(out.fills) hits in \(out.steps) steps" : "off: every cycle plays")
            HStack(alignment: .center, spacing: 18) {
                PulseRing(hits: out.steps > 0 ? out.hits : [true], color: color, dot: 5)
                    .frame(width: 112, height: 112)
                VStack(spacing: 10) {
                    stepper("Steps", value: out.steps, show: out.steps == 0 ? "Off" : "\(out.steps)", range: 0...Pulse.maxSteps) { setSteps($0) }
                    stepper("Fills", value: out.fills, show: "\(out.fills)", range: 0...max(0, out.steps)) { set(.fills, Double($0)) }
                    stepper("Rotate", value: out.rotate, show: "\(out.rotate)", range: 0...max(0, out.steps - 1)) { set(.rotate, Double($0)) }
                }
            }
            slider(.chance, title: "Chance")
            slider(.swing, title: "Swing")
        }
    }

    private func stepper(_ title: String, value: Int, show: String, range: ClosedRange<Int>, _ write: @escaping (Int) -> Void) -> some View {
        HStack(spacing: 8) {
            Text(title).font(.subheadline).foregroundStyle(Theme.fog).frame(width: 56, alignment: .leading)
            Spacer(minLength: 0)
            Button { write(max(range.lowerBound, value - 1)) } label: {
                Image(systemName: "minus").font(.system(size: 15, weight: .bold)).frame(width: 44, height: 40)
                    .background(Theme.surface, in: RoundedRectangle(cornerRadius: 9))
            }
            .buttonStyle(.plain).foregroundStyle(value > range.lowerBound ? Theme.mist : Theme.fog.opacity(0.4))
            .accessibilityLabel("\(title) down")
            Text(show).font(Theme.display(17, weight: .bold)).monospacedDigit().foregroundStyle(Theme.mist)
                .frame(width: 44)
                .accessibilityIdentifier("pulse-\(title.lowercased())-value")
            Button { write(min(range.upperBound, value + 1)) } label: {
                Image(systemName: "plus").font(.system(size: 15, weight: .bold)).frame(width: 44, height: 40)
                    .background(Theme.surface, in: RoundedRectangle(cornerRadius: 9))
            }
            .buttonStyle(.plain).foregroundStyle(value < range.upperBound ? Theme.mist : Theme.fog.opacity(0.4))
            .accessibilityLabel("\(title) up")
            .accessibilityIdentifier("pulse-\(title.lowercased())-up")
        }
    }

    private var actions: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionTitle("MORE", value: "")
            HStack(spacing: 8) {
                PulseChip(title: isMuted ? "UNMUTE" : "MUTE", strong: isMuted, tint: color) { toggleMute() }
                    .accessibilityIdentifier("pulse-mute")
                PulseChip(title: "DICE") { randomize() }.accessibilityIdentifier("pulse-dice")
                    .accessibilityLabel("Randomize the pattern")
                PulseChip(title: "COPY") { clip = out }.accessibilityIdentifier("pulse-copy")
                PulseChip(title: "PASTE", tint: clip == nil ? Theme.fog.opacity(0.5) : Theme.mist) { if let c = clip { write(c) } }
                    .disabled(clip == nil).accessibilityIdentifier("pulse-paste")
            }
            PulseChip(title: "BACK TO DEFAULTS", tint: Theme.fog) { muted[index] = nil; write(Pulse.Out()) }
                .accessibilityIdentifier("pulse-reset")
        }
    }

    private func sectionTitle(_ t: String, value: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(t).font(.caption.weight(.semibold)).tracking(1.4).foregroundStyle(Theme.fog)
            Spacer()
            Text(value).font(.caption).foregroundStyle(Theme.fog)
        }
    }

    private func slider(_ key: Pulse.Key, title: String, centre: Bool = false, dim: Bool = false) -> some View {
        let f = field(key)
        let v = f.map { store.value($0) } ?? 0
        return PulseSlider(title: title, text: f?.show(v) ?? "", value: v, color: dim ? Theme.fog : color, centre: centre) { nv in
            if key == .level, nv > 0 { muted[index] = nil }
            if let f { store.set(f, nv) }
        }
        .opacity(dim ? 0.45 : 1)
        .accessibilityIdentifier("pulse-\(key.rawValue)")
    }

    // MARK: writes

    private func field(_ key: Pulse.Key) -> Field? { store.desc?.field(Pulse.id(index, key)) }
    private func set(_ key: Pulse.Key, _ v: Double) { if let f = field(key) { store.set(f, v) } }

    private var cycleSeconds: Double {
        let labels = field(.ratio)?.disp.labels ?? []
        let rate = labels.indices.contains(out.ratio) ? Pulse.cyclesPerBeat(labels[out.ratio]) ?? 1 : 1
        var bpm = Pulse.bpm(fromKnob: store.values[Pulse.tempoId] ?? Pulse.knob(fromBpm: 120))
        if let l = store.state?.pulse, l.bpm > 0 { bpm = l.bpm }      // J1's tempo while following it
        return 60 / max(1, bpm) / rate
    }

    /// Fewer steps pulls fills and rotate inside the pattern, as the module does.
    private func setSteps(_ n: Int) {
        set(.steps, Double(n))
        if out.fills > n { set(.fills, Double(n)) }
        if out.rotate >= max(1, n) { set(.rotate, 0) }
    }

    private func toggleMute() {
        guard let f = field(.level) else { return }
        if let was = muted[index] { store.set(f, was); muted[index] = nil }
        else { muted[index] = store.value(f); store.set(f, 0) }
    }

    private func randomize() {
        var rng = SystemRandomNumberGenerator()
        let p = Pulse.randomPattern(using: &rng)
        set(.steps, Double(p.steps)); set(.fills, Double(p.fills)); set(.rotate, Double(p.rotate))
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }

    private func write(_ o: Pulse.Out) {
        set(.ratio, Double(o.ratio)); set(.wave, Double(o.wave.rawValue))
        set(.width, o.width); set(.level, o.level); set(.offset, o.offset); set(.phase, o.phase)
        set(.steps, Double(o.steps)); set(.fills, Double(o.fills)); set(.rotate, Double(o.rotate))
        set(.chance, o.chance); set(.swing, o.swing); set(.slew, o.slew)
    }
}

/// A slider for a 0…1 setting: its name, its value in the module's own
/// units, and the system's slider (so the sheet still scrolls when a drag
/// starts on one; a drag gesture of our own would take every swipe).
struct PulseSlider: View {
    var title: String
    var text: String
    var value: Double
    var color: Color
    var centre = false
    var onChange: (Double) -> Void

    var body: some View {
        VStack(spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(title).font(.subheadline).foregroundStyle(Theme.fog)
                Spacer()
                Text(text).font(Theme.display(15, weight: .bold)).monospacedDigit().foregroundStyle(Theme.mist)
            }
            .accessibilityHidden(true)
            Slider(value: Binding(get: { min(1, max(0, value)) }, set: { onChange($0) }), in: 0...1)
                .tint(color)
                .background(alignment: .center) {
                    if centre { Rectangle().fill(Theme.fog.opacity(0.6)).frame(width: 1, height: 18) }
                }
                .accessibilityLabel(title)
                .accessibilityValue(text)
        }
    }
}
