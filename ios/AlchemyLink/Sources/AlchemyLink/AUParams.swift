// The AUv3 extension's parameters (ios/AlchemyAU): two Belt modules' twelve
// performance knobs and their HARD and Mute buttons, at fixed addresses a
// host can store in a session and automate. Pure Swift (no AudioToolbox),
// so the scheme is tested on Linux; the extension maps each Spec onto an
// AUParameter.
//
// Addresses (never renumber: hosts save automation against them):
//
//   address = slot << 8 | param
//   slot  0 = Module A, 1 = Module B (by unit number, lowest first: the
//             same order as the app's Left and Right)
//   param 0 Key       1 Scale    2 Retune   3 Amount   4 Harmony  5 Formant
//         6 Voice 1   7 Voice 2  8 Voice 3  9 Voice 4  10 Doubler 11 Spread
//         12 HARD     13 Mute
//
//   Module A: 0x000 ... 0x00D      Module B: 0x100 ... 0x10D
//
// Params 0-11 are Belt's descriptor fields p0.0 ... p0.5 and p1.0 ... p1.5
// (Play and Setup pages); 12 and 13 are panel buttons 0 and 1, pressed
// over HostLink (PRESS), not stored in the running state.
//
// Values the host sees: Key, Scale and the Voices as indexed parameters
// (0 ... n-1, named from BeltTheory); Retune, Amount, Harmony, Doubler and
// Spread as 0 ... 100 %; Formant as -100 ... +100; HARD and Mute as 0 or 1.
// The module stores every knob as 0 ... 1; an indexed value i is stored as
// the middle of its zone, (i + 0.5) / n, as the app and the web page write.
//
// MIDI CCs into the extension (any channel):
//   CC 20 ... 33 move Module A's params 0 ... 13
//   CC 52 ... 65 move Module B's params 0 ... 13
// A CC's 0 ... 127 sweeps the whole range; for an indexed knob it picks
// one of n equal zones; for HARD and Mute, 64 and up is on.

import Foundation

public enum AUParams {
    public enum Kind: Equatable, Sendable {
        /// 0 ... labels.count - 1, shown by name.
        case indexed([String])
        /// 0 ... 100 %.
        case percent
        /// -100 ... +100.
        case bipolar
        /// 0 or 1; pressed on the module as a button.
        case toggle(button: Int)
    }

    public struct Spec: Sendable {
        /// 0 ... 13, the low byte of the address.
        public var param: Int
        public var name: String
        /// For identifiers: "key", "voice1", "hard" ...
        public var key: String
        /// The descriptor field (nil for HARD and Mute).
        public var fieldId: String?
        public var kind: Kind

        public var minValue: Float {
            if case .bipolar = kind { return -100 }
            return 0
        }
        public var maxValue: Float {
            switch kind {
            case .indexed(let l): return Float(max(0, l.count - 1))
            case .percent, .bipolar: return 100
            case .toggle: return 1
            }
        }
        /// Zones for an indexed knob, 0 for continuous, -1 for a toggle:
        /// the plain numbers the extension's render thread works from.
        public var steps: Int {
            switch kind {
            case .indexed(let l): return l.count
            case .toggle: return -1
            default: return 0
            }
        }
        public var valueStrings: [String]? {
            switch kind {
            case .indexed(let l): return l
            case .toggle: return ["Off", "On"]
            default: return nil
            }
        }
        public var button: Int? { if case .toggle(let b) = kind { return b }; return nil }

        /// Before a module has answered: C, Major, knobs at half, voices off.
        public var defaultValue: Float {
            switch param {
            case 1: return 1
            case 0, 6, 7, 8, 9, 12, 13: return 0
            case 5: return 0
            default: return 50
            }
        }
    }

    public static let slots = 2
    public static let perSlot = 14
    public static let count = slots * perSlot
    public static let slotNames = ["Module A", "Module B"]
    public static let slotKeys = ["moduleA", "moduleB"]

    /// Belt's twelve performance knobs, then the two buttons.
    public static let specs: [Spec] = {
        let names = ["Key", "Scale", "Retune", "Amount", "Harmony", "Formant",
                     "Voice 1", "Voice 2", "Voice 3", "Voice 4", "Doubler", "Spread", "HARD", "Mute"]
        let keys = ["key", "scale", "retune", "amount", "harmony", "formant",
                    "voice1", "voice2", "voice3", "voice4", "doubler", "spread", "hard", "mute"]
        return (0..<perSlot).map { p in
            let kind: Kind
            switch p {
            case 0: kind = .indexed(BeltTheory.keys)
            case 1: kind = .indexed(BeltTheory.scales)
            case 5: kind = .bipolar
            case 6...9: kind = .indexed(BeltTheory.intervals)
            case 12: kind = .toggle(button: 0)
            case 13: kind = .toggle(button: 1)
            default: kind = .percent
            }
            return Spec(param: p, name: names[p], key: keys[p],
                        fieldId: p < 12 ? "p\(p / 6).\(p % 6)" : nil, kind: kind)
        }
    }()

    // MARK: addresses

    public static func address(slot: Int, param: Int) -> UInt64 { UInt64(slot) << 8 | UInt64(param) }

    /// nil for an address outside the scheme.
    public static func decode(_ address: UInt64) -> (slot: Int, param: Int)? {
        let slot = Int(address >> 8), param = Int(address & 0xFF)
        guard slot < slots, param < perSlot else { return nil }
        return (slot, param)
    }

    /// Dense 0 ... count-1, for arrays indexed by parameter.
    public static func index(of address: UInt64) -> Int? {
        decode(address).map { $0.slot * perSlot + $0.param }
    }

    public static func address(index: Int) -> UInt64 { address(slot: index / perSlot, param: index % perSlot) }

    public static var allAddresses: [UInt64] { (0..<count).map { address(index: $0) } }

    public static func spec(_ address: UInt64) -> Spec? { decode(address).map { specs[$0.param] } }

    /// "moduleA_key": stable, for hosts that key on identifiers.
    public static func identifier(_ address: UInt64) -> String? {
        decode(address).map { slotKeys[$0.slot] + "_" + specs[$0.param].key }
    }

    /// "A Key": hosts that flatten groups still tell the two modules apart.
    public static func displayName(_ address: UInt64) -> String? {
        decode(address).map { ($0.slot == 0 ? "A " : "B ") + specs[$0.param].name }
    }

    // MARK: values

    /// A knob's stored 0 ... 1 as the host's value.
    public static func auValue(fromModule norm: Double, param: Int) -> Float {
        let s = specs[param]
        let c = min(1, max(0, norm))
        switch s.kind {
        case .indexed(let l): return Float(max(0, min(l.count - 1, Int(c * Double(l.count)))))
        case .percent: return Float(c * 100)
        case .bipolar: return Float(c * 200 - 100)
        case .toggle: return c >= 0.5 ? 1 : 0
        }
    }

    /// The host's value as the knob's stored 0 ... 1 (a toggle: 0 or 1).
    public static func moduleValue(fromAU v: Float, param: Int) -> Double {
        let s = specs[param]
        let x = Double(min(s.maxValue, max(s.minValue, v)))
        switch s.kind {
        case .indexed(let l):
            let i = max(0, min(l.count - 1, Int(x.rounded())))
            return (Double(i) + 0.5) / Double(l.count)
        case .percent: return x / 100
        case .bipolar: return (x + 100) / 200
        case .toggle: return x >= 0.5 ? 1 : 0
        }
    }

    /// A CC value (0 ... 127) as the host's value. Plain arithmetic on
    /// `steps`, `minValue` and `maxValue`, the same the render thread does.
    public static func auValue(fromCC cc: UInt8, param: Int) -> Float {
        let s = specs[param]
        return auValue(fromCC: cc, steps: s.steps, min: s.minValue, max: s.maxValue)
    }

    public static func auValue(fromCC cc: UInt8, steps: Int, min lo: Float, max hi: Float) -> Float {
        let v = Int(cc & 0x7F)
        if steps < 0 { return v >= 64 ? 1 : 0 }
        if steps > 0 { return Float(Swift.min(steps - 1, v * steps / 128)) }
        return lo + (hi - lo) * Float(v) / 127
    }

    /// What a host shows for a value.
    public static func valueString(_ address: UInt64, _ v: Float) -> String {
        guard let s = spec(address) else { return String(v) }
        switch s.kind {
        case .indexed(let l): return l[max(0, min(l.count - 1, Int(v.rounded())))]
        case .percent: return "\(Int(v.rounded())) %"
        case .bipolar:
            let i = Int(v.rounded())
            return i > 0 ? "+\(i)" : String(i)
        case .toggle: return v >= 0.5 ? "On" : "Off"
        }
    }

    // MARK: MIDI CC

    public static let ccBase = [20, 52]

    public static func address(forCC cc: UInt8) -> UInt64? {
        let c = Int(cc)
        for (slot, base) in ccBase.enumerated() where c >= base && c < base + perSlot {
            return address(slot: slot, param: c - base)
        }
        return nil
    }

    public static func cc(for address: UInt64) -> UInt8? {
        decode(address).map { UInt8(ccBase[$0.slot] + $0.param) }
    }

    /// CC number to dense index, -1 for an unmapped CC: a flat table for the
    /// render thread, which must not search or allocate.
    public static func ccIndexTable() -> [Int] {
        (0..<128).map { cc in address(forCC: UInt8(cc)).flatMap { index(of: $0) } ?? -1 }
    }

    // MARK: modules

    /// Which unit is Module A and which Module B: lowest unit number first,
    /// as the app sorts Left and Right. A unit of 0 (an older firmware that
    /// reports none) sorts first. Extra modules beyond two get no slot.
    public static func assignSlots(_ units: [UInt32]) -> [UInt32] {
        Array(units.sorted().prefix(slots))
    }
}

/// Who wins when the host and the module both move a parameter. The host's
/// change (automation, a CC, the extension's own slider) goes out at once;
/// a value read back from the module (its panel turned by hand, the app, a
/// preset load) is shown only for parameters the host hasn't changed in the
/// last `hold` seconds, so a read that left before the write never drags an
/// automated lane back.
public struct AUParamSync: Sendable {
    public var hold: Double
    private var touched: [UInt64: Double] = [:]

    public init(hold: Double = 1.0) { self.hold = hold }

    public mutating func hostChanged(_ address: UInt64, at now: Double) { touched[address] = now }

    public func hostOwns(_ address: UInt64, at now: Double) -> Bool {
        guard let t = touched[address] else { return false }
        return now - t < hold
    }

    /// Of the values read from a module, those to apply to the parameters:
    /// not owned by the host, and different from what the parameter holds.
    public func readback(_ read: [UInt64: Float], current: (UInt64) -> Float?, at now: Double,
                         epsilon: Float = 1e-4) -> [UInt64: Float] {
        var out: [UInt64: Float] = [:]
        for (a, v) in read where !hostOwns(a, at: now) {
            if let c = current(a), Swift.abs(c - v) <= epsilon { continue }
            out[a] = v
        }
        return out
    }

    /// Forget touches older than the hold, so the table stays small.
    public mutating func prune(at now: Double) { touched = touched.filter { now - $0.value < hold } }
}
