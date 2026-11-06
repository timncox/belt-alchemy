// Two modules as one: the app sits between them and copies knob moves
// across, at control rate. Three kinds of link, each switched on in the app:
//
//   - Shared key and scale: turn Key or Scale on either, the other follows.
//   - Follows: one knob drives another on the other module, over a range
//     (Left's Retune also sets Right's Retune; Left's Harmony sets Right's
//     Doubler from 20 to 80 %). One way, so nothing echoes.
//   - Split harmonies: four voices planned in the app, two sung by each
//     module (Left takes voices 1-2, Right 3-4, each in its own Voice 1-2
//     slots). With the same voice cabled into both, that's four harmonies
//     over two outputs. Key and scale are shared while it's on.
//
// Anything timing-tight (clock, gates, audio) stays on patch cables: the
// round trip through the phone is a few ms over USB and 25 ms or more over
// Bluetooth, and it wobbles. Kept free of UIKit so it is tested on Linux.

import Foundation

public enum BeltFields {
    public static let key = "p0.0", scale = "p0.1"
    public static let voices = ["p1.0", "p1.1", "p1.2", "p1.3"]
    /// "Off" as a stored value: the middle of the first of 12 zones.
    public static let voiceOff = 0.5 / 12
    public static func voiceValue(_ interval: Int) -> Double { (Double(max(0, min(11, interval))) + 0.5) / 12 }
    public static func voiceIndex(_ v: Double) -> Int { max(0, min(11, Int(v * 12))) }
}

public struct Follow: Identifiable, Hashable, Codable, Sendable {
    public var id = UUID()
    public var from: ControlTarget
    public var to: ControlTarget
    public var lo: Double = 0, hi: Double = 1
    public init(from: ControlTarget, to: ControlTarget, lo: Double = 0, hi: Double = 1) {
        self.from = from; self.to = to; self.lo = lo; self.hi = hi
    }
}

public struct LinkSettings: Codable, Sendable, Equatable {
    public var shareKey = false
    public var follows: [Follow] = []
    public var splitHarmonies = false
    /// The four planned harmony voices (interval index 0...11, 0 = off).
    public var plan: [Int] = [7, 9, 0, 0]
    public init() {}

    public var keyShared: Bool { shareKey || splitHarmonies }
}

public enum LinkEngine {
    /// What else to set when `field` on module `unit` changed to `value`.
    /// `units` are the connected modules, left first.
    public static func propagate(unit: UInt32, field: String, value: Double,
                                 settings s: LinkSettings, units: [UInt32]) -> [ControlTarget: Double] {
        var out: [ControlTarget: Double] = [:]
        if s.keyShared, field == BeltFields.key || field == BeltFields.scale {
            for u in units where u != unit { out[ControlTarget(unit: u, field: field)] = value }
        }
        for f in s.follows where f.from.unit == unit && f.from.field == field && f.to.unit != unit {
            guard units.contains(f.to.unit) else { continue }
            let c = min(1, max(0, value))
            out[f.to] = min(1, max(0, f.lo + (f.hi - f.lo) * c))
        }
        // While split, the voice knobs belong to the plan: a module's own
        // Voice pot moves the planned voice it is singing.
        return out
    }

    /// While split, which planned voice a module's Voice pot stands for.
    public static func planSlot(unit: UInt32, field: String, units: [UInt32]) -> Int? {
        guard units.count >= 2, let side = units.firstIndex(of: unit), side < 2,
              let v = BeltFields.voices.firstIndex(of: field), v < 2 else { return nil }
        return side * 2 + v
    }

    /// The voice knobs that sing the plan: Left voices 1-2, Right 3-4.
    public static func splitWrites(plan: [Int], units: [UInt32]) -> [ControlTarget: Double] {
        guard units.count >= 2, plan.count == 4 else { return [:] }
        var out: [ControlTarget: Double] = [:]
        for side in 0..<2 {
            let u = units[side]
            out[ControlTarget(unit: u, field: BeltFields.voices[0])] = BeltFields.voiceValue(plan[side * 2])
            out[ControlTarget(unit: u, field: BeltFields.voices[1])] = BeltFields.voiceValue(plan[side * 2 + 1])
            out[ControlTarget(unit: u, field: BeltFields.voices[2])] = BeltFields.voiceOff
            out[ControlTarget(unit: u, field: BeltFields.voices[3])] = BeltFields.voiceOff
        }
        return out
    }

    /// The plan from the left module's four voices, when split is switched on.
    public static func plan(fromLeftVoices v: [Double]) -> [Int] {
        v.count == 4 ? v.map(BeltFields.voiceIndex) : [7, 9, 0, 0]
    }

    /// A follow chain that leads back to where it started would bounce
    /// forever; refuse it.
    public static func makesLoop(_ new: Follow, with existing: [Follow]) -> Bool {
        var seen: Set<ControlTarget> = [new.from]
        var frontier = [new.to]
        while let t = frontier.popLast() {
            if seen.contains(t) { return true }
            seen.insert(t)
            frontier += existing.filter { $0.from == t }.map(\.to)
        }
        return false
    }
}
