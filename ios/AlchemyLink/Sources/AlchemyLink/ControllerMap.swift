// The app's own uses for controller controls: any knob, fader, pad or
// button on a Launchpad, Launch Control XL, gamepad or MIDI device (a
// keyboard, a Daisy Patch) can move a knob on either module (over a range),
// the XY pad's X or Y, switch linking (shared key, split harmonies), press
// HARD or mute, step through presets, or send a CC or note to another MIDI
// device. One control can drive several targets, each with its own range.
// The app's own sources (the pad, tilt, the mic) can drive MIDI out the
// same way. Learned by moving the control. A control mapped here is not
// forwarded to the module. Kept free of UIKit so it is tested on Linux.

import Foundation

/// One physical control on a controller.
public enum ControlElement: Hashable, Codable, Sendable {
    case lpPad(LPKind, x: UInt8, y: UInt8)
    case xlKnob(row: UInt8, col: UInt8)
    case xlFader(col: UInt8)
    case xlButton(row: UInt8, col: UInt8)
    case gamepad(XInputButton)
    /// A gamepad button XInput has no bit for (a touchpad click, a capture button), by GameController's name.
    case gamepadExtra(String)
    /// From any other MIDI device; channel 0...15.
    case midiCC(channel: UInt8, number: UInt8)
    case midiNote(channel: UInt8, number: UInt8)
    /// One of the app's own sources (the pad, tilt, the mic), on the
    /// device `ControllerMap.appDevice`; never learned, only mapped to MIDI out.
    case app(ControlSource)

    /// Pressed and released, rather than turned.
    public var isButton: Bool {
        switch self {
        case .xlKnob, .xlFader, .midiCC, .app: return false
        default: return true
        }
    }

    public var label: String {
        switch self {
        case let .lpPad(k, x, y):
            switch k {
            case .grid: return "Pad \(x + 1), row \(y + 1)"
            case .side: return "Side button \(y + 1)"
            case .top: return "Top button \(x + 1)"
            }
        case let .xlKnob(r, c): return "Knob \(c + 1), row \(r + 1)"
        case let .xlFader(c): return "Fader \(c + 1)"
        case let .xlButton(r, c): return "\(r == 0 ? "Upper" : "Lower") button \(c + 1)"
        case let .gamepad(b): return b.name
        case let .gamepadExtra(n): return n
        case let .midiCC(ch, n): return "CC \(n), channel \(ch + 1)"
        case let .midiNote(ch, n): return "Note \(BeltTheory.name(Int(n))), channel \(ch + 1)"
        case let .app(s): return s.name
        }
    }

    /// A plain MIDI message as a control and its value (0...1; buttons 1 down, 0 up).
    public static func from(midi m: MIDIMessage) -> (ControlElement, Double)? {
        guard case let .channel(st, d1, d2) = m else { return nil }
        let ch = st & 0x0F
        switch st & 0xF0 {
        case 0xB0: return (.midiCC(channel: ch, number: d1), Double(d2) / 127)
        case 0x90: return (.midiNote(channel: ch, number: d1), d2 > 0 ? 1 : 0)
        case 0x80: return (.midiNote(channel: ch, number: d1), 0)
        default: return nil
        }
    }
}

/// A control on a named controller (mappings are kept by the controller's
/// name, so they come back when it's plugged in again).
public struct ControllerControl: Hashable, Codable, Sendable {
    public var device: String
    public var element: ControlElement
    public init(device: String, element: ControlElement) { self.device = device; self.element = element }
}

/// A CC or note on another MIDI device (not an Alchemy Lab), by its name.
public struct MIDIOutTarget: Hashable, Codable, Sendable {
    public var device: String
    /// 0...15 (shown as 1...16).
    public var channel: UInt8
    /// The CC or note number, 0...127.
    public var number: UInt8
    public init(device: String, channel: UInt8, number: UInt8) {
        self.device = device; self.channel = min(15, channel); self.number = min(127, number)
    }
}

public enum ControllerAction: Hashable, Codable, Sendable {
    case knob(ControlTarget)
    case padX, padY
    case shareKey, splitHarmonies
    /// HARD and mute (PRESS buttons 0 and 1); unit nil = every module.
    case hard(unit: UInt32?), mute(unit: UInt32?)
    case presetNext(unit: UInt32?), presetPrevious(unit: UInt32?)
    /// A CC to another MIDI device, over a range.
    case midiCC(MIDIOutTarget)
    /// A note to another MIDI device: on while held (or latched), or past half way.
    case midiNote(MIDIOutTarget)

    /// Sweeps part of a range (a knob, the pad's X or Y, a CC).
    public var takesRange: Bool {
        switch self {
        case .knob, .padX, .padY, .midiCC: return true
        default: return false
        }
    }

    /// A button mapped to this can latch instead of holding.
    public var hasToggle: Bool {
        switch self {
        case .presetNext, .presetPrevious: return false
        default: return true
        }
    }

    /// The module it acts on, if one.
    public var unit: UInt32? {
        switch self {
        case let .knob(t): return t.unit
        case let .hard(u), let .mute(u), let .presetNext(u), let .presetPrevious(u): return u
        default: return nil
        }
    }
}

public enum ButtonMode: String, Codable, Sendable, CaseIterable {
    /// Held: on while down (HARD and mute hold too).
    case momentary
    /// Each press flips it (HARD and mute latch, as a tap on the module does).
    case toggle
}

public struct ControllerMapping: Identifiable, Hashable, Codable, Sendable {
    public var id = UUID()
    public var control: ControllerControl
    public var action: ControllerAction
    /// The 0...1 range swept (lo > hi turns it round); a button jumps between them.
    public var lo: Double = 0, hi: Double = 1
    public var mode: ButtonMode = .momentary

    public init(control: ControllerControl, action: ControllerAction, lo: Double = 0, hi: Double = 1,
                mode: ButtonMode = .momentary) {
        self.control = control; self.action = action; self.lo = lo; self.hi = hi; self.mode = mode
    }
}

/// What a mapped control asks the app to do.
public enum ControllerOutput: Equatable {
    case knob(ControlTarget, Double)
    case padX(Double), padY(Double)
    /// nil flips it.
    case shareKey(Bool?), splitHarmonies(Bool?)
    /// Button 0 HARD, 1 mute; unit nil = every module.
    case press(unit: UInt32?, button: Int, gesture: HostLinkClient.PressGesture)
    case preset(unit: UInt32?, step: Int)
    /// 0...1, already over the mapping's range.
    case midiCC(MIDIOutTarget, Double)
    case midiNote(MIDIOutTarget, on: Bool)
}

/// Every mapping, learn mode, and what a control's movement does. Only the
/// mappings are saved; latches and learning start fresh.
public struct ControllerMap: Codable {
    /// The device name the app's own sources are mapped under.
    public static let appDevice = "This device"

    public private(set) var mappings: [ControllerMapping] = []

    /// Waiting for a control to move.
    public private(set) var learning = false
    /// The control that moved while learning, waiting for an action.
    public private(set) var learned: ControllerControl?
    /// Toggle states, and for a knob or fader driving a switch whether it's past half way.
    private var latched: [UUID: Bool] = [:]

    enum CodingKeys: String, CodingKey { case mappings }

    public init(mappings: [ControllerMapping] = []) { self.mappings = mappings }

    /// Every target of one control, in the order added.
    public func mappings(for c: ControllerControl) -> [ControllerMapping] { mappings.filter { $0.control == c } }
    public func mappings(device: String) -> [ControllerMapping] { mappings.filter { $0.control.device == device } }
    /// The controls that have at least one target, in the order first mapped.
    public var controls: [ControllerControl] {
        var seen = Set<ControllerControl>(), out: [ControllerControl] = []
        for m in mappings where seen.insert(m.control).inserted { out.append(m.control) }
        return out
    }

    /// Kept away from the module: mapped here, or the control being learned.
    public func consumes(_ c: ControllerControl) -> Bool {
        learning || learned == c || mappings.contains { $0.control == c }
    }

    // MARK: learning

    public mutating func beginLearn() { learning = true; learned = nil }
    /// Done with the learned control (its targets are kept), or gave up.
    public mutating func cancelLearn() { learning = false; learned = nil }
    public mutating func finishLearn() { cancelLearn() }

    /// Add a target to a control, or change one (the same id). A control
    /// keeps its other targets; the control just learned stays learned
    /// until `finishLearn`, so more targets can be added to it.
    public mutating func assign(_ m: ControllerMapping) {
        if let i = mappings.firstIndex(where: { $0.id == m.id }) { mappings[i] = m } else { mappings.append(m) }
        latched[m.id] = nil
        learning = false
    }

    public mutating func remove(_ id: UUID) {
        mappings.removeAll { $0.id == id }
        latched[id] = nil
    }

    /// Every target of one control.
    public mutating func remove(control c: ControllerControl) {
        for m in mappings(for: c) { latched[m.id] = nil }
        mappings.removeAll { $0.control == c }
    }

    public mutating func removeAll(device: String) { mappings.removeAll { $0.control.device == device } }

    /// Mappings to a module or a knob that isn't there any more stay (it
    /// may be plugged in again); this drops them on request.
    public mutating func prune(keeping ok: (ControllerAction) -> Bool) { mappings.removeAll { !ok($0.action) } }

    // MARK: a control moved

    /// A control moved (`value` 0...1; buttons 1 down, 0 up). nil when the
    /// app has no use for it, so it goes on to the module; otherwise what
    /// the app should do (often nothing yet).
    public mutating func handle(_ c: ControllerControl, value: Double) -> [ControllerOutput]? {
        if learning {
            // A button is learned on its press; the release that follows is
            // kept back too (see consumes).
            if c.element.isButton && value < 0.5 { return [] }
            learning = false
            learned = c
            return []
        }
        let ms = mappings(for: c)
        if ms.isEmpty { return learned == c ? [] : nil }
        return outputs(ms, c, value)
    }

    /// One of the app's own sources moved: its MIDI out (no learning here,
    /// so tilt or the mic never get learned by accident).
    public mutating func drive(_ s: ControlSource, value: Double) -> [ControllerOutput] {
        outputs(mappings(for: ControllerControl(device: Self.appDevice, element: .app(s))),
                ControllerControl(device: Self.appDevice, element: .app(s)), value)
    }

    private mutating func outputs(_ ms: [ControllerMapping], _ c: ControllerControl, _ value: Double) -> [ControllerOutput] {
        var out: [ControllerOutput] = []
        for m in ms { out += c.element.isButton ? button(m, down: value >= 0.5) : continuous(m, value) }
        return out
    }

    private func ranged(_ m: ControllerMapping, _ v: Double) -> Double {
        let c = min(1, max(0, v))
        return min(1, max(0, m.lo + (m.hi - m.lo) * c))
    }

    private func value(_ m: ControllerMapping, _ v: Double) -> ControllerOutput? {
        switch m.action {
        case let .knob(t): return .knob(t, ranged(m, v))
        case .padX: return .padX(ranged(m, v))
        case .padY: return .padY(ranged(m, v))
        case let .midiCC(t): return .midiCC(t, ranged(m, v))
        default: return nil
        }
    }

    private mutating func button(_ m: ControllerMapping, down: Bool) -> [ControllerOutput] {
        switch m.action {
        case .knob, .padX, .padY, .midiCC:
            if m.mode == .momentary { return value(m, down ? 1 : 0).map { [$0] } ?? [] }
            guard down else { return [] }
            let on = !(latched[m.id] ?? false)
            latched[m.id] = on
            return value(m, on ? 1 : 0).map { [$0] } ?? []
        case .shareKey, .splitHarmonies:
            let on: Bool?
            if m.mode == .momentary { on = down } else if down { on = nil } else { return [] }
            return [m.action == .shareKey ? .shareKey(on) : .splitHarmonies(on)]
        case let .hard(u), let .mute(u):
            let b = m.action == .hard(unit: u) ? 0 : 1
            if m.mode == .toggle { return down ? [.press(unit: u, button: b, gesture: .tap)] : [] }
            return [.press(unit: u, button: b, gesture: down ? .down : .up)]
        case let .presetNext(u): return down ? [.preset(unit: u, step: 1)] : []
        case let .presetPrevious(u): return down ? [.preset(unit: u, step: -1)] : []
        case let .midiNote(t):
            if m.mode == .momentary { return [.midiNote(t, on: down)] }
            guard down else { return [] }
            let on = !(latched[m.id] ?? false)
            latched[m.id] = on
            return [.midiNote(t, on: on)]
        }
    }

    private mutating func continuous(_ m: ControllerMapping, _ v: Double) -> [ControllerOutput] {
        if let out = value(m, v) { return [out] }
        // A knob or fader driving a switch: past half way is on, with a
        // little hysteresis so a knob resting near the middle doesn't chatter.
        let was = latched[m.id] ?? false
        let on = was ? v > 0.45 : v >= 0.55
        guard on != was else { return [] }
        latched[m.id] = on
        switch m.action {
        case .shareKey: return [.shareKey(on)]
        case .splitHarmonies: return [.splitHarmonies(on)]
        case let .hard(u), let .mute(u):
            let b = m.action == .hard(unit: u) ? 0 : 1
            if m.mode == .toggle { return on ? [.press(unit: u, button: b, gesture: .tap)] : [] }
            return [.press(unit: u, button: b, gesture: on ? .down : .up)]
        case let .presetNext(u): return on ? [.preset(unit: u, step: 1)] : []
        case let .presetPrevious(u): return on ? [.preset(unit: u, step: -1)] : []
        case let .midiNote(t): return [.midiNote(t, on: on)]
        case .knob, .padX, .padY, .midiCC: return []
        }
    }
}

/// MIDI out to other devices: CCs sent only when their value changes and at
/// most once per `period` (the newest value wins, as with knob writes);
/// notes at once, only when on/off changes. Used from one thread (the
/// app's main actor).
public final class MIDIOutQueue: @unchecked Sendable {
    public var period: Double
    private var pending: [MIDIOutTarget: UInt8] = [:]
    private var order: [MIDIOutTarget] = []
    private var sent: [MIDIOutTarget: UInt8] = [:]
    private var notes: [MIDIOutTarget: Bool] = [:]
    private var lastFlush = -Double.infinity

    public init(period: Double) { self.period = period }

    public var hasPending: Bool { !order.isEmpty }

    /// A CC's new value, 0...1 (to 0...127).
    public func cc(_ t: MIDIOutTarget, _ v: Double) {
        let b = UInt8((min(1, max(0, v)) * 127).rounded())
        if sent[t] == b {
            pending[t] = nil
            order.removeAll { $0 == t }
            return
        }
        if pending[t] == nil { order.append(t) }
        pending[t] = b
    }

    /// The message to send now for a note, or nil if it's already in that state.
    public func note(_ t: MIDIOutTarget, on: Bool) -> [UInt8]? {
        guard (notes[t] ?? false) != on else { return nil }
        notes[t] = on
        return on ? [0x90 | t.channel, t.number, 127] : [0x80 | t.channel, t.number, 0]
    }

    /// The CCs due now, by device, in the order they first changed.
    public func flush(now: Double) -> [(device: String, messages: [[UInt8]])] {
        guard !order.isEmpty, now - lastFlush >= period else { return [] }
        lastFlush = now
        var out: [(device: String, messages: [[UInt8]])] = []
        for t in order {
            guard let v = pending[t] else { continue }
            sent[t] = v
            let msg: [UInt8] = [0xB0 | t.channel, t.number, v]
            if let i = out.firstIndex(where: { $0.device == t.device }) { out[i].messages.append(msg) }
            else { out.append((t.device, [msg])) }
        }
        pending = [:]
        order = []
        return out
    }

    /// A device came (back): what it last had is unknown, so the next value goes out.
    public func forget(device: String) {
        sent = sent.filter { $0.key.device != device }
        notes = notes.filter { $0.key.device != device }
    }
}
