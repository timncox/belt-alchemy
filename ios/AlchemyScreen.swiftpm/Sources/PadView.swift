// The XY pad: drag for X and Y, press harder for the third axis. Apple
// Pencil reports real force; a finger on an iPhone (which has had no 3D
// Touch since 2019) reports how much of the fingertip touches the glass, so
// pressing flatter reads as pressing harder. Each axis goes to whatever knob
// the Controls page gave it, on either module. Hold keeps the last place,
// Spring back returns to the middle, and Record keeps the next gesture and
// plays it round as a loop.

import SwiftUI
import UIKit
import AlchemyLink

struct PadView: View {
    @EnvironmentObject var rig: Rig
    @EnvironmentObject var inputs: Inputs

    var body: some View {
        NavigationStack {
            VStack(spacing: 16) {
                HStack(alignment: .top, spacing: 14) {
                    axis(.padX, "X")
                    axis(.padY, "Y")
                    axis(.pressure, "Press")
                }
                GeometryReader { g in
                    let side = min(g.size.width, g.size.height)
                    PadSurface()
                        .frame(width: side, height: side)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                SectionTabs(items: Inputs.Release.allCases, title: { $0.title }, selection: $inputs.release)
                    .padding(.horizontal, -16)
                HStack(spacing: 10) {
                    Spacer()
                    if inputs.looping {
                        pill("Stop loop", "stop.fill", on: false) { inputs.stopLoop() }
                    } else if !inputs.motion.isEmpty {
                        pill("Play loop", "repeat", on: false) { inputs.playLoop() }
                    }
                    pill(inputs.recording ? "Recording" : "Record", "record.circle", on: inputs.recordArmed, tint: Theme.alarm) {
                        inputs.recordArmed.toggle()
                    }
                }
            }
            .padding()
            .background(Theme.ground)
            .toolbar(.hidden, for: .navigationBar)
        }
    }

    private func axis(_ s: ControlSource, _ t: String) -> some View {
        let r = rig.target(of: s)
        return VStack(alignment: .leading, spacing: 3) {
            Text(t).font(Theme.display(22, weight: .black))
                .foregroundStyle(r == nil ? Theme.fog : Theme.key)
            Text(r.map { rig.label($0.target) } ?? "Not set")
                .font(.footnote).lineLimit(1)
                .foregroundStyle(r == nil ? Theme.fog : Theme.mist)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func pill(_ title: String, _ icon: String, on: Bool, tint: Color = Theme.mist,
                      action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: icon)
                .font(.subheadline.weight(.semibold))
                .lineLimit(1).fixedSize()
                .foregroundStyle(on ? Color.black : Theme.mist)
                .padding(.horizontal, 16).padding(.vertical, 11)
                .background {
                    if on { Capsule().fill(tint).shadow(color: tint.opacity(0.5), radius: 10) }
                    else { Capsule().fill(.ultraThinMaterial) }
                }
                .overlay(Capsule().stroke(Color.white.opacity(on ? 0 : 0.10)))
        }
        .buttonStyle(.plain)
    }
}

/// The square itself: grid, trail of a recorded loop, the finger's dot
/// (bigger with pressure).
private struct PadSurface: View {
    @EnvironmentObject var inputs: Inputs

    var body: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            ZStack {
                RoundedRectangle(cornerRadius: 28, style: .continuous).fill(Theme.surface)
                Canvas { ctx, size in
                    // A field of fine dots, the centre lines a touch brighter.
                    let step = size.width / 16
                    for gx in 1..<16 {
                        for gy in 1..<16 {
                            let centre = gx == 8 || gy == 8
                            let p = CGPoint(x: CGFloat(gx) * step, y: CGFloat(gy) * size.height / 16)
                            ctx.fill(Path(ellipseIn: CGRect(x: p.x - 1, y: p.y - 1, width: 2, height: 2)),
                                     with: .color(Theme.mist.opacity(centre ? 0.22 : 0.08)))
                        }
                    }
                    if !inputs.motion.isEmpty {
                        var trail = Path()
                        for (i, pt) in inputs.motion.points.enumerated() {
                            let q = CGPoint(x: pt.x * size.width, y: (1 - pt.y) * size.height)
                            if i == 0 { trail.move(to: q) } else { trail.addLine(to: q) }
                        }
                        var glow = ctx
                        glow.addFilter(.blur(radius: 5))
                        glow.stroke(trail, with: .color(Theme.key.opacity(0.5)), style: StrokeStyle(lineWidth: 5, lineCap: .round, lineJoin: .round))
                        ctx.stroke(trail, with: .color(Theme.key.opacity(0.8)), style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
                    }
                }
                let r = 16 + 30 * inputs.pressure
                let live = inputs.touching || inputs.looping
                Circle()
                    .fill(RadialGradient(colors: [Theme.mist, Theme.key, Theme.key.opacity(0)],
                                         center: .center, startRadius: 0, endRadius: r))
                    .frame(width: r * 2, height: r * 2)
                    .opacity(live ? 1 : 0.55)
                    .shadow(color: Theme.key.opacity(0.7), radius: 12 * inputs.pressure + 4)
                    .position(x: inputs.pad.x * w, y: (1 - inputs.pad.y) * h)
                TouchCatcher(
                    began: { inputs.padBegan() },
                    moved: { x, y, p in inputs.padMoved(x: x, y: 1 - y, pressure: p) },
                    ended: { inputs.padEnded() })
            }
            .clipShape(RoundedRectangle(cornerRadius: 28, style: .continuous))
        }
        .accessibilityElement()
        .accessibilityLabel("XY pad")
    }
}

/// A UIKit view under the finger, for what SwiftUI's drag doesn't give:
/// the Pencil's force and the finger's contact size.
private struct TouchCatcher: UIViewRepresentable {
    var began: () -> Void
    var moved: (Double, Double, Double) -> Void
    var ended: () -> Void

    func makeUIView(context: Context) -> TouchView {
        let v = TouchView()
        v.backgroundColor = .clear
        v.isMultipleTouchEnabled = false
        return v
    }

    func updateUIView(_ v: TouchView, context: Context) {
        v.began = began; v.moved = moved; v.ended = ended
    }

    final class TouchView: UIView {
        var began: (() -> Void)?
        var moved: ((Double, Double, Double) -> Void)?
        var ended: (() -> Void)?
        private var tracked: UITouch?
        /// Contact size seen so far, so "pressing" fits this finger.
        private var radiusLo: CGFloat = 12, radiusHi: CGFloat = 26

        override func touchesBegan(_ t: Set<UITouch>, with e: UIEvent?) {
            guard tracked == nil, let touch = t.first else { return }
            tracked = touch
            began?()
            report(touch, e)
        }
        override func touchesMoved(_ t: Set<UITouch>, with e: UIEvent?) {
            guard let touch = tracked, t.contains(touch) else { return }
            report(touch, e)
        }
        override func touchesEnded(_ t: Set<UITouch>, with e: UIEvent?) { finish(t) }
        override func touchesCancelled(_ t: Set<UITouch>, with e: UIEvent?) { finish(t) }

        private func finish(_ t: Set<UITouch>) {
            guard let touch = tracked, t.contains(touch) else { return }
            tracked = nil
            ended?()
        }

        private func report(_ touch: UITouch, _ e: UIEvent?) {
            // Coalesced touches: the Pencil reports up to 240 a second.
            let all = e?.coalescedTouches(for: touch) ?? [touch]
            guard let last = all.last else { return }
            let p = last.location(in: self)
            let x = Double(min(1, max(0, p.x / max(1, bounds.width))))
            let y = Double(min(1, max(0, p.y / max(1, bounds.height))))
            moved?(x, y, pressure(last))
        }

        private func pressure(_ t: UITouch) -> Double {
            if t.type == .pencil || traitCollection.forceTouchCapability == .available, t.maximumPossibleForce > 0 {
                return Double(min(1, t.force / t.maximumPossibleForce))
            }
            let r = t.majorRadius
            radiusLo = min(radiusLo, r)
            radiusHi = max(radiusHi, r)
            return Double(min(1, max(0, (r - radiusLo) / max(4, radiusHi - radiusLo))))
        }
    }
}
