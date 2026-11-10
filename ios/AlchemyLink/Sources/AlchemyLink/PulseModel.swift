// Pulse (pulse_alchemy 0.4.0 and later): what its own screen on the phone
// needs to know that the descriptor doesn't say. The field ids, how an
// output's settings turn into the voltage it plays, and the euclidean
// pattern. The maths mirrors pulse_engine.cpp (Shape, EuclidHit), so the
// phone draws what the jack does.

import Foundation

public enum Pulse {
    public static let moduleId = "pulse_alchemy"
    public static let outs = 8
    public static let bpmMin = 10.0, bpmMax = 300.0
    public static let maxSteps = 32

    /// The per-output settings, in the firmware's id suffixes.
    public enum Key: String, CaseIterable {
        case ratio, wave, width, level, offset, phase      // the OUTPUT layer
        case steps, fills, rotate, chance, swing, slew     // the PATTERN layer
    }

    public static func id(_ out: Int, _ key: Key) -> String { "o\(out + 1)_\(key.rawValue)" }
    public static let tempoId = "bpm", selectedId = "sel"

    /// True for a descriptor this screen can drive: Pulse with tempo and
    /// every per-output setting as a field (0.4.0). Older Pulse keeps the
    /// plain LED screen.
    public static func drives(_ d: Descriptor?) -> Bool {
        guard let d, d.moduleId == moduleId, d.field(tempoId) != nil else { return false }
        return (0..<outs).allSatisfy { o in Key.allCases.allSatisfy { d.field(id(o, $0)) != nil } }
    }

    public static func bpm(fromKnob v: Double) -> Double { bpmMin + (bpmMax - bpmMin) * min(1, max(0, v)) }
    public static func knob(fromBpm b: Double) -> Double { (min(bpmMax, max(bpmMin, b)) - bpmMin) / (bpmMax - bpmMin) }

    /// Cycles per quarter-note beat from a ratio label: "/64" … "/1.5", "x1" … "x32".
    public static func cyclesPerBeat(_ label: String) -> Double? {
        guard let first = label.first, let n = Double(label.dropFirst()), n > 0 else { return nil }
        switch first {
        case "/": return 1 / n
        case "x", "X", "×": return n
        default: return nil
        }
    }

    public enum Wave: Int, CaseIterable {
        case gate = 0, trig, saw, ramp, tri, sine, env, stepRandom, smoothRandom

        public var usesWidth: Bool { self == .gate || self == .trig || self == .tri || self == .env }
        /// What WIDTH means for this wave, for the knob's label.
        public var widthName: String {
            switch self {
            case .gate: return "Gate length"
            case .trig: return "Trigger length"
            case .tri: return "Skew"
            case .env: return "Decay"
            default: return "Width"
            }
        }
    }

    /// One cycle's shape, 0…1, before level and offset (pulse_engine Shape).
    public static func shape(_ wave: Wave, phase p: Double, width: Double, cycleSeconds: Double,
                             held r0: Double = 0.35, next r1: Double = 0.8) -> Double {
        let w01 = min(1, max(0, width))
        switch wave {
        case .gate:
            let w = max(0.01, w01)
            return (w >= 1 || p < w) ? 1 : 0
        case .trig:
            var len = 0.005 + w01 * 0.045
            if len > 0.5 * cycleSeconds { len = 0.5 * cycleSeconds }
            return p * cycleSeconds < len ? 1 : 0
        case .saw: return p
        case .ramp: return 1 - p
        case .tri:
            let w = min(0.999, max(0.001, w01))
            return p < w ? p / w : (1 - p) / (1 - w)
        case .sine: return 0.5 - 0.5 * cos(2 * Double.pi * p)
        case .env: return exp(-p / (0.03 + w01 * 0.6))
        case .stepRandom: return r1
        case .smoothRandom: return r0 + (r1 - r0) * (0.5 - 0.5 * cos(Double.pi * p))
        }
    }

    /// Step `i` of an euclidean pattern is a hit (pulse_engine EuclidHit).
    public static func euclidHit(_ i: Int, steps: Int, fills: Int, rotate: Int) -> Bool {
        if steps <= 0 { return true }
        if fills <= 0 { return false }
        if fills >= steps { return true }
        var j = (i + rotate) % steps
        if j < 0 { j += steps }
        return (j * fills) % steps < fills
    }

    public static func pattern(steps: Int, fills: Int, rotate: Int) -> [Bool] {
        guard steps > 0 else { return [] }
        return (0..<steps).map { euclidHit($0, steps: steps, fills: min(fills, steps), rotate: rotate) }
    }

    /// One output's settings as the screen uses them.
    public struct Out: Equatable {
        public var ratio = 13, wave = Wave.gate
        public var width = 0.5, level = 1.0, offset = 0.5, phase = 0.0     // knob values, 0…1
        public var steps = 0, fills = 0, rotate = 0
        public var chance = 1.0, swing = 0.0, slew = 0.0                   // knob values, 0…1

        public init() {}

        /// From the module's values (field id -> value: a choice's index, or 0…1).
        public init(_ out: Int, values: [String: Double]) {
            func v(_ k: Key, _ d: Double) -> Double { values[Pulse.id(out, k)] ?? d }
            ratio = Int(v(.ratio, 13)); wave = Wave(rawValue: Int(v(.wave, 0))) ?? .gate
            width = v(.width, 0.5); level = v(.level, 1); offset = v(.offset, 0.5); phase = v(.phase, 0)
            steps = Int(v(.steps, 0)); fills = Int(v(.fills, 0)); rotate = Int(v(.rotate, 0))
            chance = v(.chance, 1); swing = v(.swing, 0); slew = v(.slew, 0)
        }

        public var offsetVolts: Double { offset * 10 - 5 }
        public var hits: [Bool] { Pulse.pattern(steps: steps, fills: fills, rotate: rotate) }

        /// The voltage through `cycles` cycles from a reset, `points` samples,
        /// clipped to the jack's ±5 V. Rests in the euclidean pattern sit at
        /// the offset; the random waves use fixed stand-in levels. Chance,
        /// swing and slew are left out: this is the shape, not a recording.
        public func curve(points: Int, cycles: Int, cycleSeconds: Double) -> [Double] {
            guard points > 1, cycles > 0 else { return [] }
            let stand: [Double] = [0.35, 0.8, 0.15, 0.6, 0.95, 0.45, 0.25, 0.7]
            return (0..<points).map { i in
                let c = Double(i) / Double(points - 1) * Double(cycles) * 0.9999 + phase
                let n = Int(c.rounded(.down)), p = c - Double(n)
                let sounding = steps == 0 || Pulse.euclidHit(n % max(1, steps), steps: steps, fills: fills, rotate: rotate)
                let s = sounding ? Pulse.shape(wave, phase: p, width: width, cycleSeconds: cycleSeconds,
                                               held: stand[n % stand.count], next: stand[(n + 1) % stand.count]) : 0
                return min(5, max(-5, s * level * 5 + offsetVolts))
            }
        }
    }

    /// A pattern someone might want: steps 3…16, a sensible number of hits.
    public static func randomPattern(using rng: inout some RandomNumberGenerator) -> (steps: Int, fills: Int, rotate: Int) {
        let steps = [3, 4, 5, 6, 7, 8, 8, 9, 12, 12, 13, 16, 16].randomElement(using: &rng) ?? 8
        let fills = Int.random(in: max(1, steps / 4)...max(1, steps * 3 / 4), using: &rng)
        return (steps, fills, Int.random(in: 0..<steps, using: &rng))
    }
}
