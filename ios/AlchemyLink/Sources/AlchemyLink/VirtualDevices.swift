// On-screen stand-ins for the controllers on the hub, for the Simulator and
// the demo: a Launchpad Mini MK3, a Launch Control XL, a gamepad and a
// Daisy Patch. Each makes the MIDI bytes the real device sends and reads
// the bytes the app sends it (LEDs, mode changes, routed notes), so the app
// treats them exactly as it treats the real ones (ControllerHub, Learn,
// mappings, routing). They go by the real devices' names, so routes and
// mappings made against them carry over. The app wraps them as MIDI ports
// (VirtualDevices.swift in the app); the bytes are worked out here, with
// tests, because they don't need CoreMIDI.

import Foundation

/// The names the on-screen devices go by. The Launchpad's and the XL's are
/// the CoreMIDI device names of the real ones. The gamepad's and the Daisy
/// Patch's depend on the device and its firmware, so they can be changed to
/// whatever the real one shows in Controls.
public struct VirtualDeviceNames: Codable, Equatable, Sendable {
    public static let launchpad = "Launchpad Mini MK3"
    public static let launchControlXL = "Launch Control XL"
    /// What iPadOS calls a PS4-mode gamepad (unchecked for a Haute42).
    public var gamepad = "DUALSHOCK 4 Wireless Controller"
    public var daisyPatch = "Daisy Patch"
    public init() {}
}

// MARK: - Launchpad Mini MK3

/// The Mini MK3 in Programmer mode, laid out 9 x 9 as on the device: row 0
/// is the top row of buttons (and the logo at column 8), rows 1-8 the grid
/// with the side buttons in column 8.
public struct VirtualLaunchpad: Sendable {
    public static let size = 9
    /// What the app last lit, as the CTRL reply's lp_leds (MK3 palette indices).
    public private(set) var leds = [UInt8](repeating: 0, count: CtrlReply.lpCount)
    /// Put in Programmer mode by the app (false: Live mode, as unplugged).
    public private(set) var programmerMode = false
    private var parser = MIDIStreamParser()

    public init() {}

    /// The pad number Programmer mode uses: 10 * row + column, row 1 at the
    /// bottom; nil for the logo.
    public static func padNumber(row: Int, col: Int) -> Int? {
        guard (0..<size).contains(row), (0..<size).contains(col), !(row == 0 && col == 8) else { return nil }
        return 10 * (9 - row) + col + 1
    }

    /// What pressing (or letting go of) a cell sends: a note for the grid,
    /// a CC for the top row and the side column, on channel 1.
    public static func press(row: Int, col: Int, down: Bool, velocity: UInt8 = 127) -> [UInt8]? {
        guard let n = padNumber(row: row, col: col) else { return nil }
        if row == 0 || col == 8 { return [0xB0, UInt8(n), down ? 127 : 0] }
        return [0x90, UInt8(n), down ? max(1, velocity & 0x7F) : 0]
    }

    /// The cell's index in lp_leds.
    public static func ledIndex(row: Int, col: Int) -> Int? {
        guard (0..<size).contains(row), (0..<size).contains(col) else { return nil }
        if row == 0 { return col == 8 ? LaunchpadMK3.logo : LaunchpadMK3.top0 + col }
        if col == 8 { return LaunchpadMK3.side0 + row - 1 }
        return (row - 1) * 8 + col
    }

    /// The colour shown on a cell (a palette index).
    public func colour(row: Int, col: Int) -> UInt8 {
        Self.ledIndex(row: row, col: col).map { leds[$0] } ?? 0
    }

    /// Bytes from the app: Programmer or Live mode, and LEDs as notes or CCs
    /// (any channel: 1 lit, 2 flashing, 3 pulsing; all shown lit here).
    public mutating func receive(_ bytes: [UInt8]) {
        var msgs: [MIDIMessage] = []
        parser.feed(bytes) { msgs.append($0) }
        for m in msgs {
            switch m {
            case let .sysex(s):
                if s == LaunchpadMK3.programmerMode { programmerMode = true }
                if s == LaunchpadMK3.liveMode { programmerMode = false; clear() }
            case let .channel(st, d1, d2):
                let t = st & 0xF0
                guard t == 0x90 || t == 0x80 || t == 0xB0, let i = Self.index(pad: Int(d1)) else { continue }
                leds[i] = t == 0x80 ? 0 : d2 & 0x7F
            }
        }
    }

    public mutating func clear() { for i in leds.indices { leds[i] = 0 } }

    /// lp_leds index of a Programmer-mode pad number.
    static func index(pad: Int) -> Int? {
        if pad == 99 { return LaunchpadMK3.logo }
        let row = pad / 10, col = pad % 10
        guard (1...9).contains(row), (1...9).contains(col) else { return nil }
        if row == 9 { return col <= 8 ? LaunchpadMK3.top0 + col - 1 : nil }
        if col == 9 { return LaunchpadMK3.side0 + 8 - row }
        return (8 - row) * 8 + col - 1
    }
}

/// The MK3's 128 colours, approximately (the commonly published Novation
/// palette), as 0xRRGGBB.
public enum LaunchpadPalette {
    public static let rgb: [UInt32] = [
        0x000000, 0x1E1E1E, 0x7F7F7F, 0xFFFFFF, 0xFF4C4C, 0xFF0000, 0x590000, 0x190000,
        0xFFBD6C, 0xFF5400, 0x591D00, 0x271B00, 0xFFFF4C, 0xFFFF00, 0x595900, 0x191900,
        0x88FF4C, 0x54FF00, 0x1D5900, 0x142B00, 0x4CFF4C, 0x00FF00, 0x005900, 0x001900,
        0x4CFF5E, 0x00FF19, 0x00590D, 0x001902, 0x4CFF88, 0x00FF55, 0x00591D, 0x001F12,
        0x4CFFB7, 0x00FF99, 0x005935, 0x001912, 0x4CC3FF, 0x00A9FF, 0x004152, 0x001019,
        0x4C88FF, 0x0055FF, 0x001D59, 0x000819, 0x4C4CFF, 0x0000FF, 0x000059, 0x000019,
        0x874CFF, 0x5400FF, 0x190064, 0x0F0030, 0xFF4CFF, 0xFF00FF, 0x590059, 0x190019,
        0xFF4C87, 0xFF0054, 0x59001D, 0x220013, 0xFF1500, 0x993500, 0x795100, 0x436400,
        0x033900, 0x005735, 0x00547F, 0x0000FF, 0x00454F, 0x2500CC, 0x7F7F7F, 0x202020,
        0xFF0000, 0xBDFF2D, 0xAFED06, 0x64FF09, 0x108B00, 0x00FF87, 0x00A9FF, 0x002AFF,
        0x3F00FF, 0x7A00FF, 0xB21A7D, 0x402100, 0xFF4A00, 0x88E106, 0x72FF15, 0x00FF00,
        0x3BFF26, 0x59FF71, 0x38FFCC, 0x5B8AFF, 0x3151C6, 0x877FE9, 0xD31DFF, 0xFF005D,
        0xFF7F00, 0xB9B000, 0x90FF00, 0x835D07, 0x392B00, 0x144C10, 0x0D5038, 0x15152A,
        0x16205A, 0x693C1C, 0xA8000A, 0xDE513D, 0xD86A1C, 0xFFE126, 0x9EE12F, 0x67B50F,
        0x1E1E30, 0xDCFF6B, 0x80FFBD, 0x9A99FF, 0x8E66FF, 0x404040, 0x757575, 0xE0FFFF,
        0xA00000, 0x350000, 0x1AD000, 0x074200, 0xB9B000, 0x3F3100, 0xB35F00, 0x4B1502,
    ]

    /// Red, green and blue 0...1 for a palette index.
    public static func components(_ index: UInt8) -> (r: Double, g: Double, b: Double) {
        let v = rgb[Int(index & 0x7F)]
        return (Double((v >> 16) & 0xFF) / 255, Double((v >> 8) & 0xFF) / 255, Double(v & 0xFF) / 255)
    }
}

// MARK: - Launch Control XL

/// The original Launch Control XL on factory template 1 (MIDI channel 9):
/// 24 knobs in three rows, 8 faders, 16 buttons in two rows.
public struct VirtualLaunchControlXL: Sendable {
    /// Factory template 1 sends on MIDI channel 9.
    public static let channel: UInt8 = 8
    static let knobBase: [UInt8] = [13, 29, 49]
    static let faderBase: UInt8 = 77
    static let buttonNotes: [[UInt8]] = [[41, 42, 43, 44, 57, 58, 59, 60], [73, 74, 75, 76, 89, 90, 91, 92]]

    /// The LEDs as the CTRL reply's xl_leds (xl::Col values): knobs 24, buttons 16.
    public private(set) var leds = [UInt8](repeating: LaunchControlXL.colour(red: 0, green: 0), count: CtrlReply.xlCount)
    public private(set) var knobs = [UInt8](repeating: 64, count: 24)
    public private(set) var faders = [UInt8](repeating: 0, count: 8)
    private var parser = MIDIStreamParser()

    public init() {}

    /// Turning a knob (row 0 top): its CC, nil if it didn't move.
    public mutating func turn(row: Int, col: Int, to v: UInt8) -> [UInt8]? {
        guard (0..<3).contains(row), (0..<8).contains(col) else { return nil }
        let v = min(v, 127)
        guard knobs[row * 8 + col] != v else { return nil }
        knobs[row * 8 + col] = v
        return [0xB0 | Self.channel, Self.knobBase[row] + UInt8(col), v]
    }

    public mutating func slide(col: Int, to v: UInt8) -> [UInt8]? {
        guard (0..<8).contains(col) else { return nil }
        let v = min(v, 127)
        guard faders[col] != v else { return nil }
        faders[col] = v
        return [0xB0 | Self.channel, Self.faderBase + UInt8(col), v]
    }

    /// A button (row 0 the upper row): a note on when pressed, off when let go.
    public static func press(row: Int, col: Int, down: Bool) -> [UInt8]? {
        guard (0..<2).contains(row), (0..<8).contains(col) else { return nil }
        let n = buttonNotes[row][col]
        return down ? [0x90 | channel, n, 127] : [0x80 | channel, n, 0]
    }

    public func knobLED(row: Int, col: Int) -> UInt8 { leds[row * 8 + col] }
    public func buttonLED(row: Int, col: Int) -> UInt8 { leds[24 + row * 8 + col] }

    /// Bytes from the app: LED SysEx (F0 00 20 29 02 11 78 tt idx val F7),
    /// template select, and CC 0 = 0 (all LEDs off), as the real XL takes them.
    public mutating func receive(_ bytes: [UInt8]) {
        var msgs: [MIDIMessage] = []
        parser.feed(bytes) { msgs.append($0) }
        for m in msgs {
            switch m {
            case let .sysex(s):
                if s.count == 11, Array(s[1...6]) == [0x00, 0x20, 0x29, 0x02, 0x11, 0x78], Int(s[8]) < leds.count {
                    leds[Int(s[8])] = s[9]
                }
            case let .channel(st, d1, d2):
                if st & 0xF0 == 0xB0, d1 == 0, d2 == 0 {
                    for i in leds.indices { leds[i] = LaunchControlXL.colour(red: 0, green: 0) }
                }
            }
        }
    }

    /// An xl::Col value as red and green brightness 0...1.
    public static func components(_ v: UInt8) -> (r: Double, g: Double) {
        (Double(v & 3) / 3, Double((v >> 4) & 3) / 3)
    }
}

// MARK: - Gamepad

/// The buttons held on the on-screen gamepad, as GameController would
/// report them (by XInput's positions).
public struct VirtualGamepad: Sendable {
    /// The buttons it has: d-pad, A B X Y, shoulders and triggers, Start and Back.
    public static let buttons: [XInputButton] = [.up, .down, .left, .right, .a, .b, .x, .y, .lb, .rb, .lt, .rt, .start, .back]
    public private(set) var held: Set<XInputButton> = []
    public init() {}

    /// True when it changed what's held.
    @discardableResult
    public mutating func set(_ b: XInputButton, down: Bool) -> Bool {
        down ? held.insert(b).inserted : held.remove(b) != nil
    }
}

// MARK: - Daisy Patch

/// A Daisy Patch with USB MIDI on: four knobs sending CCs, a gate button
/// sending a note, and a monitor of the last 50 messages it received.
public struct VirtualDaisyPatch: Sendable {
    public struct Line: Identifiable, Equatable, Sendable {
        public var id: Int
        public var text: String
    }

    public static let knobCCs: [UInt8] = [21, 22, 23, 24]
    public static let gateNote: UInt8 = 60
    public static let monitorLimit = 50
    public var channel: UInt8 = 0
    public private(set) var knobs = [UInt8](repeating: 0, count: 4)
    public private(set) var gate = false
    /// Newest last.
    public private(set) var monitor: [Line] = []
    private var count = 0
    private var parser = MIDIRouteParser()

    public init() {}

    public mutating func turn(_ i: Int, to v: UInt8) -> [UInt8]? {
        guard knobs.indices.contains(i) else { return nil }
        let v = min(v, 127)
        guard knobs[i] != v else { return nil }
        knobs[i] = v
        return [0xB0 | (channel & 0x0F), Self.knobCCs[i], v]
    }

    public mutating func setGate(_ down: Bool) -> [UInt8]? {
        guard down != gate else { return nil }
        gate = down
        return down ? [0x90 | (channel & 0x0F), Self.gateNote, 100] : [0x80 | (channel & 0x0F), Self.gateNote, 0]
    }

    /// Bytes from the app (routes and mappings), into the monitor.
    public mutating func receive(_ bytes: [UInt8]) {
        var msgs: [RawMIDIMessage] = []
        parser.feed(bytes) { msgs.append($0) }
        for m in msgs {
            count += 1
            monitor.append(Line(id: count, text: Self.describe(m)))
        }
        if monitor.count > Self.monitorLimit { monitor.removeFirst(monitor.count - Self.monitorLimit) }
    }

    public mutating func clearMonitor() { monitor = [] }

    /// "Note on C4 vel 100, ch 1" and the like.
    public static func describe(_ m: RawMIDIMessage) -> String {
        let b = m.bytes
        if m.isSysEx { return "SysEx, \(b.count) bytes" }
        if let ch = m.channel {
            let c = ", ch \(ch + 1)"
            let d1 = b.count > 1 ? Int(b[1]) : 0, d2 = b.count > 2 ? Int(b[2]) : 0
            switch m.status & 0xF0 {
            case 0x80: return "Note off \(BeltTheory.name(d1))\(c)"
            case 0x90: return d2 == 0 ? "Note off \(BeltTheory.name(d1))\(c)" : "Note on \(BeltTheory.name(d1)) vel \(d2)\(c)"
            case 0xA0: return "Poly pressure \(BeltTheory.name(d1)) \(d2)\(c)"
            case 0xB0: return "CC \(d1) = \(d2)\(c)"
            case 0xC0: return "Program \(d1 + 1)\(c)"
            case 0xD0: return "Pressure \(d1)\(c)"
            default: return "Pitch bend \((d1 | (d2 << 7)) - 8192)\(c)"
            }
        }
        switch m.status {
        case 0xF8: return "Clock"
        case 0xFA: return "Start"
        case 0xFB: return "Continue"
        case 0xFC: return "Stop"
        case 0xF2: return "Song position"
        default: return b.map { String(format: "%02X", $0) }.joined(separator: " ")
        }
    }
}
