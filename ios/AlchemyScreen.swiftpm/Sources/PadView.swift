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
            VStack(spacing: 12) {
                HStack(spacing: 10) {
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
                HStack(spacing: 10) {
                    Picker("Release", selection: $inputs.release) {
                        ForEach(Inputs.Release.allCases) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .frame(maxWidth: 240)
                    Spacer()
                    if inputs.looping {
                        Button { inputs.stopLoop() } label: { Label("Stop loop", systemImage: "stop.fill") }
                    } else if !inputs.motion.isEmpty {
                        Button { inputs.playLoop() } label: { Label("Play loop", systemImage: "repeat") }
                    }
                    Toggle(isOn: $inputs.recordArmed) {
                        Label(inputs.recording ? "Recording" : "Record", systemImage: "record.circle")
                    }
                    .toggleStyle(.button)
                    .tint(.red)
                }
                .buttonStyle(.bordered)
            }
            .padding()
            .navigationTitle("Pad")
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    private func axis(_ s: ControlSource, _ t: String) -> some View {
        let r = rig.target(of: s)
        return VStack(alignment: .leading, spacing: 2) {
            Text(t).font(.caption.weight(.bold)).foregroundStyle(.orange)
            Text(r.map { rig.label($0.target) } ?? "Not set")
                .font(.caption).lineLimit(1)
                .foregroundStyle(r == nil ? Color.secondary : Color.primary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
        .background(Color.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
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
                RoundedRectangle(cornerRadius: 18).fill(Color.white.opacity(0.05))
                Canvas { ctx, size in
                    for i in 1..<4 {
                        let f = CGFloat(i) / 4
                        var p = Path()
                        p.move(to: CGPoint(x: size.width * f, y: 0)); p.addLine(to: CGPoint(x: size.width * f, y: size.height))
                        p.move(to: CGPoint(x: 0, y: size.height * f)); p.addLine(to: CGPoint(x: size.width, y: size.height * f))
                        ctx.stroke(p, with: .color(.white.opacity(i == 2 ? 0.12 : 0.05)))
                    }
                    if !inputs.motion.isEmpty {
                        var trail = Path()
                        for (i, pt) in inputs.motion.points.enumerated() {
                            let q = CGPoint(x: pt.x * size.width, y: (1 - pt.y) * size.height)
                            if i == 0 { trail.move(to: q) } else { trail.addLine(to: q) }
                        }
                        ctx.stroke(trail, with: .color(.orange.opacity(0.3)), lineWidth: 2)
                    }
                }
                let r = 14 + 26 * inputs.pressure
                Circle()
                    .fill(Color.orange.opacity(inputs.touching || inputs.looping ? 0.9 : 0.45))
                    .frame(width: r * 2, height: r * 2)
                    .shadow(color: .orange.opacity(0.6), radius: 10 * inputs.pressure + 2)
                    .position(x: inputs.pad.x * w, y: (1 - inputs.pad.y) * h)
                TouchCatcher(
                    began: { inputs.padBegan() },
                    moved: { x, y, p in inputs.padMoved(x: x, y: 1 - y, pressure: p) },
                    ended: { inputs.padEnded() })
            }
            .clipShape(RoundedRectangle(cornerRadius: 18))
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
