// The phone block's STATE reply (0x76, firmware/phone_ext.h): what the
// app's own screen shows beyond the knob values. A generic header (page,
// the pot a hand last moved, load, levels) and a firmware's own tail; Belt's
// tail carries the sung pitch, the correction target, the harmony voices,
// HARD and the mute, and a short pitch trace.

import Foundation

public struct ModuleState {
    public var frame: UInt16
    public var page: Int
    public var settingsOpen: Bool
    public var firmwareBusy: Bool
    /// The pot a hand last moved on the panel (pager page, pot 0…5).
    public var touchPage: Int?, touchPot: Int?
    /// How long ago, in seconds; nil when nothing was touched yet.
    public var touchAge: Double?
    /// Smoothed audio load, 0…1.
    public var cpu: Double
    /// Peaks since the last read, in dBFS (−60 or below reads −60).
    public var inLevel: Double, outLevel: Double
    public var tail: Tail

    public enum Tail {
        case belt(BeltState)
        case other(tag: UInt8, bytes: [UInt8])
        case none
    }

    public var belt: BeltState? { if case let .belt(b) = tail { return b }; return nil }

    static func db(_ b: UInt8) -> Double { Double(b) * 60 / 255 - 60 }

    public init(reading r0: Reader) throws {
        var r = r0
        frame = try r.u16()
        page = Int(try r.u8())
        let flags = try r.u8()
        settingsOpen = flags & 1 != 0
        firmwareBusy = flags & 2 != 0
        let tp = try r.u8(), tq = try r.u8(), age = try r.u16()
        touchPage = tq == 0xFF ? nil : Int(tp)
        touchPot = tq == 0xFF ? nil : Int(tq)
        touchAge = (tq == 0xFF || age == 0xFFFF) ? nil : Double(age) / 1000
        cpu = Double(try r.u8()) / 255
        inLevel = Self.db(try r.u8())
        outLevel = Self.db(try r.u8())
        if r.remaining < 2 { tail = .none; return }
        let tag = try r.u8(), ver = try r.u8()
        if tag == UInt8(ascii: "B") && ver == 1 {
            tail = .belt(try BeltState(reading: &r))
        } else {
            tail = .other(tag: tag, bytes: r.rest())
        }
    }
}

/// Belt's part of STATE (tag "B", version 1).
public struct BeltState {
    public var key: Int, scale: Int
    /// Interval index per harmony voice, 0…11 (0 = off, 6 = unison).
    public var voices: [Int]
    public var voiced: Bool, hard: Bool, muted: Bool
    public var hardLatched: Bool, muteLatched: Bool, gate: Bool
    /// The sung pitch as a fractional MIDI note (69.0 = A4); nil unvoiced.
    public var sung: Double?
    /// Cents from the correction target.
    public var cents: Int
    /// The trace: the number of its newest sample, and the samples (oldest
    /// first, one per control frame, ~60 a second; nil = unvoiced).
    public var traceLast: UInt16
    public var trace: [Double?]

    public static let unvoiced = Int16.min

    init(reading r: inout Reader) throws {
        key = Int(try r.u8()); scale = Int(try r.u8())
        voices = try (0..<4).map { _ in Int(try r.u8()) }
        let f = try r.u8()
        voiced = f & 1 != 0; hard = f & 2 != 0; muted = f & 4 != 0
        hardLatched = f & 8 != 0; muteLatched = f & 16 != 0; gate = f & 32 != 0
        let s = Int16(bitPattern: try r.u16())
        sung = s == Self.unvoiced ? nil : Double(s) / 100
        cents = Int(Int16(bitPattern: try r.u16()))
        traceLast = try r.u16()
        let n = Int(try r.u8())
        trace = try (0..<n).map { _ in
            let v = Int16(bitPattern: try r.u16())
            return v == Self.unvoiced ? nil : Double(v) / 100
        }
    }

    /// The note the sung pitch is being pulled to.
    public var target: Int? {
        guard let s = sung else { return nil }
        return Int((s - Double(cents) / 100).rounded())
    }

    /// Each sounding harmony voice's note, from the target, as the engine
    /// places it.
    public var harmonyNotes: [Int?] {
        voices.map { itv in
            guard let t = target, itv != 0 else { return nil }
            return Int(BeltTheory.harmTarget(Double(t), interval: itv, key: key, scale: scale).rounded())
        }
    }
}

/// Belt's music, ported from the engine (vendor/belt_core.c) so the phone
/// names the same notes the module sings.
public enum BeltTheory {
    public static let keys = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    public static let scales = ["Chromatic", "Major", "Minor", "Harm Minor", "Dorian", "Mixolydian",
                                "Maj Pent", "Min Pent", "Blues"]
    public static let intervals = ["Off", "-Oct", "-6th", "-5th", "-4th", "-3rd",
                                   "Unison", "+3rd", "+4th", "+5th", "+6th", "+Oct"]
    static let scaleDeg: [[Int]] = [
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
        [0, 2, 4, 5, 7, 9, 11],
        [0, 2, 3, 5, 7, 8, 10],
        [0, 2, 3, 5, 7, 8, 11],
        [0, 2, 3, 5, 7, 9, 10],
        [0, 2, 4, 5, 7, 9, 10],
        [0, 2, 4, 7, 9],
        [0, 3, 5, 7, 10],
        [0, 3, 5, 6, 7, 10],
    ]
    static let itvSteps = [0, 0, -5, -4, -3, -2, 0, 2, 3, 4, 5, 0]
    static let itvChroma = [0, -12, -9, -7, -5, -4, 0, 4, 5, 7, 9, 12]
    static let itvIsOct = [0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]

    static func degrees(_ scale: Int) -> [Int] { scaleDeg[max(0, min(scaleDeg.count - 1, scale))] }

    /// "A4" for 69 (middle C is C4).
    public static func name(_ midi: Int) -> String {
        keys[((midi % 12) + 12) % 12] + String(Int((Double(midi) / 12).rounded(.down)) - 1)
    }

    public static func inScale(_ note: Int, key: Int, scale: Int) -> Bool {
        degrees(scale).contains(((note - key) % 12 + 12) % 12)
    }

    /// The engine's degree_shift: walk `steps` scale degrees from a note.
    static func degreeShift(_ note: Double, key: Int, scale: Int, steps: Int) -> Double {
        let deg = degrees(scale), n = deg.count
        let rn = Int(note.rounded())
        let rel = rn - key
        var oct = Int((Double(rel) / 12).rounded(.down))
        let pc = ((rel % 12) + 12) % 12
        var idx = 0, best = 99
        for i in 0..<n where Swift.abs(deg[i] - pc) < best { best = Swift.abs(deg[i] - pc); idx = i }
        var j = idx + steps
        while j < 0 { j += n; oct -= 1 }
        while j >= n { j -= n; oct += 1 }
        return Double(key + oct * 12 + deg[j]) + (note - Double(rn))
    }

    /// The engine's harm_target.
    public static func harmTarget(_ q: Double, interval itv: Int, key: Int, scale: Int) -> Double {
        guard itv > 0, itv < 12 else { return q }
        if itvIsOct[itv] != 0 { return q + 12 * Double(itvIsOct[itv]) }
        if itv == 6 { return q }
        if scale == 0 { return q + Double(itvChroma[itv]) }
        return degreeShift(q, key: key, scale: scale, steps: itvSteps[itv])
    }
}
