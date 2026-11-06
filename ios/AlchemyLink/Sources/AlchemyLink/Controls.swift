// The app's own controls (the XY pad's X, Y and pressure, the phone's tilt
// and turn, the mic's loudness and pitch) and the knobs they move. Any
// source can go to any knob on any connected module, over its own range,
// turned upside down if wanted. Kept free of UIKit so it is tested on Linux.

import Foundation

public enum ControlSource: String, CaseIterable, Codable, Sendable, Identifiable {
    case padX, padY, pressure, tilt, roll, turn, micLevel, micPitch
    public var id: String { rawValue }
    public var name: String {
        switch self {
        case .padX: return "Pad X"
        case .padY: return "Pad Y"
        case .pressure: return "Pressure"
        case .tilt: return "Tilt"
        case .roll: return "Roll"
        case .turn: return "Turn"
        case .micLevel: return "Mic level"
        case .micPitch: return "Mic pitch"
        }
    }
    public var isPad: Bool { self == .padX || self == .padY || self == .pressure }
    public var isMotion: Bool { self == .tilt || self == .roll || self == .turn }
    public var isMic: Bool { self == .micLevel || self == .micPitch }
}

/// A knob on one module: the module's unit number (from Phone INFO, the
/// same across cables and adapters) and the descriptor's field id.
public struct ControlTarget: Hashable, Codable, Sendable {
    public var unit: UInt32
    public var field: String
    public init(unit: UInt32, field: String) { self.unit = unit; self.field = field }
}

public struct ControlRoute: Identifiable, Hashable, Codable, Sendable {
    public var id = UUID()
    public var source: ControlSource
    public var target: ControlTarget
    /// The knob's 0...1 range the source sweeps; lo > hi turns it round.
    public var lo: Double = 0, hi: Double = 1
    public init(source: ControlSource, target: ControlTarget, lo: Double = 0, hi: Double = 1) {
        self.source = source; self.target = target; self.lo = lo; self.hi = hi
    }
    public func map(_ v: Double) -> Double {
        let c = min(1, max(0, v))
        return min(1, max(0, lo + (hi - lo) * c))
    }
}

/// All routes, and what a set of source values does to the knobs. When two
/// sources share a knob the later route wins, as on a mod matrix.
public struct ControlMatrix: Codable, Sendable {
    public var routes: [ControlRoute] = []
    public init(routes: [ControlRoute] = []) { self.routes = routes }

    public func targets(for s: ControlSource) -> [ControlRoute] { routes.filter { $0.source == s } }

    public func apply(_ inputs: [ControlSource: Double]) -> [ControlTarget: Double] {
        var out: [ControlTarget: Double] = [:]
        for r in routes { if let v = inputs[r.source] { out[r.target] = r.map(v) } }
        return out
    }

    /// Drop routes to modules that aren't here (or fields they don't have).
    public mutating func prune(keeping ok: (ControlTarget) -> Bool) { routes.removeAll { !ok($0.target) } }
}

/// One-pole smoothing at a set time constant, for tilt and the mic.
public struct Smoother: Sendable {
    public var seconds: Double
    public private(set) var value: Double?
    public init(seconds: Double) { self.seconds = seconds }
    public mutating func step(_ x: Double, dt: Double) -> Double {
        guard let v = value, seconds > 0 else { value = x; return x }
        let a = 1 - exp(-max(0, dt) / seconds)
        value = v + (x - v) * a
        return value!
    }
    public mutating func reset() { value = nil }
}

/// Tilt, roll or turn in radians, to 0...1: zero at the calibrated rest
/// angle maps to 0.5, and `range` degrees either side reaches the ends.
public struct AngleShaper: Codable, Sendable {
    public var rangeDegrees: Double = 40
    public var zero: Double = 0
    public init(rangeDegrees: Double = 40, zero: Double = 0) { self.rangeDegrees = rangeDegrees; self.zero = zero }
    public func value(_ radians: Double) -> Double {
        var d = (radians - zero) * 180 / .pi
        while d > 180 { d -= 360 }        // turn wraps
        while d < -180 { d += 360 }
        return min(1, max(0, 0.5 + d / (2 * max(1, rangeDegrees))))
    }
}

/// Mic loudness to 0...1: -60 dBFS is 0, `ceiling` is 1, with a fast attack
/// and slower release, like an envelope follower.
public struct LevelFollower: Sendable {
    public var floorDb = -60.0, ceilingDb = -6.0
    public var attack = 0.01, release = 0.25
    public private(set) var value = 0.0
    public init() {}
    public mutating func step(rms: Double, dt: Double) -> Double {
        let db = rms > 0 ? 20 * log10(rms) : -120
        let x = min(1, max(0, (db - floorDb) / (ceilingDb - floorDb)))
        let t = x > value ? attack : release
        value += (x - value) * (1 - exp(-max(0, dt) / max(1e-4, t)))
        return value
    }
}

/// A sung or played pitch from the mic, by autocorrelation with a
/// normalised difference (the YIN idea, cut down). Returns the frequency in
/// Hz, or nil when the sound isn't clearly pitched.
public enum PitchDetector {
    public static func frequency(_ x: [Float], sampleRate: Double,
                                 lowHz: Double = 70, highHz: Double = 1000, threshold: Float = 0.15) -> Double? {
        let n = x.count
        let minLag = max(2, Int(sampleRate / highHz)), maxLag = min(n / 2, Int(sampleRate / lowHz))
        guard maxLag > minLag + 2 else { return nil }
        var d = [Float](repeating: 0, count: maxLag + 1)
        let w = n - maxLag
        for lag in 1...maxLag {
            var s: Float = 0
            for i in 0..<w { let e = x[i] - x[i + lag]; s += e * e }
            d[lag] = s
        }
        // Cumulative mean normalised difference.
        var cm = [Float](repeating: 1, count: maxLag + 1)
        var run: Float = 0
        for lag in 1...maxLag { run += d[lag]; cm[lag] = run > 0 ? d[lag] * Float(lag) / run : 1 }
        var lag = minLag
        while lag < maxLag {
            if cm[lag] < threshold {
                while lag + 1 < maxLag && cm[lag + 1] < cm[lag] { lag += 1 }
                break
            }
            lag += 1
        }
        guard lag < maxLag, cm[lag] < threshold else { return nil }
        // Parabolic step between lags for a finer answer.
        let a = cm[lag - 1], b = cm[lag], c = cm[lag + 1]
        let den = a - 2 * b + c
        let shift = den != 0 ? Double((a - c) / (2 * den)) : 0
        return sampleRate / (Double(lag) + max(-1, min(1, shift)))
    }

    /// A frequency as a MIDI note (69 = A 440), and as 0...1 over a range.
    public static func note(_ hz: Double) -> Double { 69 + 12 * log2(hz / 440) }
    public static func unit(_ hz: Double, low: Double = 40, high: Double = 84) -> Double {
        min(1, max(0, (note(hz) - low) / (high - low)))
    }
}

/// A finger's path on the pad, recorded and played back as a loop.
public struct PadMotion: Codable, Sendable {
    public struct Point: Codable, Sendable { public var t: Double, x: Double, y: Double, p: Double }
    public private(set) var points: [Point] = []
    public private(set) var length: Double = 0
    public init() {}

    public var isEmpty: Bool { points.count < 2 }

    public mutating func start() { points = []; length = 0 }
    public mutating func add(t: Double, x: Double, y: Double, p: Double) {
        if let last = points.last, t <= last.t { return }
        points.append(Point(t: t, x: x, y: y, p: p))
    }
    /// Close the loop. Times start at 0; the loop is as long as the gesture.
    public mutating func finish() {
        guard let first = points.first else { return }
        points = points.map { Point(t: $0.t - first.t, x: $0.x, y: $0.y, p: $0.p) }
        length = points.last?.t ?? 0
    }

    /// Where the finger was, `t` seconds into the loop (it repeats).
    public func at(_ t: Double) -> Point? {
        guard points.count >= 2, length > 0 else { return points.first }
        let u = t.truncatingRemainder(dividingBy: length)
        let tt = u < 0 ? u + length : u
        var lo = 0, hi = points.count - 1
        while hi - lo > 1 { let m = (lo + hi) / 2; if points[m].t <= tt { lo = m } else { hi = m } }
        let a = points[lo], b = points[hi]
        let f = b.t > a.t ? (tt - a.t) / (b.t - a.t) : 0
        return Point(t: tt, x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f, p: a.p + (b.p - a.p) * f)
    }
}

/// Lets a write through at most once per `period`, keeping the newest
/// value for when the window opens: knob writes never back up on a slow link.
public struct RateGate: Sendable {
    public var period: Double
    private var lastSent: Double = -.infinity
    public init(period: Double) { self.period = period }
    public mutating func ready(now: Double) -> Bool {
        guard now - lastSent >= period else { return false }
        lastSent = now
        return true
    }
}
