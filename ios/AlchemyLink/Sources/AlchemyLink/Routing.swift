// MIDI routing: the iPhone or iPad as the patchbay for everything on the
// USB-C hub. Any MIDI device (a Launchpad, a Launch Control XL, a keyboard,
// a Daisy Patch), a gamepad's buttons, or a module's own MIDI out (Belt's
// sung note and harmonies from STATE, its knobs as CCs) can go to any MIDI
// device, or to a module played as a keyboard (CTRL events 6 and 7, present
// bit 3). Each route has a filter, an input and output channel and a
// transpose. HostLink's SysEx is never routed. Notes still held when a
// route goes or a device is unplugged are let go, and a route that would
// lead back to where it started is refused, as linking's follows are.
// Kept free of CoreMIDI so it is tested on Linux.

import Foundation

// MARK: - What a route lets through

public struct RouteFilter: OptionSet, Hashable, Codable, Sendable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }
    public static let notes = RouteFilter(rawValue: 1 << 0)
    public static let cc = RouteFilter(rawValue: 1 << 1)
    public static let pitchBend = RouteFilter(rawValue: 1 << 2)
    /// Poly and channel pressure.
    public static let aftertouch = RouteFilter(rawValue: 1 << 3)
    public static let program = RouteFilter(rawValue: 1 << 4)
    /// Clock, start, continue, stop and song position.
    public static let clock = RouteFilter(rawValue: 1 << 5)
    public static let all: RouteFilter = [.notes, .cc, .pitchBend, .aftertouch, .program, .clock]

    /// Each kind with its name, in the order shown.
    public static let kinds: [(filter: RouteFilter, name: String)] = [
        (.notes, "Notes"), (.cc, "CC"), (.pitchBend, "Pitch bend"), (.aftertouch, "Aftertouch"),
        (.program, "Program change"), (.clock, "Clock and transport"),
    ]

    public var summary: String {
        if self == .all { return "Everything" }
        let names = Self.kinds.filter { contains($0.filter) }.map(\.name)
        return names.isEmpty ? "Nothing" : names.joined(separator: ", ")
    }
}

// MARK: - Messages

/// One whole MIDI message as it arrived: a channel message, a real-time or
/// system common byte group, or a SysEx (which is never routed).
public struct RawMIDIMessage: Hashable, Sendable {
    public var bytes: [UInt8]
    public init(_ bytes: [UInt8]) { self.bytes = bytes }

    public var status: UInt8 { bytes.first ?? 0 }
    public var isChannel: Bool { status >= 0x80 && status < 0xF0 }
    public var channel: UInt8? { isChannel ? status & 0x0F : nil }
    public var isSysEx: Bool { status == 0xF0 }

    /// The filter kind it falls under; nil for what is never routed (SysEx,
    /// active sensing, reset, MTC, song select, tune request).
    public var category: RouteFilter? {
        if isChannel {
            switch status & 0xF0 {
            case 0x80, 0x90: return .notes
            case 0xA0, 0xD0: return .aftertouch
            case 0xB0: return .cc
            case 0xC0: return .program
            default: return .pitchBend
            }
        }
        switch status {
        case 0xF8, 0xFA, 0xFB, 0xFC, 0xF2: return .clock
        default: return nil
        }
    }

    public var isNoteOn: Bool { status & 0xF0 == 0x90 && isChannel && bytes.count == 3 && bytes[2] > 0 }
    public var isNoteOff: Bool {
        guard isChannel, bytes.count == 3 else { return false }
        return status & 0xF0 == 0x80 || (status & 0xF0 == 0x90 && bytes[2] == 0)
    }

    /// As the controller code reads it (MIDIStreamParser's messages).
    public var message: MIDIMessage? {
        if isSysEx { return .sysex(bytes) }
        guard isChannel, bytes.count >= 2 else { return nil }
        return .channel(status: status, d1: bytes[1], d2: bytes.count > 2 ? bytes[2] : 0)
    }
}

/// Splits one source's byte stream (CoreMIDI packets) into whole messages:
/// running status, real-time bytes passed on at once even in the middle of
/// another message or a SysEx, SysEx gathered (up to 512 bytes) across packets.
public struct MIDIRouteParser: Sendable {
    private var status: UInt8 = 0
    private var data: [UInt8] = []
    private var sysex: [UInt8]?
    public static let sysexLimit = 512
    public init() {}

    public mutating func feed<C: Sequence>(_ bytes: C, _ out: (RawMIDIMessage) -> Void) where C.Element == UInt8 {
        for b in bytes { feed(b, out) }
    }

    private static func need(_ s: UInt8) -> Int {
        switch s {
        case 0xF1, 0xF3: return 1
        case 0xF2: return 2
        default: return (s & 0xF0 == 0xC0 || s & 0xF0 == 0xD0) ? 1 : 2
        }
    }

    public mutating func feed(_ b: UInt8, _ out: (RawMIDIMessage) -> Void) {
        if b >= 0xF8 { out(RawMIDIMessage([b])); return }          // real-time: anywhere
        if b == 0xF0 { sysex = [b]; status = 0; data = []; return }
        if b == 0xF7 {
            if let s = sysex { out(RawMIDIMessage(s + [b])) }
            sysex = nil
            return
        }
        if b & 0x80 != 0 {
            sysex = nil
            data = []
            if b < 0xF0 { status = b; return }
            // System common clears running status.
            switch b {
            case 0xF1, 0xF2, 0xF3: status = b
            case 0xF6: status = 0; out(RawMIDIMessage([b]))
            default: status = 0
            }
            return
        }
        if sysex != nil {
            if sysex!.count < Self.sysexLimit { sysex!.append(b) }
            return
        }
        guard status != 0 else { return }
        data.append(b)
        if data.count == Self.need(status) {
            out(RawMIDIMessage([status] + data))
            data = []
            if status >= 0xF0 { status = 0 }
        }
    }
}

// MARK: - Routes

/// Where a route starts or ends. Devices are kept by name (so a route
/// comes back when the device is plugged in again), modules by unit number.
public enum RouteEndpoint: Hashable, Codable, Sendable {
    /// Any MIDI device that isn't a module: a controller, a keyboard, a Daisy Patch.
    case device(String)
    /// As a source, the module's MIDI out (made by the app from what it reads);
    /// as a destination, the module played as a keyboard.
    case module(UInt32)
    /// A gamepad's buttons, as notes (a source only).
    case gamepad(String)

    public var canReceive: Bool { if case .gamepad = self { return false }; return true }
    public var unit: UInt32? { if case let .module(u) = self { return u }; return nil }
    public var isModule: Bool { unit != nil }
}

/// A module knob sent as a CC.
public struct KnobCC: Hashable, Codable, Sendable {
    public var field: String
    public var cc: UInt8
    public init(field: String, cc: UInt8) { self.field = field; self.cc = min(127, cc) }
}

public struct MIDIRoute: Identifiable, Hashable, Codable, Sendable {
    public var id = UUID()
    public var from: RouteEndpoint
    public var to: RouteEndpoint
    public var filter: RouteFilter = .all
    /// nil: any channel; else 0...15 (shown 1...16).
    public var inChannel: UInt8?
    /// nil: the same channel it came in on; else 0...15.
    public var outChannel: UInt8?
    /// Semitones added to notes; a note pushed past 0...127 is dropped.
    public var transpose = 0
    public var enabled = true
    /// Also route controls already forwarded to a module (CTRL) or mapped in the app.
    public var includeMapped = false
    /// A module source: the harmony voices as notes too, not only the sung note.
    public var harmonies = false
    /// A module source: knobs sent as CCs.
    public var knobCCs: [KnobCC] = []
    /// A gamepad source: the note of the first button (XInputButton's order).
    public var baseNote: UInt8 = 36

    public static let transposeRange = -48...48

    public init(from: RouteEndpoint, to: RouteEndpoint, filter: RouteFilter = .all,
                inChannel: UInt8? = nil, outChannel: UInt8? = nil, transpose: Int = 0, enabled: Bool = true) {
        self.from = from; self.to = to; self.filter = filter
        self.inChannel = inChannel.map { min(15, $0) }; self.outChannel = outChannel.map { min(15, $0) }
        self.transpose = min(Self.transposeRange.upperBound, max(Self.transposeRange.lowerBound, transpose))
        self.enabled = enabled
    }

    enum CodingKeys: String, CodingKey {
        case id, from, to, filter, inChannel, outChannel, transpose, enabled, includeMapped, harmonies, knobCCs, baseNote
    }

    public init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        from = try c.decode(RouteEndpoint.self, forKey: .from)
        to = try c.decode(RouteEndpoint.self, forKey: .to)
        filter = try c.decodeIfPresent(RouteFilter.self, forKey: .filter) ?? .all
        inChannel = try c.decodeIfPresent(UInt8.self, forKey: .inChannel).map { min(15, $0) }
        outChannel = try c.decodeIfPresent(UInt8.self, forKey: .outChannel).map { min(15, $0) }
        transpose = min(48, max(-48, try c.decodeIfPresent(Int.self, forKey: .transpose) ?? 0))
        enabled = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? true
        includeMapped = try c.decodeIfPresent(Bool.self, forKey: .includeMapped) ?? false
        harmonies = try c.decodeIfPresent(Bool.self, forKey: .harmonies) ?? false
        knobCCs = try c.decodeIfPresent([KnobCC].self, forKey: .knobCCs) ?? []
        baseNote = min(127, try c.decodeIfPresent(UInt8.self, forKey: .baseNote) ?? 36)
    }

    /// What can actually pass: a gamepad sends only notes; a module's MIDI
    /// out is notes and (to a device) its knobs as CCs; a module plays notes
    /// and takes CC 64 as its sustain pedal (other CCs to a module act only
    /// through mappings learned in the app).
    public var effectiveFilter: RouteFilter {
        var f = filter
        if case .gamepad = from { f.formIntersection(.notes) }
        if from.isModule { f.formIntersection(to.isModule ? .notes : [.notes, .cc]) }
        if to.isModule { f.formIntersection([.notes, .cc]) }
        return f
    }

    /// Not to a gamepad (a route to itself is refused as a loop).
    public var isValid: Bool { to.canReceive }

    /// "Notes, CC · ch 1 → 2 · +12" for a list row.
    public var summary: String {
        var parts = [effectiveFilter.summary]
        let o = outChannel.map { "ch \($0 + 1)" } ?? "same ch"
        if from.isModule || isGamepadSource {
            // Made by the app on channel 1.
            if let c = outChannel, !to.isModule { parts.append("on ch \(c + 1)") }
        } else if (inChannel != nil || outChannel != nil) && !to.isModule {
            parts.append("\(inChannel.map { "ch \($0 + 1)" } ?? "any ch") → \(o)")
        } else if let c = inChannel {
            parts.append("ch \(c + 1) only")
        }
        if transpose != 0 && effectiveFilter.contains(.notes) { parts.append(transpose > 0 ? "+\(transpose)" : "\(transpose)") }
        if from.isModule && harmonies { parts.append("with harmonies") }
        if from.isModule && !to.isModule && !knobCCs.isEmpty && effectiveFilter.contains(.cc) {
            parts.append(knobCCs.count == 1 ? "1 knob as a CC" : "\(knobCCs.count) knobs as CCs")
        }
        if includeMapped && !from.isModule { parts.append("mapped controls too") }
        return parts.joined(separator: " · ")
    }

    private var isGamepadSource: Bool { if case .gamepad = from { return true }; return false }
}

/// What the router asks the app to send.
public enum RouteOutput: Equatable, Sendable {
    /// A whole MIDI message to a device, by name.
    case midi(device: String, bytes: [UInt8])
    /// A note or the sustain pedal to a module, as CTRL events 6 and 7.
    case keyboard(unit: UInt32, CtrlEvent)
}

public enum RouteRefusal: Error, Equatable {
    /// The route chain would lead back to where it started.
    case loop
    /// To a gamepad, or from a thing to itself.
    case invalid
}

// MARK: - Module MIDI out (Belt's sung note)

public enum NotePart: String, Codable, Sendable { case sung, harmony }

/// A note a module's MIDI out starts (velocity 1...127) or ends (0).
public struct ModuleNote: Equatable, Sendable {
    public var part: NotePart
    public var note: UInt8
    public var velocity: UInt8
    public init(_ part: NotePart, _ note: UInt8, _ velocity: UInt8) { self.part = part; self.note = note; self.velocity = velocity }
}

/// Belt's sung note (the note it pulls the voice to) and its harmony voices
/// as notes, from each STATE: a note on when a note starts, a note off when
/// it ends or changes. Velocity from the input level when it starts. A
/// harmony on the sung note itself (unison) isn't sent twice.
public struct BeltNoteFollower: Sendable {
    public private(set) var sung: UInt8?
    public private(set) var harmony: Set<UInt8> = []
    public init() {}

    /// −60 dBFS (or less) is 1, 0 dBFS is 127.
    public static func velocity(inLevel db: Double) -> UInt8 {
        UInt8(max(1, min(127, Int(((db + 60) / 60 * 126).rounded()) + 1)))
    }

    public mutating func update(_ s: ModuleState) -> [ModuleNote] {
        guard let b = s.belt else { return release() }
        return update(belt: b, inLevel: s.inLevel)
    }

    public mutating func update(belt b: BeltState, inLevel: Double) -> [ModuleNote] {
        let on = b.voiced ? b.target : nil
        let h = on == nil ? [] : b.harmonyNotes.compactMap { $0 }
        return update(sung: on, harmonies: h, velocity: Self.velocity(inLevel: inLevel))
    }

    public mutating func update(sung s: Int?, harmonies: [Int], velocity: UInt8) -> [ModuleNote] {
        func valid(_ n: Int) -> UInt8? { (0...127).contains(n) ? UInt8(n) : nil }
        let newSung = s.flatMap(valid)
        var newHarm = Set(harmonies.compactMap(valid))
        if let n = newSung { newHarm.remove(n) }
        let v = max(1, min(127, velocity))
        var offs: [ModuleNote] = [], ons: [ModuleNote] = []
        for n in harmony.subtracting(newHarm).sorted() { offs.append(ModuleNote(.harmony, n, 0)) }
        if sung != newSung {
            if let old = sung { offs.append(ModuleNote(.sung, old, 0)) }
            if let n = newSung { ons.append(ModuleNote(.sung, n, v)) }
        }
        for n in newHarm.subtracting(harmony).sorted() { ons.append(ModuleNote(.harmony, n, v)) }
        sung = newSung
        harmony = newHarm
        return offs + ons
    }

    /// Everything off (the module went away, or stopped singing Belt).
    public mutating func release() -> [ModuleNote] { update(sung: nil, harmonies: [], velocity: 1) }
}

// MARK: - The router

/// Every route, and what a message from a source does. Keeps the notes each
/// route holds, so they can be let go. Used from one thread (the app's main
/// actor).
public final class MIDIRouter: @unchecked Sendable {
    public private(set) var routes: [MIDIRoute] = []
    /// Modules whose firmware left the keyboard bit out of CTRL's
    /// present_seen: it can't play notes, so none are sent to it.
    public private(set) var noNotes: Set<UInt32> = []

    private struct Dest: Hashable { var to: RouteEndpoint; var channel: UInt8; var note: UInt8 }
    private struct Pedal: Hashable { var to: RouteEndpoint; var channel: UInt8 }

    /// Per route: input (channel << 8 | note) to the note it sent.
    private var held: [UUID: [UInt16: Dest]] = [:]
    /// How many routes hold each note, so one source letting go doesn't cut another.
    private var sounding: [Dest: Int] = [:]
    private var pedalsHeld: [UUID: Set<Pedal>] = [:]
    private var pedals: [Pedal: Int] = [:]
    private let knobs: MIDIOutQueue

    public init(knobPeriod: Double = 1.0 / 60) { knobs = MIDIOutQueue(period: knobPeriod) }

    // MARK: changing routes

    /// Routes as saved (nothing held yet). Invalid ones and any that would
    /// close a loop are dropped.
    public func load(_ rs: [MIDIRoute]) {
        routes = []
        held = [:]; sounding = [:]; pedalsHeld = [:]; pedals = [:]
        for r in rs where r.isValid && !Self.makesLoop(r, with: routes) { routes.append(r) }
    }

    /// A route chain from the new route's destination that leads back to
    /// its source would echo forever. Disabled routes count (enabling one
    /// later mustn't close a loop).
    public static func makesLoop(_ new: MIDIRoute, with existing: [MIDIRoute]) -> Bool {
        if new.from == new.to { return true }
        var seen: Set<RouteEndpoint> = []
        var frontier = [new.to]
        while let e = frontier.popLast() {
            if e == new.from { return true }
            guard seen.insert(e).inserted else { continue }
            frontier += existing.filter { $0.id != new.id && $0.from == e }.map(\.to)
        }
        return false
    }

    /// Add a route, or change one (the same id). Notes the old version held
    /// are let go first.
    @discardableResult
    public func save(_ r: MIDIRoute) throws -> [RouteOutput] {
        guard r.isValid else { throw RouteRefusal.invalid }
        guard !Self.makesLoop(r, with: routes) else { throw RouteRefusal.loop }
        var out: [RouteOutput] = []
        if let i = routes.firstIndex(where: { $0.id == r.id }) {
            out = release(route: routes[i].id)
            routes[i] = r
        } else {
            routes.append(r)
        }
        return out
    }

    public func remove(_ id: UUID) -> [RouteOutput] {
        let out = release(route: id)
        routes.removeAll { $0.id == id }
        return out
    }

    public func setEnabled(_ id: UUID, _ on: Bool) -> [RouteOutput] {
        guard let i = routes.firstIndex(where: { $0.id == id }), routes[i].enabled != on else { return [] }
        routes[i].enabled = on
        return on ? [] : release(route: id)
    }

    // MARK: who uses what

    /// An enabled route plays this module as a keyboard (and it can).
    public func feedsKeyboard(unit: UInt32) -> Bool {
        !noNotes.contains(unit) && routes.contains { $0.enabled && $0.to == .module(unit) && $0.effectiveFilter.contains(.notes) }
    }

    /// Any route, on or off, starts or ends at this.
    public func uses(_ e: RouteEndpoint) -> Bool { routes.contains { $0.from == e || $0.to == e } }

    /// The module answered CTRL: if it was asked to take notes and left
    /// bit 3 out, its firmware can't play them, so none are sent to it
    /// (until it goes away and comes back, perhaps with other firmware).
    public func ctrlReply(unit: UInt32, asked: CtrlPresent, seen: CtrlPresent) {
        guard asked.contains(.keyboard), !seen.contains(.keyboard), !noNotes.contains(unit) else { return }
        noNotes.insert(unit)
        // It played none of them: forget them without sending offs.
        for id in held.keys {
            for (k, d) in held[id]! where d.to == .module(unit) { held[id]![k] = nil; drop(d) }
        }
        for id in pedalsHeld.keys {
            for p in pedalsHeld[id]! where p.to == .module(unit) { pedalsHeld[id]!.remove(p); pedals[p] = nil }
        }
    }

    /// A device, gamepad or module went away. Notes it was playing through
    /// any route are let go; notes sent to it are forgotten (it can't hear
    /// an off now).
    public func gone(_ e: RouteEndpoint) -> [RouteOutput] {
        var out: [RouteOutput] = []
        for r in routes where r.from == e { out += release(route: r.id) }
        for r in routes where r.to == e {
            for d in (held[r.id] ?? [:]).values { drop(d) }
            held[r.id] = nil
            for p in pedalsHeld[r.id] ?? [] { pedals[p] = nil }
            pedalsHeld[r.id] = nil
        }
        if case let .device(name) = e { knobs.forget(device: name) }
        if let u = e.unit { noNotes.remove(u) }
        return out
    }

    /// A device came (back): its knob CCs go out again on the next change.
    public func arrived(_ e: RouteEndpoint) {
        if case let .device(name) = e { knobs.forget(device: name) }
    }

    // MARK: messages in

    /// A message from a MIDI device. `consumed`: the control was forwarded
    /// to a module by CTRL or mapped in the app, so it's routed only where
    /// the route says to include those. Note offs for held notes always pass.
    public func input(from e: RouteEndpoint, _ m: RawMIDIMessage, consumed: Bool = false) -> [RouteOutput] {
        guard m.category != nil else { return [] }
        var out: [RouteOutput] = []
        for r in routes where r.from == e { out += pass(r, m, consumed: consumed) }
        return out
    }

    /// A gamepad button, as a note: the route's base note plus the button's
    /// place in XInputButton's order (up, down, left, right, start, back,
    /// L3, R3, LB, RB, home, A, B, X, Y, LT, RT).
    public func gamepad(_ name: String, button b: XInputButton, down: Bool, consumed: Bool = false) -> [RouteOutput] {
        let idx = XInputButton.allCases.firstIndex(of: b) ?? 0
        var out: [RouteOutput] = []
        for r in routes where r.from == .gamepad(name) {
            let n = Int(r.baseNote) + idx
            guard n <= 127 else { continue }
            let m = RawMIDIMessage(down ? [0x90, UInt8(n), 100] : [0x80, UInt8(n), 0])
            out += pass(r, m, consumed: consumed)
        }
        return out
    }

    /// Notes from a module's MIDI out (BeltNoteFollower), on channel 1.
    public func moduleNotes(unit: UInt32, _ notes: [ModuleNote]) -> [RouteOutput] {
        var out: [RouteOutput] = []
        for n in notes {
            let m = RawMIDIMessage(n.velocity > 0 ? [0x90, n.note & 0x7F, n.velocity] : [0x80, n.note & 0x7F, 0])
            for r in routes where r.from == .module(unit) && (n.part == .sung || r.harmonies) {
                out += pass(r, m, consumed: false)
            }
        }
        return out
    }

    /// Knobs that moved on a module: those a route sends as CCs are queued
    /// (only on change, at most once per period; see `flush`).
    public func moduleKnobs(unit: UInt32, _ changes: [String: Double]) {
        for r in routes where r.enabled && r.from == .module(unit) && r.effectiveFilter.contains(.cc) {
            guard case let .device(name) = r.to else { continue }
            for k in r.knobCCs {
                guard let v = changes[k.field] else { continue }
                knobs.cc(MIDIOutTarget(device: name, channel: r.outChannel ?? 0, number: k.cc), v)
            }
        }
    }

    public var hasPending: Bool { knobs.hasPending }

    /// The knob CCs due now.
    public func flush(now: Double) -> [RouteOutput] {
        knobs.flush(now: now).flatMap { d in d.messages.map { RouteOutput.midi(device: d.device, bytes: $0) } }
    }

    // MARK: inside

    private func pass(_ r: MIDIRoute, _ m: RawMIDIMessage, consumed: Bool) -> [RouteOutput] {
        if m.isNoteOff {
            let key = UInt16(m.channel ?? 0) << 8 | UInt16(m.bytes[1])
            guard let d = held[r.id]?[key] else { return [] }
            held[r.id]![key] = nil
            return noteOff(d)
        }
        guard r.enabled, !consumed || r.includeMapped, let cat = m.category,
              r.effectiveFilter.contains(cat) else { return [] }
        if let ch = m.channel, let want = r.inChannel, ch != want { return [] }
        let outCh = r.outChannel ?? m.channel ?? 0

        if m.isNoteOn {
            let n = Int(m.bytes[1]) + r.transpose
            guard (0...127).contains(n) else { return [] }
            if let u = r.to.unit, noNotes.contains(u) { return [] }
            let d = Dest(to: r.to, channel: r.to.isModule ? 0 : outCh, note: UInt8(n))
            let key = UInt16(m.channel ?? 0) << 8 | UInt16(m.bytes[1])
            var out: [RouteOutput] = []
            if let old = held[r.id]?[key] {
                if old != d { out += noteOff(old) } else { return [noteOn(d, m.bytes[2])] }
            }
            held[r.id, default: [:]][key] = d
            sounding[d, default: 0] += 1
            return out + [noteOn(d, m.bytes[2])]
        }

        let isPedal = m.status & 0xF0 == 0xB0 && m.bytes.count == 3 && m.bytes[1] == 64
        switch r.to {
        case let .module(u):
            guard isPedal, !noNotes.contains(u) else { return [] }   // other CCs: learned mappings only
            return pedal(r, Pedal(to: r.to, channel: 0), down: m.bytes[2] >= 64, value: m.bytes[2])
        case let .device(name):
            guard m.isChannel else { return [.midi(device: name, bytes: m.bytes)] }   // clock and transport
            if isPedal { return pedal(r, Pedal(to: r.to, channel: outCh), down: m.bytes[2] >= 64, value: m.bytes[2]) }
            var b = m.bytes
            b[0] = (b[0] & 0xF0) | outCh
            if b[0] & 0xF0 == 0xA0, b.count == 3 {                  // poly pressure follows the transpose
                let n = Int(b[1]) + r.transpose
                guard (0...127).contains(n) else { return [] }
                b[1] = UInt8(n)
            }
            return [.midi(device: name, bytes: b)]
        case .gamepad:
            return []
        }
    }

    private func noteOn(_ d: Dest, _ velocity: UInt8) -> RouteOutput {
        switch d.to {
        case let .module(u): return .keyboard(unit: u, .keyNote(note: d.note, velocity: max(1, velocity)))
        case let .device(name): return .midi(device: name, bytes: [0x90 | d.channel, d.note, velocity])
        case .gamepad: return .midi(device: "", bytes: [])
        }
    }

    /// One route lets go of a note: the off goes out once no route holds it.
    private func noteOff(_ d: Dest) -> [RouteOutput] {
        let left = (sounding[d] ?? 1) - 1
        sounding[d] = left > 0 ? left : nil
        guard left <= 0 else { return [] }
        switch d.to {
        case let .module(u): return [.keyboard(unit: u, .keyNote(note: d.note, velocity: 0))]
        case let .device(name): return [.midi(device: name, bytes: [0x80 | d.channel, d.note, 0])]
        case .gamepad: return []
        }
    }

    /// Forget a note without sending its off.
    private func drop(_ d: Dest) {
        let left = (sounding[d] ?? 1) - 1
        sounding[d] = left > 0 ? left : nil
    }

    private func pedalMessage(_ p: Pedal, down: Bool, value: UInt8) -> RouteOutput {
        switch p.to {
        case let .module(u): return .keyboard(unit: u, .keySustain(down: down))
        case let .device(name): return .midi(device: name, bytes: [0xB0 | p.channel, 64, value])
        case .gamepad: return .midi(device: "", bytes: [])
        }
    }

    /// The sustain pedal: up goes out once no route holds it down.
    private func pedal(_ r: MIDIRoute, _ p: Pedal, down: Bool, value: UInt8) -> [RouteOutput] {
        if down {
            if pedalsHeld[r.id, default: []].insert(p).inserted { pedals[p, default: 0] += 1 }
            return [pedalMessage(p, down: true, value: value)]
        }
        if pedalsHeld[r.id]?.remove(p) != nil {
            let left = (pedals[p] ?? 1) - 1
            pedals[p] = left > 0 ? left : nil
        }
        return (pedals[p] ?? 0) > 0 ? [] : [pedalMessage(p, down: false, value: value)]
    }

    /// Everything one route holds, let go.
    private func release(route id: UUID) -> [RouteOutput] {
        var out: [RouteOutput] = []
        let notes = (held[id] ?? [:]).sorted { $0.key < $1.key }
        held[id] = nil
        for (_, d) in notes { out += noteOff(d) }
        for p in (pedalsHeld[id] ?? []).sorted(by: { $0.channel < $1.channel }) {
            let left = (pedals[p] ?? 1) - 1
            pedals[p] = left > 0 ? left : nil
            if left <= 0 { out.append(pedalMessage(p, down: false, value: 0)) }
        }
        pedalsHeld[id] = nil
        return out
    }
}
