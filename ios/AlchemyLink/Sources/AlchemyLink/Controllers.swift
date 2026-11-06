// Controllers on the phone's USB-C hub, next to the modules: a Novation
// Launchpad Mini MK3, a Launch Control XL and a gamepad (a Haute42 on
// GP2040-CE). The app reads them and forwards their events to a module with
// HostLink 0x77 CTRL (firmware/CONTROLLERS.md), so the module runs the
// behaviour it has in its own Launchpad USB mode, and shows the LED colours
// the module sends back on the controllers. The bytes to and from the
// Novation devices are copied from the firmware's own driver
// (belt-alchemy src/launchpad.h and launchpad.cpp). No CoreMIDI and no
// GameController here, so all of it is tested on Linux.

import Foundation

// MARK: - 0x77 CTRL

/// Which controllers a CTRL request says are plugged in.
public struct CtrlPresent: OptionSet, Hashable, Sendable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }
    public static let launchpad = CtrlPresent(rawValue: 1 << 0)
    public static let launchControlXL = CtrlPresent(rawValue: 1 << 1)
    public static let gamepad = CtrlPresent(rawValue: 1 << 2)
}

/// A Launchpad pad's group, as the firmware's lp::Kind.
public enum LPKind: UInt8, Codable, Sendable, CaseIterable {
    case grid = 0, side = 1, top = 2
}

/// One controller event in the firmware's coordinates (lp::Event, xl::, pad::).
public enum CtrlEvent: Hashable, Sendable {
    /// Grid: x left to right, y top to bottom. Side: x 0, y top to bottom.
    /// Top: x left to right, y 0.
    case launchpad(LPKind, x: UInt8, y: UInt8, down: Bool)
    case xlKnob(row: UInt8, col: UInt8, value: UInt8)
    case xlFader(col: UInt8, value: UInt8)
    /// Row 0 is the upper row of buttons under the faders.
    case xlButton(row: UInt8, col: UInt8, down: Bool)
    /// The whole held mask: bits 0-15 XInput wButtons, 16 LT, 17 RT.
    case gamepad(UInt32)

    /// The 4 bytes of the event in a CTRL request.
    public var bytes: [UInt8] {
        switch self {
        case let .launchpad(k, x, y, down): return [1, k.rawValue, x, (y & 0x7F) | (down ? 0x80 : 0)]
        case let .xlKnob(r, c, v): return [2, r, c, v]
        case let .xlFader(c, v): return [3, c, v, 0]
        case let .xlButton(r, c, down): return [4, r, c, down ? 1 : 0]
        case let .gamepad(m): return [5, UInt8(m & 0xFF), UInt8((m >> 8) & 0xFF), UInt8((m >> 16) & 0xFF)]
        }
    }

    /// The other way, as the module reads it (for the pretend module).
    public init?(bytes b: [UInt8]) {
        guard b.count == 4 else { return nil }
        switch b[0] {
        case 1:
            guard let k = LPKind(rawValue: b[1]) else { return nil }
            self = .launchpad(k, x: b[2], y: b[3] & 0x7F, down: b[3] & 0x80 != 0)
        case 2: self = .xlKnob(row: b[1], col: b[2], value: b[3])
        case 3: self = .xlFader(col: b[1], value: b[2])
        case 4: self = .xlButton(row: b[1], col: b[2], down: b[3] != 0)
        case 5: self = .gamepad(UInt32(b[1]) | UInt32(b[2]) << 8 | UInt32(b[3]) << 16)
        default: return nil
        }
    }

    /// Which controller it came from.
    public var source: CtrlPresent {
        switch self {
        case .launchpad: return .launchpad
        case .xlKnob, .xlFader, .xlButton: return .launchControlXL
        case .gamepad: return .gamepad
        }
    }

    /// The one control it moved, for the app's own mappings (nil for the
    /// gamepad's mask, which the hub splits into buttons itself).
    public var element: ControlElement? {
        switch self {
        case let .launchpad(k, x, y, _): return .lpPad(k, x: x, y: y)
        case let .xlKnob(r, c, _): return .xlKnob(row: r, col: c)
        case let .xlFader(c, _): return .xlFader(col: c)
        case let .xlButton(r, c, _): return .xlButton(row: r, col: c)
        case .gamepad: return nil
        }
    }

    /// 0...1: a knob or fader's position, or 1 for down and 0 for up.
    public var value: Double {
        switch self {
        case let .launchpad(_, _, _, down), let .xlButton(_, _, down): return down ? 1 : 0
        case let .xlKnob(_, _, v), let .xlFader(_, v): return Double(v) / 127
        case .gamepad: return 0
        }
    }
}

public enum CtrlRequest {
    /// u8 present, u8 n, { u8 type, u8 a, u8 b, u8 c } x n
    public static func body(present: CtrlPresent, events: [CtrlEvent]) -> [UInt8] {
        let n = min(events.count, 255)
        var out: [UInt8] = [present.rawValue, UInt8(n)]
        out.reserveCapacity(2 + 4 * n)
        for e in events.prefix(n) { out += e.bytes }
        return out
    }

    /// The most events one request carries: whatever fits the module's
    /// largest body, and never more than 64 (the rest go in the next one).
    public static func maxEvents(maxBody: Int) -> Int { max(1, min(64, (maxBody - 2) / 4)) }
}

/// A CTRL reply: the module's LEDs for the controllers, always the whole frame.
public struct CtrlReply: Equatable, Sendable {
    public static let lpCount = 81, xlCount = 40
    public var version: UInt8
    public var presentSeen: CtrlPresent
    /// Grid 64 (row-major, y from the top), side 8 (top down), top 8, logo 1.
    public var lpLeds: [UInt8]
    /// Knobs 24 (row-major, rows top down), buttons 16 (upper row, then lower).
    public var xlLeds: [UInt8]

    public init(version: UInt8 = 1, presentSeen: CtrlPresent, lpLeds: [UInt8], xlLeds: [UInt8]) {
        self.version = version; self.presentSeen = presentSeen; self.lpLeds = lpLeds; self.xlLeds = xlLeds
    }

    /// From the reply body past the status byte. Later versions may add
    /// bytes after the frames; they're ignored.
    public init(reading r0: Reader) throws {
        var r = r0
        version = try r.u8()
        presentSeen = CtrlPresent(rawValue: try r.u8())
        lpLeds = try r.bytes(Self.lpCount)
        xlLeds = try r.bytes(Self.xlCount)
    }

    public var body: [UInt8] { [version, presentSeen.rawValue] + lpLeds + xlLeds }
}

extension HostLinkClient {
    /// 0x77 CTRL: events in, the module's LEDs out. nil when this firmware
    /// has no controller support in Phone mode (UNSUPPORTED). Not retried:
    /// an event must not arrive twice. A request with no events is a poll.
    public func ctrl(present: CtrlPresent, events: [CtrlEvent], timeout: Double = 0.5) async throws -> CtrlReply? {
        let (st, r) = try await request(.ctrl, CtrlRequest.body(present: present, events: events),
                                        timeout: timeout, allow: [.unsupported])
        if st == .unsupported { return nil }
        return try CtrlReply(reading: r)
    }
}

/// The events waiting for one module, and when the next CTRL is due: at
/// most one per control period while events wait, otherwise a poll for the
/// LEDs once per state period. One request in flight at a time. Used from
/// one thread (the app's main actor); the request itself goes through
/// `begin` and `end` so this never runs on another executor.
public final class CtrlSession: @unchecked Sendable {
    public private(set) var queue: [CtrlEvent] = []
    public private(set) var inFlight = false
    private var lastSent = -Double.infinity
    private var lastMask: UInt32?
    /// Events beyond this are dropped (never blocks), as on the module.
    public static let capacity = 256

    public init() {}

    /// Queue an event. A knob or fader still waiting is moved to its newest
    /// value (it's a position, not a press); a gamepad mask equal to the one
    /// before it is dropped; presses and releases are all kept, in order.
    public func enqueue(_ e: CtrlEvent) {
        switch e {
        case let .xlKnob(r, c, _):
            if let i = queue.firstIndex(where: { if case let .xlKnob(r2, c2, _) = $0 { return r2 == r && c2 == c }; return false }) {
                queue[i] = e; return
            }
        case let .xlFader(c, _):
            if let i = queue.firstIndex(where: { if case let .xlFader(c2, _) = $0 { return c2 == c }; return false }) {
                queue[i] = e; return
            }
        case let .gamepad(m):
            if lastMask == m { return }
            lastMask = m
        default: break
        }
        guard queue.count < Self.capacity else { return }
        queue.append(e)
    }

    /// Forget the gamepad mask last queued (a gamepad left or was re-routed).
    public func resetMask() { lastMask = nil }

    public func due(now: Double, controlPeriod: Double, statePeriod: Double) -> Bool {
        guard !inFlight else { return false }
        let since = now - lastSent
        return queue.isEmpty ? since >= statePeriod : since >= controlPeriod
    }

    /// Start a request: the events it carries, taken off the queue.
    public func begin(now: Double, maxEvents: Int) -> [CtrlEvent] {
        inFlight = true
        lastSent = now
        let n = min(queue.count, max(0, maxEvents))
        let batch = Array(queue.prefix(n))
        queue.removeFirst(n)
        return batch
    }

    /// The request finished. If it failed, the module may have missed the
    /// gamepad's state, so the newest mask goes again first; presses are not
    /// resent (one that did arrive would count twice).
    public func end(failed batch: [CtrlEvent]? = nil) {
        inFlight = false
        guard let batch, batch.contains(where: { $0.source == .gamepad }), let m = lastMask else { return }
        queue.removeAll { if case .gamepad = $0 { return true }; return false }
        queue.insert(.gamepad(m), at: 0)
    }
}

// MARK: - MIDI bytes

/// A MIDI message from a controller.
public enum MIDIMessage: Equatable, Sendable {
    /// Note off/on, poly pressure, CC, pitch bend: status (with channel) and two data bytes;
    /// program change and channel pressure with d2 = 0.
    case channel(status: UInt8, d1: UInt8, d2: UInt8)
    /// A whole SysEx message, F0 ... F7.
    case sysex([UInt8])

    public var type: UInt8 { if case let .channel(s, _, _) = self { return s & 0xF0 }; return 0xF0 }
    public var channel: UInt8 { if case let .channel(s, _, _) = self { return s & 0x0F }; return 0 }
}

/// Splits a MIDI byte stream (CoreMIDI packets) into messages: running
/// status, real-time bytes in the middle of anything, SysEx across packets.
public struct MIDIStreamParser: Sendable {
    private var status: UInt8 = 0
    private var data: [UInt8] = []
    private var sysex: [UInt8]?
    public init() {}

    public mutating func feed<C: Sequence>(_ bytes: C, _ out: (MIDIMessage) -> Void) where C.Element == UInt8 {
        for b in bytes { feed(b, out) }
    }

    public mutating func feed(_ b: UInt8, _ out: (MIDIMessage) -> Void) {
        if b >= 0xF8 { return }                                  // clock and the like
        if b == 0xF0 { sysex = [b]; status = 0; data = []; return }
        if b == 0xF7 {
            if let s = sysex { out(.sysex(s + [b])) }
            sysex = nil; return
        }
        if b & 0x80 != 0 {
            sysex = nil
            data = []
            status = b < 0xF0 ? b : 0                              // system common clears running status
            return
        }
        if sysex != nil {
            if sysex!.count < 512 { sysex!.append(b) }
            return
        }
        guard status != 0 else { return }
        data.append(b)
        let need = (status & 0xF0 == 0xC0 || status & 0xF0 == 0xD0) ? 1 : 2
        if data.count == need {
            out(.channel(status: status, d1: data[0], d2: need == 2 ? data[1] : 0))
            data = []
        }
    }
}

/// The first of several ports to deliver an event is the one listened to
/// (the Launchpad sends each press on both its DAW and MIDI ports), as the
/// firmware does with the two USB cables.
public struct PortLatch: Sendable {
    public private(set) var port: Int64?
    public init() {}
    public mutating func accept(_ p: Int64) -> Bool {
        if port == nil { port = p }
        return port == p
    }
    public mutating func reset() { port = nil }
}

/// What has been sent to a controller's LEDs, so only changes go out.
public struct LEDDiffer: Sendable {
    public private(set) var sent: [UInt8?]
    public init(count: Int) { sent = Array(repeating: nil, count: count) }

    /// Nothing known (after connecting, or a mode change on the device).
    public mutating func forget() { for i in sent.indices { sent[i] = nil } }

    /// The LEDs in `want` that differ from what was sent, now counted as sent.
    public mutating func changes(_ want: [UInt8]) -> [(index: Int, value: UInt8)] {
        var out: [(index: Int, value: UInt8)] = []
        for i in 0..<min(want.count, sent.count) where sent[i] != want[i] {
            out.append((i, want[i]))
            sent[i] = want[i]
        }
        return out
    }
}

// MARK: - Launchpad Mini MK3

public enum LaunchpadMK3 {
    /// Programmer mode, sent to both ports (as the firmware does).
    public static let programmerMode: [UInt8] = [0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x0E, 0x01, 0xF7]
    /// Back to Live mode, when the app lets go of it.
    public static let liveMode: [UInt8] = [0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x0E, 0x00, 0xF7]
    public static let cells = CtrlReply.lpCount
    static let side0 = 64, top0 = 72, logo = 80

    /// A note or CC from the Launchpad in Programmer mode, as the firmware's
    /// mini_rx reads it: pad = 10 * row + col, row 1 at the bottom.
    public static func event(_ m: MIDIMessage) -> CtrlEvent? {
        guard case let .channel(st, d1, d2) = m else { return nil }
        let t = st & 0xF0
        guard t == 0x80 || t == 0x90 || t == 0xB0 else { return nil }
        let row = Int(d1) / 10, col = Int(d1) % 10
        guard (1...9).contains(row), (1...9).contains(col) else { return nil }
        let down = t != 0x80 && d2 > 0
        if row == 9 { return col <= 8 ? .launchpad(.top, x: UInt8(col - 1), y: 0, down: down) : nil }
        if col == 9 { return .launchpad(.side, x: 0, y: UInt8(8 - row), down: down) }
        return .launchpad(.grid, x: UInt8(col - 1), y: UInt8(8 - row), down: down)
    }

    /// One LED (an index into the reply's lp_leds) as the MIDI message
    /// that sets it: a note on for the grid, a CC for the rest, channel 1.
    public static func message(index i: Int, colour: UInt8) -> [UInt8] {
        let c = colour & 0x7F
        if i < side0 {
            let note: Int = 10 * (8 - i / 8) + i % 8 + 1
            return [0x90, UInt8(note), c]
        }
        if i < top0 {
            let cc: Int = 10 * (8 - (i - side0)) + 9
            return [0xB0, UInt8(cc), c]
        }
        if i < logo { return [0xB0, UInt8(91 + i - top0), c] }
        return [0xB0, 99, c]
    }

    /// The messages that bring the Launchpad's LEDs to `want`.
    public static func messages(_ want: [UInt8], diff: inout LEDDiffer) -> [[UInt8]] {
        diff.changes(want).map { message(index: $0.index, colour: $0.value) }
    }

    /// The index in lp_leds of a pad, for tests and the pretend module.
    public static func index(_ k: LPKind, x: Int, y: Int) -> Int {
        switch k {
        case .grid: return y * 8 + x
        case .side: return side0 + y
        case .top: return top0 + x
        }
    }
}

// MARK: - Launch Control XL

public enum LaunchControlXL {
    /// Factory template 1, as the firmware's driver selects it.
    public static let template: UInt8 = 0x08
    public static let selectTemplate: [UInt8] = [0xF0, 0x00, 0x20, 0x29, 0x02, 0x11, 0x77, template, 0xF7]
    public static let leds = CtrlReply.xlCount

    /// An xl::Col colour: red 0...3, green 0...3.
    public static func colour(red: UInt8, green: UInt8) -> UInt8 { ((green & 3) << 4) | (red & 3) | 0x0C }

    /// One LED (an index into xl_leds) as its SysEx: F0 00 20 29 02 11 78 tt idx val F7.
    public static func message(index i: Int, colour: UInt8) -> [UInt8] {
        [0xF0, 0x00, 0x20, 0x29, 0x02, 0x11, 0x78, template, UInt8(i), colour & 0x7F, 0xF7]
    }

    public static func messages(_ want: [UInt8], diff: inout LEDDiffer) -> [[UInt8]] {
        diff.changes(want).map { message(index: $0.index, colour: $0.value) }
    }

    /// "F0 00 20 29 02 11 77 tt F7": someone picked another template on the
    /// device, so template 1 is selected again (and every LED resent).
    public static func templateChanged(_ m: MIDIMessage) -> Bool {
        guard case let .sysex(s) = m else { return false }
        return s.count == 9 && s[5] == 0x11 && s[6] == 0x77 && s[7] != template
    }

    /// A CC or note from the XL, as the firmware's xl_rx reads it (any channel).
    public static func event(_ m: MIDIMessage) -> CtrlEvent? {
        guard case let .channel(st, d1, d2) = m else { return nil }
        switch st & 0xF0 {
        case 0xB0:
            if (13...20).contains(d1) { return .xlKnob(row: 0, col: d1 - 13, value: d2) }
            if (29...36).contains(d1) { return .xlKnob(row: 1, col: d1 - 29, value: d2) }
            if (49...56).contains(d1) { return .xlKnob(row: 2, col: d1 - 49, value: d2) }
            if (77...84).contains(d1) { return .xlFader(col: d1 - 77, value: d2) }
            return nil
        case 0x80, 0x90:
            let down = st & 0xF0 == 0x90 && d2 > 0
            if (41...44).contains(d1) { return .xlButton(row: 0, col: d1 - 41, down: down) }
            if (57...60).contains(d1) { return .xlButton(row: 0, col: 4 + d1 - 57, down: down) }
            if (73...76).contains(d1) { return .xlButton(row: 1, col: d1 - 73, down: down) }
            if (89...92).contains(d1) { return .xlButton(row: 1, col: 4 + d1 - 89, down: down) }
            return nil
        default: return nil
        }
    }
}

// MARK: - Gamepad (XInput's buttons)

/// A gamepad button by where XInput puts it. GameController's face buttons
/// are read by position (buttonA south, buttonB east, buttonX west, buttonY
/// north), which is where XInput's A, B, X and Y are.
public enum XInputButton: String, CaseIterable, Codable, Sendable {
    case up, down, left, right, start, back, l3, r3, lb, rb, guide, a, b, x, y, lt, rt

    /// The bit in pad::Buttons(): XInput wButtons, then LT 16 and RT 17.
    public var bit: UInt32 {
        switch self {
        case .up: return 0x0001
        case .down: return 0x0002
        case .left: return 0x0004
        case .right: return 0x0008
        case .start: return 0x0010
        case .back: return 0x0020
        case .l3: return 0x0040
        case .r3: return 0x0080
        case .lb: return 0x0100
        case .rb: return 0x0200
        case .guide: return 0x0400
        case .a: return 0x1000
        case .b: return 0x2000
        case .x: return 0x4000
        case .y: return 0x8000
        case .lt: return 1 << 16
        case .rt: return 1 << 17
        }
    }

    public var name: String {
        switch self {
        case .up: return "Up"
        case .down: return "Down"
        case .left: return "Left"
        case .right: return "Right"
        case .start: return "Start"
        case .back: return "Back"
        case .l3: return "L3"
        case .r3: return "R3"
        case .lb: return "LB"
        case .rb: return "RB"
        case .guide: return "Home"
        case .a: return "A (bottom)"
        case .b: return "B (right)"
        case .x: return "X (left)"
        case .y: return "Y (top)"
        case .lt: return "LT"
        case .rt: return "RT"
        }
    }
}

public enum XInput {
    public static func mask<S: Sequence>(_ held: S) -> UInt32 where S.Element == XInputButton {
        held.reduce(0) { $0 | $1.bit }
    }
    public static func buttons(_ mask: UInt32) -> Set<XInputButton> {
        Set(XInputButton.allCases.filter { mask & $0.bit != 0 })
    }
}

// MARK: - Which controller, and where it goes

public enum ControllerKind: String, Codable, Sendable {
    case launchpad, launchControlXL, gamepad, midi

    /// From a CoreMIDI name (port and device names together). nil for an
    /// Alchemy Lab, which is a module, not a controller. Only the Mini MK3
    /// and the original Launch Control XL speak the firmware's protocol;
    /// anything else (an XL 3, a Launchpad X, a keyboard) is plain MIDI.
    public static func classify(midiName: String) -> ControllerKind? {
        let n = midiName.lowercased()
        if n.contains("alchemy") { return nil }
        let squashed = n.filter { !$0.isWhitespace && $0 != "-" && $0 != "_" }
        if squashed.contains("lpminimk3") || (squashed.contains("launchpadmini") && squashed.contains("mk3")) {
            return .launchpad
        }
        let xl = (squashed.contains("launchcontrolxl") && !squashed.contains("launchcontrolxl3"))
            || (squashed.contains("lcxl") && !squashed.contains("lcxl3"))
        return xl ? .launchControlXL : .midi
    }

    /// Its bit in a CTRL request; nil for plain MIDI, which is never forwarded.
    public var present: CtrlPresent? {
        switch self {
        case .launchpad: return .launchpad
        case .launchControlXL: return .launchControlXL
        case .gamepad: return .gamepad
        case .midi: return nil
        }
    }

    public var forwards: Bool { present != nil }

    public var label: String {
        switch self {
        case .launchpad: return "Launchpad Mini MK3"
        case .launchControlXL: return "Launch Control XL"
        case .gamepad: return "Gamepad"
        case .midi: return "MIDI"
        }
    }
}

/// Where a controller's events go.
public enum ControllerRoute: String, Codable, Sendable, CaseIterable, Identifiable {
    case left, right, both, appOnly, off
    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .left: return "Left"
        case .right: return "Right"
        case .both: return "Both"
        case .appOnly: return "App only"
        case .off: return "Off"
        }
    }

    /// The modules it forwards to, from the connected ones (left first).
    /// With both, the LEDs shown are the first one's.
    public func units(_ ready: [UInt32]) -> [UInt32] {
        switch self {
        case .left: return Array(ready.prefix(1))
        case .right: return ready.count > 1 ? [ready[1]] : []
        case .both: return Array(ready.prefix(2))
        case .appOnly, .off: return []
        }
    }

    public static func choices(for kind: ControllerKind) -> [ControllerRoute] {
        kind.forwards ? allCases : [.appOnly, .off]
    }

    public static func defaultRoute(for kind: ControllerKind) -> ControllerRoute { kind.forwards ? .left : .appOnly }
}

/// The present bits for one module: every controller routed to it.
public func ctrlPresent(for unit: UInt32, controllers: [(kind: ControllerKind, route: ControllerRoute)],
                        ready: [UInt32]) -> CtrlPresent {
    var p: CtrlPresent = []
    for c in controllers {
        guard let bit = c.kind.present, c.route.units(ready).contains(unit) else { continue }
        p.insert(bit)
    }
    return p
}

/// The next or previous saved preset, wrapping round. `current` nil starts
/// from before the first (next) or after the last (previous).
public enum PresetStepper {
    public static func step(from current: Int?, valid: [Int], by step: Int) -> Int? {
        let v = valid.sorted()
        guard !v.isEmpty, step != 0 else { return nil }
        guard let cur = current else { return step > 0 ? v.first : v.last }
        if step > 0 { return v.first { $0 > cur } ?? v.first }
        return v.last { $0 < cur } ?? v.last
    }
}
