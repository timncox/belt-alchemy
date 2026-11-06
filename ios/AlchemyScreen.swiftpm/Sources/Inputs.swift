// The app's own controls as live values: the pad (X, Y and pressure, with
// hold, spring-back and a recorded loop), the phone's tilt, roll and turn,
// and the mic's level and pitch. Each sends what changed to the Rig, which
// moves whatever knobs the Controls page routed them to, on either module.
// Motion and the mic are off until switched on, so nothing moves by accident.

import AVFoundation
import CoreMotion
import Foundation
import SwiftUI
import AlchemyLink

@MainActor
final class Inputs: ObservableObject {
    weak var rig: Rig?

    // MARK: the pad

    enum Release: String, CaseIterable, Identifiable {
        case hold, spring
        var id: String { rawValue }
        var title: String { self == .hold ? "Hold" : "Spring back" }
    }

    @Published var pad = CGPoint(x: 0.5, y: 0.5)
    @Published var pressure = 0.0
    @Published var touching = false
    @Published var release: Release = .hold
    @Published var recordArmed = false
    @Published private(set) var recording = false
    @Published private(set) var looping = false
    @Published private(set) var motion = PadMotion()
    private var loopTask: Task<Void, Never>?
    private var loopStart = Date()

    func padBegan() {
        touching = true
        stopLoop()
        if recordArmed { motion.start(); recording = true }
    }

    /// x and y 0...1 (y up), pressure 0...1.
    func padMoved(x: Double, y: Double, pressure p: Double) {
        pad = CGPoint(x: x, y: y)
        pressure = p
        if recording { motion.add(t: Date().timeIntervalSinceReferenceDate, x: x, y: y, p: p) }
        rig?.drive([.padX: x, .padY: y, .pressure: p])
    }

    func padEnded() {
        touching = false
        if recording {
            recording = false
            recordArmed = false
            motion.finish()
            if !motion.isEmpty { playLoop() }
            return
        }
        pressure = 0
        if release == .spring {
            withAnimation(.spring(response: 0.25)) { pad = CGPoint(x: 0.5, y: 0.5) }
            rig?.drive([.padX: 0.5, .padY: 0.5, .pressure: 0])
        } else {
            rig?.drive([.pressure: 0])
        }
    }

    func playLoop() {
        guard !motion.isEmpty else { return }
        loopTask?.cancel()
        looping = true
        loopStart = Date()
        loopTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                if let p = self.motion.at(Date().timeIntervalSince(self.loopStart)) {
                    self.pad = CGPoint(x: p.x, y: p.y)
                    self.pressure = p.p
                    self.rig?.drive([.padX: p.x, .padY: p.y, .pressure: p.p])
                }
                try? await Task.sleep(nanoseconds: 16_000_000)
            }
        }
    }

    func stopLoop() {
        loopTask?.cancel(); loopTask = nil
        looping = false
    }

    func clearLoop() { stopLoop(); motion = PadMotion() }

    // MARK: tilt, roll and turn

    @Published var motionOn = false { didSet { motionOn ? startMotion() : stopMotion() } }
    @Published var motionRange = 40.0 { didSet { for k in shapers.keys { shapers[k]!.rangeDegrees = motionRange } } }
    @Published private(set) var motionValues: [ControlSource: Double] = [:]
    private let cm = CMMotionManager()
    private var shapers: [ControlSource: AngleShaper] = [.tilt: AngleShaper(), .roll: AngleShaper(), .turn: AngleShaper()]
    private var smoothers: [ControlSource: Smoother] = [.tilt: Smoother(seconds: 0.08), .roll: Smoother(seconds: 0.08),
                                                         .turn: Smoother(seconds: 0.08)]
    private var lastAttitude: (pitch: Double, roll: Double, yaw: Double)?
    var motionAvailable: Bool { cm.isDeviceMotionAvailable }

    private func startMotion() {
        guard cm.isDeviceMotionAvailable else { motionOn = false; return }
        cm.deviceMotionUpdateInterval = 1.0 / 60
        cm.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: .main) { [weak self] m, _ in
            guard let a = m?.attitude else { return }
            let (p, r, y) = (a.pitch, a.roll, a.yaw)
            Task { @MainActor in self?.motionUpdate(pitch: p, roll: r, yaw: y) }
        }
    }

    private func stopMotion() {
        cm.stopDeviceMotionUpdates()
        for k in smoothers.keys { smoothers[k]!.reset() }
    }

    private func motionUpdate(pitch: Double, roll: Double, yaw: Double) {
        if lastAttitude == nil { zeroMotion(pitch: pitch, roll: roll, yaw: yaw) }
        lastAttitude = (pitch, roll, yaw)
        var out: [ControlSource: Double] = [:]
        for (src, angle) in [(ControlSource.tilt, pitch), (.roll, roll), (.turn, yaw)] {
            let v = shapers[src]!.value(angle)
            out[src] = smoothers[src]!.step(v, dt: 1.0 / 60)
        }
        motionValues = out
        rig?.drive(out)
    }

    /// The way the phone is held now becomes the middle of each range.
    func zeroMotion() {
        guard let a = lastAttitude else { return }
        zeroMotion(pitch: a.pitch, roll: a.roll, yaw: a.yaw)
    }

    private func zeroMotion(pitch: Double, roll: Double, yaw: Double) {
        shapers[.tilt]!.zero = pitch
        shapers[.roll]!.zero = roll
        shapers[.turn]!.zero = yaw
        for k in smoothers.keys { smoothers[k]!.reset() }
    }

    // MARK: the mic

    @Published var micOn = false { didSet { micOn ? startMic() : stopMic() } }
    @Published private(set) var micLevel = 0.0
    @Published private(set) var micNote: Double?
    @Published var micProblem: String?
    private let engine = AVAudioEngine()
    private var follower = LevelFollower()
    private var pitchSmooth = Smoother(seconds: 0.05)

    private func startMic() {
        let session = AVAudioSession.sharedInstance()
        session.requestRecordPermission { ok in
            Task { @MainActor in
                guard ok else {
                    self.micProblem = "Alchemy Screen can't hear the mic. Allow it in Settings > Privacy > Microphone."
                    self.micOn = false
                    return
                }
                do {
                    try session.setCategory(.playAndRecord, mode: .measurement, options: [.mixWithOthers, .defaultToSpeaker])
                    try session.setActive(true)
                    let input = self.engine.inputNode
                    let fmt = input.outputFormat(forBus: 0)
                    let sr = fmt.sampleRate
                    input.removeTap(onBus: 0)
                    input.installTap(onBus: 0, bufferSize: 2048, format: fmt) { [weak self] buf, _ in
                        guard let ch = buf.floatChannelData?[0] else { return }
                        let n = Int(buf.frameLength)
                        let x = Array(UnsafeBufferPointer(start: ch, count: n))
                        var sum: Float = 0
                        for v in x { sum += v * v }
                        let rms = Double(sqrt(sum / Float(max(1, n))))
                        let hz = rms > 0.01 ? PitchDetector.frequency(x, sampleRate: sr) : nil
                        let dt = Double(n) / sr
                        Task { @MainActor in self?.micUpdate(rms: rms, hz: hz, dt: dt) }
                    }
                    try self.engine.start()
                } catch {
                    self.micProblem = "The mic didn't start: \(error.localizedDescription)"
                    self.micOn = false
                }
            }
        }
    }

    private func stopMic() {
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        micLevel = 0; micNote = nil
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    private func micUpdate(rms: Double, hz: Double?, dt: Double) {
        guard micOn else { return }
        micLevel = follower.step(rms: rms, dt: dt)
        var out: [ControlSource: Double] = [.micLevel: micLevel]
        if let hz {
            micNote = PitchDetector.note(hz)
            out[.micPitch] = pitchSmooth.step(PitchDetector.unit(hz), dt: dt)
        } else {
            micNote = nil       // hold the last pitch while there is none
        }
        rig?.drive(out)
    }

    /// A source's live value, for the meters on the Controls page.
    func value(_ s: ControlSource) -> Double {
        switch s {
        case .padX: return pad.x
        case .padY: return pad.y
        case .pressure: return pressure
        case .tilt, .roll, .turn: return motionValues[s] ?? 0.5
        case .micLevel: return micLevel
        case .micPitch: return micNote.map { min(1, max(0, ($0 - 40) / 44)) } ?? 0
        }
    }
}
