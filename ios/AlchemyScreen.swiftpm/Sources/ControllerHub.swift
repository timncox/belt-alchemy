// Controllers on the same USB-C hub as the modules: a Launchpad Mini MK3,
// a Launch Control XL, a gamepad (a Haute42 in PS4 or Switch mode, or any
// gamepad iOS knows), and any other MIDI device (a keyboard). Each one
// goes to Left, Right, both or the app only:
//
//  - Left / Right / Both: its events go to the module as 0x77 CTRL
//    (firmware/CONTROLLERS.md), so the module does what it does in its own
//    Launchpad USB mode, and the LEDs the module sends back are shown on
//    the Launchpad and the XL (with both, the first module's).
//  - Any control can also be mapped in the app (ControllerMap) to one or
//    more targets: a knob on either module, the pad's X or Y, linking, HARD
//    and mute, presets, or a CC or note on another MIDI device (a Daisy
//    Patch). A mapped control isn't forwarded. Writes go through the Rig,
//    so linking still applies. The app's own sources (pad, tilt, mic) can
//    drive MIDI out too; CCs go out on change, at most at the control rate.
//
// CTRL goes out at most once per control period while events wait, and as
// a poll for the LEDs once per state period while any controller is routed
// to the module. Routes and mappings are kept by the controller's name.
//
// MIDI routing (the patchbay, AlchemyLink/Routing.swift) runs here too: any
// MIDI device, a gamepad's buttons or a module's MIDI out (Belt's sung note
// from STATE, its knobs as CCs) to any MIDI device, or to a module played
// as a keyboard (CTRL events 6 and 7, present bit 3). Controls already
// forwarded or mapped are routed raw only where a route asks for them.
//
// The on-screen devices (VirtualDevices, for the Simulator and the demo)
// come in through the same doors: their MIDI ports are in the Rig's device
// list and open through Rig.connectRaw like CoreMIDI's, and the on-screen
// gamepad's buttons arrive as a gamepad's do. Same names, same code.

import Combine
import CoreMIDI
import Foundation
import GameController
import SwiftUI
import AlchemyLink

/// A controller in the list on the Controls page.
struct ControllerInfo: Identifiable, Equatable {
    var id: String
    var name: String
    var kind: ControllerKind
}

/// What arrives from CoreMIDI's thread and the gamepads' handler, kept in
/// order and taken on the main actor.
private final class ControllerInbox: @unchecked Sendable {
    enum Item {
        case midi(controller: String, port: MIDIUniqueID, bytes: [UInt8])
        case pad(controller: String, held: Set<XInputButton>, extras: Set<String>)
    }
    private let lock = NSLock()
    private var items: [Item] = []

    /// True when the caller should schedule a drain.
    func push(_ i: Item) -> Bool {
        lock.lock(); defer { lock.unlock() }
        items.append(i)
        return items.count == 1
    }

    func take() -> [Item] {
        lock.lock(); defer { lock.unlock() }
        defer { items.removeAll() }
        return items
    }
}

@MainActor
final class ControllerHub: ObservableObject {
    @Published private(set) var controllers: [ControllerInfo] = []
    @Published private(set) var routes: [String: ControllerRoute] = [:]
    @Published private(set) var mappings: [ControllerMapping] = []
    @Published private(set) var learning = false
    @Published private(set) var learned: ControllerControl?
    /// Learn was started from the on-screen devices: the Learn sheet waits
    /// for a control to move, so it doesn't cover the devices meanwhile.
    @Published private(set) var learnInPlace = false
    /// Modules (by unit) whose firmware answered UNSUPPORTED or lacks the CTRL flag.
    @Published private(set) var unsupported: Set<UInt32> = []
    /// The control last moved on each controller, for the list.
    @Published private(set) var lastMoved: [String: String] = [:]
    /// MIDI routes: the patchbay.
    @Published private(set) var midiRoutes: [MIDIRoute] = []
    /// Modules whose firmware left the keyboard bit out of CTRL's reply: no notes go to them.
    @Published private(set) var noNotes: Set<UInt32> = []

    @AppStorage("controllerRoutes") private var routesRaw = ""
    @AppStorage("controllerMap") private var mapRaw = ""
    @AppStorage("midiRoutes") private var midiRoutesRaw = ""

    private weak var rig: Rig?
    private weak var inputs: Inputs?
    private var map = ControllerMap()
    private var subs: Set<AnyCancellable> = []
    private let inbox = ControllerInbox()
    /// False while the app is in the background: the Launchpad goes back to
    /// Live mode and the modules are told the controllers have gone.
    private var active = true

    /// One plugged-in controller.
    private final class Plugged {
        let id: String
        let name: String
        let kind: ControllerKind
        let ports: [MIDIDevice]
        var open: [MIDIUniqueID: MIDIPortOut] = [:]
        var parsers: [MIDIUniqueID: MIDIRouteParser] = [:]
        var latch = PortLatch()
        var leds: LEDDiffer
        var gamepad: GCController?
        /// The on-screen gamepad (VirtualDevices), which has no GCController.
        var virtualPad = false
        var held: Set<XInputButton> = []
        var extras: Set<String> = []

        init(id: String, name: String, kind: ControllerKind, ports: [MIDIDevice]) {
            self.id = id; self.name = name; self.kind = kind; self.ports = ports
            leds = LEDDiffer(count: kind == .launchControlXL ? LaunchControlXL.leds : LaunchpadMK3.cells)
        }
        var isOpen: Bool { !open.isEmpty }
        /// A gamepad (real or on-screen) rather than MIDI ports.
        var isPad: Bool { gamepad != nil || virtualPad }
    }

    private static let virtualPadID = "virtual:gamepad"

    private var plugged: [String: Plugged] = [:]
    private var sessions: [UInt32: CtrlSession] = [:]
    /// The gamepad mask last queued for each module.
    private var sentMask: [UInt32: UInt32] = [:]
    private var lastUnits: [UInt32] = []
    private var loop: Task<Void, Never>?
    /// CCs and notes to other MIDI devices.
    private let midiOut = MIDIOutQueue(period: LinkKind.usb.controlPeriod)
    /// The patchbay, and each module's sung note as MIDI.
    private let router = MIDIRouter(knobPeriod: LinkKind.usb.controlPeriod)
    private var followers: [UInt32: BeltNoteFollower] = [:]

    // MARK: setup

    func attach(rig: Rig, inputs: Inputs) {
        guard self.rig == nil else { return }
        self.rig = rig
        self.inputs = inputs
        load()
        rig.$devices
            .receive(on: DispatchQueue.main)
            .sink { [weak self] d in Task { @MainActor in self?.reconcile(d) } }
            .store(in: &subs)
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            NotificationCenter.default.publisher(for: name)
                .receive(on: DispatchQueue.main)
                .sink { [weak self] _ in Task { @MainActor in self?.reconcilePads() } }
                .store(in: &subs)
        }
        rig.onDrive = { [weak self] values in self?.appSources(values) }
        rig.onModuleState = { [weak self] unit, st in self?.moduleState(unit, st) }
        rig.onModuleKnobs = { [weak self] unit, changes in self?.moduleKnobs(unit, changes) }
        // The on-screen gamepad: there while it's switched on, its buttons
        // queued as a real gamepad's are.
        let inbox = inbox, padID = Self.virtualPadID
        rig.virtual.onPad = { [weak self] held in
            if inbox.push(.pad(controller: padID, held: held, extras: [])) {
                Task { @MainActor in self?.drain() }
            }
        }
        rig.virtual.$gamepadName
            .receive(on: DispatchQueue.main)
            .sink { [weak self] n in Task { @MainActor in self?.reconcileVirtualPad(n) } }
            .store(in: &subs)
        reconcile(rig.devices)
        reconcilePads()
    }

    /// The app came forward (true) or went to the background (false).
    func setActive(_ on: Bool) {
        guard on != active else { return }
        active = on
        for p in plugged.values where p.isOpen && route(p) != .off { on ? start(p) : release(p) }
        if on { ensureLoop() }
    }

    private func load() {
        if let d = routesRaw.data(using: .utf8), let r = try? JSONDecoder().decode([String: ControllerRoute].self, from: d) {
            routes = r
        }
        if let d = mapRaw.data(using: .utf8), let m = try? JSONDecoder().decode(ControllerMap.self, from: d) { map = m }
        syncMap(save: false)
        if let d = midiRoutesRaw.data(using: .utf8), let r = try? JSONDecoder().decode([MIDIRoute].self, from: d) {
            router.load(r)
            midiRoutes = router.routes
        }
    }

    private func syncMap(save: Bool) {
        if mappings != map.mappings { mappings = map.mappings }
        if learning != map.learning { learning = map.learning }
        if learned != map.learned { learned = map.learned }
        guard save else { return }
        if let d = try? JSONEncoder().encode(map), let s = String(data: d, encoding: .utf8) { mapRaw = s }
    }

    // MARK: finding controllers

    /// MIDI devices that aren't modules, grouped by their CoreMIDI device
    /// (a Launchpad's DAW and MIDI ports are one controller).
    private func reconcile(_ devices: [MIDIDevice]) {
        guard let rig else { return }
        var groups: [String: (name: String, kind: ControllerKind, ports: [MIDIDevice])] = [:]
        for d in devices where !d.looksLikeModule && !rig.isConnected(d) {
            guard let k = ControllerKind.classify(midiName: d.name + " " + d.deviceName) else { continue }
            let key = d.deviceID != 0 ? "midi:\(d.deviceID)" : "port:\(d.id)"
            var g = groups[key] ?? (name: d.deviceName.isEmpty ? d.name : d.deviceName, kind: k, ports: [])
            if g.kind == .midi && k != .midi { g.kind = k }
            g.ports.append(d)
            groups[key] = g
        }
        for (id, p) in plugged where !p.isPad {
            if let g = groups[id], Set(g.ports.map(\.id)) == Set(p.ports.map(\.id)) { continue }
            unplug(id)
        }
        for (id, g) in groups where plugged[id] == nil {
            let p = Plugged(id: id, name: g.name, kind: g.kind, ports: g.ports.sorted { $0.name < $1.name })
            plugged[id] = p
            if wantsOpen(p) { open(p) }
        }
        publish()
    }

    private func reconcilePads() {
        let pads = GCController.controllers().filter { $0.extendedGamepad != nil }
        for (id, p) in plugged where p.gamepad != nil && !pads.contains(where: { $0 === p.gamepad }) { unplug(id) }
        for c in pads where !plugged.values.contains(where: { $0.gamepad === c }) {
            let id = "gc:\(ObjectIdentifier(c).hashValue)"
            let p = Plugged(id: id, name: c.vendorName ?? "Gamepad", kind: .gamepad, ports: [])
            p.gamepad = c
            plugged[id] = p
            let inbox = inbox
            // Called on the controller's handler queue (the main queue).
            c.extendedGamepad?.valueChangedHandler = { @Sendable [weak self] gamepad, _ in
                guard let c = gamepad.controller else { return }
                let (held, extras) = ControllerHub.snapshot(c)
                if inbox.push(.pad(controller: id, held: held, extras: extras)) {
                    Task { @MainActor in self?.drain() }
                }
            }
        }
        publish()
        ensureLoop()
    }

    /// The on-screen gamepad came, went or was renamed (nil: switched off).
    private func reconcileVirtualPad(_ name: String?) {
        if let p = plugged[Self.virtualPadID], p.name != name { unplug(p.id) }
        if let name, plugged[Self.virtualPadID] == nil {
            let p = Plugged(id: Self.virtualPadID, name: name, kind: .gamepad, ports: [])
            p.virtualPad = true
            plugged[p.id] = p
        }
        publish()
        ensureLoop()
    }

    /// What's held on a gamepad, as XInput's buttons. Face buttons are read
    /// by position (GameController's buttonA is the bottom one, B the right,
    /// X the left, Y the top), which is where XInput's A, B, X and Y sit. A
    /// d-pad sent as the left stick (a GP2040-CE option) counts as the d-pad.
    /// Buttons XInput has no bit for come back by name, for mappings only.
    nonisolated static func snapshot(_ c: GCController) -> (Set<XInputButton>, Set<String>) {
        guard let g = c.extendedGamepad else { return ([], []) }
        var held = Set<XInputButton>()
        let known: [(GCControllerButtonInput?, XInputButton)] = [
            (g.buttonA, .a), (g.buttonB, .b), (g.buttonX, .x), (g.buttonY, .y),
            (g.leftShoulder, .lb), (g.rightShoulder, .rb), (g.leftTrigger, .lt), (g.rightTrigger, .rt),
            (g.buttonMenu, .start), (g.buttonOptions, .back), (g.buttonHome, .guide),
            (g.leftThumbstickButton, .l3), (g.rightThumbstickButton, .r3),
            (g.dpad.up, .up), (g.dpad.down, .down), (g.dpad.left, .left), (g.dpad.right, .right),
        ]
        for (b, x) in known where b?.isPressed == true { held.insert(x) }
        let stick = g.leftThumbstick
        if stick.yAxis.value > 0.5 { held.insert(.up) }
        if stick.yAxis.value < -0.5 { held.insert(.down) }
        if stick.xAxis.value < -0.5 { held.insert(.left) }
        if stick.xAxis.value > 0.5 { held.insert(.right) }
        var extras = Set<String>()
        for (name, b) in c.physicalInputProfile.buttons where b.isPressed {
            if !known.contains(where: { $0.0 === b }) { extras.insert(name) }
        }
        return (held, extras)
    }

    private func publish() {
        let list = plugged.values
            .map { ControllerInfo(id: $0.id, name: $0.name, kind: $0.kind) }
            .sorted { ($0.kind.forwards ? 0 : 1, $0.name, $0.id) < ($1.kind.forwards ? 0 : 1, $1.name, $1.id) }
        if list != controllers { controllers = list }
    }

    // MARK: opening and letting go

    private func open(_ p: Plugged) {
        guard !p.isPad, !p.isOpen, let rig else { return }
        let inbox = inbox, id = p.id
        for port in p.ports {
            let pid = port.id
            // On CoreMIDI's thread: queued in order, taken on the main actor.
            let raw = rig.connectRaw(port) { @Sendable [weak self] bytes in
                if inbox.push(.midi(controller: id, port: pid, bytes: bytes)) {
                    Task { @MainActor in self?.drain() }
                }
            }
            if let raw { p.open[port.id] = raw }
        }
        midiOut.forget(device: p.name)
        router.arrived(.device(p.name))
        if active && route(p) != .off { start(p) }
        ensureLoop()
    }

    private func close(_ p: Plugged) {
        guard p.isOpen else { return }
        release(p)
        for id in p.open.keys { rig?.disconnectRaw(id) }
        p.open = [:]
        p.parsers = [:]
    }

    /// Programmer mode on the Launchpad, template 1 on the XL; every LED
    /// is sent again from the next frame.
    private func start(_ p: Plugged) {
        switch p.kind {
        case .launchpad:
            send(p, [LaunchpadMK3.programmerMode])
            p.latch.reset()
        case .launchControlXL:
            send(p, [LaunchControlXL.selectTemplate])
        default: return
        }
        p.leds.forget()
        refreshIdle(p)
    }

    /// The app stops using it: the Launchpad back to Live mode, the XL's LEDs off.
    private func release(_ p: Plugged) {
        switch p.kind {
        case .launchpad:
            send(p, [LaunchpadMK3.liveMode])
        case .launchControlXL:
            send(p, LaunchControlXL.messages(Self.dark(p), diff: &p.leds))
        default: break
        }
        p.leds.forget()
    }

    private func unplug(_ id: String) {
        guard let p = plugged.removeValue(forKey: id) else { return }
        for port in p.open.keys { rig?.disconnectRaw(port) }
        p.gamepad?.extendedGamepad?.valueChangedHandler = nil
        lastMoved[id] = nil
        updateMasks()
        deliver(router.gone(endpoint(p)))
    }

    private func endpoint(_ p: Plugged) -> RouteEndpoint { p.isPad ? .gamepad(p.name) : .device(p.name) }

    /// Open while it's routed in Controls, or used by a MIDI route (a
    /// controller set to Off can still be routed raw, or be a destination).
    private func wantsOpen(_ p: Plugged) -> Bool { route(p) != .off || router.uses(.device(p.name)) }

    private func send(_ p: Plugged, _ messages: [[UInt8]]) {
        guard !messages.isEmpty else { return }
        for port in p.open.values { port.send(messages) }
    }

    // MARK: routes

    private func route(_ p: Plugged) -> ControllerRoute {
        let r = routes[p.name] ?? ControllerRoute.defaultRoute(for: p.kind)
        return ControllerRoute.choices(for: p.kind).contains(r) ? r : ControllerRoute.defaultRoute(for: p.kind)
    }

    func route(of c: ControllerInfo) -> ControllerRoute {
        plugged[c.id].map { route($0) } ?? routes[c.name] ?? ControllerRoute.defaultRoute(for: c.kind)
    }

    func setRoute(_ r: ControllerRoute, for c: ControllerInfo) {
        var was: [String: ControllerRoute] = [:]
        for p in plugged.values where p.name == c.name { was[p.id] = route(p) }
        routes[c.name] = r
        if let d = try? JSONEncoder().encode(routes), let s = String(data: d, encoding: .utf8) { routesRaw = s }
        for p in plugged.values where p.name == c.name {
            if r == .off {
                // Still open for MIDI routing: let go of it as a controller only.
                if !wantsOpen(p) { close(p) } else if p.isOpen && was[p.id] != .off { release(p) }
            } else if !p.isOpen {
                open(p)
            } else if was[p.id] == .off {
                if active { start(p) }
            } else {
                refreshIdle(p)
            }
            if r == .off || r == .appOnly { p.leds.forget(); refreshIdle(p) }
        }
        for s in sessions.values { s.resetMask() }
        sentMask = [:]
        updateMasks()
        ensureLoop()
    }

    private var readyUnits: [UInt32] { rig?.ready.map(\.unit) ?? [] }

    /// The module whose LEDs this controller shows, if any.
    private func ledSource(_ p: Plugged) -> UInt32? {
        guard active, let u = route(p).units(readyUnits).first, !unsupported.contains(u) else { return nil }
        return u
    }

    private static func dark(_ p: Plugged) -> [UInt8] {
        p.kind == .launchControlXL
            ? Array(repeating: LaunchControlXL.colour(red: 0, green: 0), count: LaunchControlXL.leds)
            : Array(repeating: 0, count: LaunchpadMK3.cells)
    }

    /// A controller no module lights: all its LEDs off.
    private func refreshIdle(_ p: Plugged) {
        guard p.isOpen, route(p) != .off, p.kind == .launchpad || p.kind == .launchControlXL, ledSource(p) == nil else { return }
        showLEDs(p, Self.dark(p))
    }

    private func showLEDs(_ p: Plugged, _ frame: [UInt8]) {
        switch p.kind {
        case .launchpad: send(p, LaunchpadMK3.messages(frame, diff: &p.leds))
        case .launchControlXL: send(p, LaunchControlXL.messages(frame, diff: &p.leds))
        default: break
        }
    }

    // MARK: events in

    private func drain() {
        for item in inbox.take() {
            switch item {
            case let .midi(id, port, bytes): midiIn(id, port: port, bytes)
            case let .pad(id, held, extras): padIn(id, held: held, extras: extras)
            }
        }
        syncMap(save: false)
    }

    private func midiIn(_ id: String, port: MIDIUniqueID, _ bytes: [UInt8]) {
        guard let p = plugged[id], p.isOpen, active else { return }
        var parser = p.parsers[port] ?? MIDIRouteParser()
        var messages: [RawMIDIMessage] = []
        parser.feed(bytes) { messages.append($0) }
        p.parsers[port] = parser
        let controls = route(p) != .off
        var outs: [RouteOutput] = []
        for raw in messages {
            let m = raw.message
            if p.kind == .launchpad {
                // Each press comes on both ports; the first port heard is the
                // one used, for the module and for routing alike.
                if let m, LaunchpadMK3.event(m) != nil {
                    guard p.latch.accept(Int64(port)) else { continue }
                } else if let heard = p.latch.port, heard != Int64(port) {
                    continue
                }
            }
            // Forwarded to a module or mapped in the app: not routed raw
            // unless a route includes those.
            var consumed = false
            if controls, let m {
                switch p.kind {
                case .launchpad:
                    if let e = LaunchpadMK3.event(m) { consumed = control(p, e) }
                case .launchControlXL:
                    if LaunchControlXL.templateChanged(m) {
                        send(p, [LaunchControlXL.selectTemplate])
                        p.leds.forget()
                        refreshIdle(p)
                        continue
                    }
                    if let e = LaunchControlXL.event(m) { consumed = control(p, e) }
                case .midi:
                    if let hit = ControlElement.from(midi: m) {
                        let (element, v) = hit
                        moved(p, element)
                        if let out = map.handle(ControllerControl(device: p.name, element: element), value: v) {
                            apply(out)
                            consumed = true
                        }
                    }
                case .gamepad:
                    break
                }
            }
            outs += router.input(from: .device(p.name), raw, consumed: consumed)
        }
        deliver(outs)
    }

    /// A Launchpad or XL control: the app's mapping, else on to the
    /// module(s). True when it went to one or the other.
    @discardableResult
    private func control(_ p: Plugged, _ e: CtrlEvent) -> Bool {
        guard let element = e.element else { return false }
        moved(p, element)
        if let out = map.handle(ControllerControl(device: p.name, element: element), value: e.value) {
            apply(out)
            return true
        }
        let units = route(p).units(readyUnits).filter { !unsupported.contains($0) }
        for u in units { session(u).enqueue(e) }
        return !units.isEmpty
    }

    private func padIn(_ id: String, held: Set<XInputButton>, extras: Set<String>) {
        guard let p = plugged[id], active else { return }
        let controls = route(p) != .off
        let forwarded = controls && route(p).units(readyUnits).contains { !unsupported.contains($0) }
        var outs: [RouteOutput] = []
        for b in p.held.symmetricDifference(held).sorted(by: { $0.bit < $1.bit }) {
            let down = held.contains(b)
            var consumed = forwarded
            if controls {
                moved(p, .gamepad(b))
                if let out = map.handle(ControllerControl(device: p.name, element: .gamepad(b)), value: down ? 1 : 0) {
                    apply(out)
                    consumed = true
                }
            }
            outs += router.gamepad(p.name, button: b, down: down, consumed: consumed)
        }
        if controls {
            for name in p.extras.symmetricDifference(extras).sorted() {
                moved(p, .gamepadExtra(name))
                if let out = map.handle(ControllerControl(device: p.name, element: .gamepadExtra(name)), value: extras.contains(name) ? 1 : 0) {
                    apply(out)
                }
            }
        }
        p.held = held
        p.extras = extras
        updateMasks()
        deliver(outs)
    }

    /// Each module's gamepad mask: every gamepad routed to it, less the
    /// buttons the app has mapped, queued when it changes.
    private func updateMasks() {
        let units = readyUnits
        for u in units {
            var mask: UInt32 = 0
            for p in plugged.values where p.kind == .gamepad && route(p).units(units).contains(u) {
                mask |= XInput.mask(p.held.filter { !map.consumes(ControllerControl(device: p.name, element: .gamepad($0))) })
            }
            let had = sentMask[u] ?? 0
            guard mask != had, !unsupported.contains(u) else { continue }
            sentMask[u] = mask
            session(u).enqueue(.gamepad(mask))
        }
        ensureLoop()
    }

    private func moved(_ p: Plugged, _ e: ControlElement) {
        let label = e.label
        if lastMoved[p.id] != label { lastMoved[p.id] = label }
    }

    // MARK: the app's own mappings

    private func apply(_ outputs: [ControllerOutput]) {
        guard let rig else { return }
        for o in outputs {
            switch o {
            case let .knob(t, v): rig.module(unit: t.unit)?.set(fieldId: t.field, v)
            case let .padX(v): inputs?.controllerPad(x: v, y: nil)
            case let .padY(v): inputs?.controllerPad(x: nil, y: v)
            case let .shareKey(on):
                guard rig.canLink else { continue }
                let want = on ?? !rig.linking.shareKey
                if want != rig.linking.shareKey { rig.setShareKey(want) }
            case let .splitHarmonies(on):
                guard rig.canLink else { continue }
                rig.setSplit(on ?? !rig.linking.splitHarmonies)
            case let .press(unit, button, gesture):
                for m in modules(unit) { m.press(button: button, gesture) }
            case let .preset(unit, step):
                for m in modules(unit) { m.stepPreset(step) }
            case let .midiCC(t, v):
                midiOut.cc(t, v)
                ensureLoop()
            case let .midiNote(t, on):
                if let msg = midiOut.note(t, on: on) { sendOut(t.device, [msg]) }
            }
        }
    }

    private func modules(_ unit: UInt32?) -> [ModuleStore] {
        guard let rig else { return [] }
        if let unit { return rig.module(unit: unit).map { [$0] } ?? [] }
        return rig.ready
    }

    func beginLearn(inPlace: Bool = false) { learnInPlace = inPlace; map.beginLearn(); syncMap(save: false) }
    func cancelLearn() { learnInPlace = false; map.cancelLearn(); syncMap(save: false) }

    /// The app's own sources moved (from Rig.drive): their MIDI out.
    private func appSources(_ values: [ControlSource: Double]) {
        for (s, v) in values {
            let out = map.drive(s, value: v)
            if !out.isEmpty { apply(out) }
        }
    }

    /// To another MIDI device by name, on its first port.
    private func sendOut(_ device: String, _ messages: [[UInt8]]) {
        guard let p = plugged.values.first(where: { $0.name == device && $0.isOpen }),
              let port = p.ports.lazy.compactMap({ p.open[$0.id] }).first else { return }
        port.send(messages)
    }

    /// Devices MIDI can go out to: every MIDI controller plugged in, and
    /// any named in a mapping (it may be plugged in later).
    var outputDevices: [String] {
        let here = controllers.filter { $0.kind != .gamepad }.map(\.name)
        let named = mappings.compactMap { m -> String? in
            switch m.action {
            case let .midiCC(t), let .midiNote(t): return t.device
            default: return nil
            }
        }
        return Array(Set(here + named)).sorted()
    }

    func isPlugged(_ device: String) -> Bool { controllers.contains { $0.name == device } }

    func save(_ m: ControllerMapping) {
        map.assign(m)
        syncMap(save: true)
        updateMasks()
    }

    func remove(_ id: UUID) {
        map.remove(id)
        syncMap(save: true)
        updateMasks()
    }

    /// Every target of one control.
    func targets(for c: ControllerControl) -> [ControllerMapping] { mappings.filter { $0.control == c } }

    /// Finished adding targets to the control just learned.
    func finishLearn() { map.finishLearn(); syncMap(save: false) }

    func moduleName(_ unit: UInt32?) -> String {
        guard let unit else { return "both" }
        guard let rig, let m = rig.module(unit: unit) else { return "not connected" }
        return rig.name(m)
    }

    func label(_ a: ControllerAction) -> String {
        switch a {
        case let .knob(t): return rig?.label(t) ?? t.field
        case .padX: return "Pad X"
        case .padY: return "Pad Y"
        case .shareKey: return "Share key and scale"
        case .splitHarmonies: return "Split harmonies"
        case let .hard(u): return "HARD · \(moduleName(u))"
        case let .mute(u): return "Mute · \(moduleName(u))"
        case let .presetNext(u): return "Next preset · \(moduleName(u))"
        case let .presetPrevious(u): return "Previous preset · \(moduleName(u))"
        case let .midiCC(t): return "\(t.device) · CC \(t.number), channel \(t.channel + 1)"
        case let .midiNote(t): return "\(t.device) · note \(BeltTheory.name(Int(t.number))), channel \(t.channel + 1)"
        }
    }

    /// "Left · Retune, 20 to 60 %, latch" for the list.
    func label(_ m: ControllerMapping) -> String {
        var s = label(m.action)
        if m.action.takesRange && (m.lo != 0 || m.hi != 1) { s += ", \(Int(m.lo * 100)) to \(Int(m.hi * 100)) %" }
        if m.control.element.isButton && m.mode == .toggle && m.action.hasToggle { s += ", latch" }
        return s
    }

    // MARK: MIDI routing (the patchbay)

    /// Add or change a route; the reason when it's refused.
    func saveMIDIRoute(_ r: MIDIRoute) -> String? {
        do {
            deliver(try router.save(r))
        } catch RouteRefusal.loop {
            return "That route would lead back to where it starts (through the routes already there), so everything on it would echo forever."
        } catch {
            return "A gamepad can only send, not receive."
        }
        routesChanged()
        return nil
    }

    func removeMIDIRoute(_ id: UUID) {
        deliver(router.remove(id))
        routesChanged()
    }

    func setMIDIRoute(_ id: UUID, enabled: Bool) {
        deliver(router.setEnabled(id, enabled))
        routesChanged()
    }

    private func routesChanged() {
        if midiRoutes != router.routes { midiRoutes = router.routes }
        if let d = try? JSONEncoder().encode(router.routes), let s = String(data: d, encoding: .utf8) { midiRoutesRaw = s }
        // A device set to Off in Controls opens for a route, and closes when none needs it.
        for p in plugged.values where !p.isPad {
            if wantsOpen(p) && !p.isOpen { open(p) } else if !wantsOpen(p) && p.isOpen { close(p) }
        }
        ensureLoop()
    }

    private func syncNoNotes() {
        if noNotes != router.noNotes { noNotes = router.noNotes }
    }

    /// Notes and CCs to devices (by name, a few messages per packet list),
    /// notes and the pedal to modules as CTRL.
    private func deliver(_ outs: [RouteOutput]) {
        guard !outs.isEmpty else { return }
        var byDevice: [String: [[UInt8]]] = [:]
        var order: [String] = []
        for o in outs {
            switch o {
            case let .midi(device, bytes):
                if byDevice[device] == nil { order.append(device) }
                byDevice[device, default: []].append(bytes)
            case let .keyboard(unit, e):
                guard !unsupported.contains(unit), rig?.module(unit: unit)?.hasCtrl == true else { continue }
                session(unit).enqueue(e)
            }
        }
        for d in order { sendOut(d, byDevice[d] ?? []) }
        ensureLoop()
    }

    /// A module's STATE: Belt's sung note (and harmonies) as its MIDI out.
    private func moduleState(_ unit: UInt32, _ st: ModuleState) {
        var f = followers[unit] ?? BeltNoteFollower()
        let notes = f.update(st)
        followers[unit] = f
        if !notes.isEmpty { deliver(router.moduleNotes(unit: unit, notes)) }
    }

    /// A module's knobs moved: those routed as CCs go out at the control rate.
    private func moduleKnobs(_ unit: UInt32, _ changes: [String: Double]) {
        router.moduleKnobs(unit: unit, changes)
        if router.hasPending { ensureLoop() }
    }

    /// Everything that can send: the modules, then MIDI devices and
    /// gamepads, then any a route names that isn't here now.
    var routeSources: [RouteEndpoint] {
        var out: [RouteEndpoint] = (rig?.ready ?? []).map { .module($0.unit) }
        for c in controllers {
            let e: RouteEndpoint = c.kind == .gamepad ? .gamepad(c.name) : .device(c.name)
            if !out.contains(e) { out.append(e) }
        }
        for r in midiRoutes where !out.contains(r.from) { out.append(r.from) }
        return out
    }

    /// Everything that can receive: the modules (as keyboards) and MIDI devices.
    var routeDestinations: [RouteEndpoint] {
        var out: [RouteEndpoint] = (rig?.ready ?? []).map { .module($0.unit) }
        for c in controllers where c.kind != .gamepad && !out.contains(.device(c.name)) { out.append(.device(c.name)) }
        for r in midiRoutes where !out.contains(r.to) { out.append(r.to) }
        return out
    }

    func endpointName(_ e: RouteEndpoint) -> String {
        switch e {
        case let .device(n), let .gamepad(n): return n
        case let .module(u):
            guard let rig, let m = rig.module(unit: u) else { return "Module (not connected)" }
            return rig.name(m)
        }
    }

    func isHere(_ e: RouteEndpoint) -> Bool {
        switch e {
        case let .device(n): return plugged.values.contains { $0.name == n && !$0.isPad }
        case let .gamepad(n): return plugged.values.contains { $0.name == n && $0.isPad }
        case let .module(u): return rig?.module(unit: u) != nil
        }
    }

    func endpointIcon(_ e: RouteEndpoint) -> String {
        switch e {
        case .module: return "waveform"
        case .gamepad: return "gamecontroller"
        case let .device(n):
            switch controllers.first(where: { $0.name == n })?.kind {
            case .launchpad: return "square.grid.3x3.fill"
            case .launchControlXL: return "slider.vertical.3"
            default: return "pianokeys"
            }
        }
    }

    /// A line for the route's row: a problem (warn) or how it behaves.
    func routeNote(_ r: MIDIRoute) -> (text: String, warn: Bool)? {
        if !isHere(r.from) { return ("\(endpointName(r.from)) isn't connected.", true) }
        if !isHere(r.to) { return ("\(endpointName(r.to)) isn't connected.", true) }
        guard let u = r.to.unit else { return nil }
        if noNotes.contains(u) { return ("This module's firmware doesn't play notes yet.", true) }
        if unsupported.contains(u) || rig?.module(unit: u)?.hasCtrl != true {
            return ("This module's firmware has no controller support (CTRL), so it can't play notes.", true)
        }
        if case let .device(name) = r.from, r.effectiveFilter.contains(.cc) {
            let n = mappings.filter { $0.control.device == name && $0.action.unit == u }.count
            return (n > 0
                ? "CCs act only through your \(n) learned mapping\(n == 1 ? "" : "s") to this module; other CCs are dropped. CC 64 is its sustain pedal."
                : "CCs are dropped: none learned from \(name) for this module. CC 64 is its sustain pedal.", false)
        }
        if r.from.isModule { return ("Notes only, between modules: knob to knob is a follow on the Link tab.", false) }
        return nil
    }

    /// The module's knobs, for a route that sends them as CCs.
    func knobFields(_ unit: UInt32) -> [Field] {
        rig?.module(unit: unit)?.allFields.filter { !$0.severs } ?? []
    }

    // MARK: CTRL to the modules

    private func session(_ u: UInt32) -> CtrlSession {
        if let s = sessions[u] { return s }
        let s = CtrlSession()
        sessions[u] = s
        return s
    }

    private var forwarding: [(kind: ControllerKind, route: ControllerRoute)] {
        plugged.values.filter { $0.kind.forwards && ($0.isOpen || $0.isPad) }.map { ($0.kind, route($0)) }
    }

    private func ensureLoop() {
        guard loop == nil else { return }
        loop = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, self.tick() else { break }
                try? await Task.sleep(nanoseconds: 5_000_000)
            }
            self?.loop = nil
        }
    }

    /// One pass: a CTRL to each module that's due. False when there's
    /// nothing left to do (no controllers routed, every module told).
    private func tick() -> Bool {
        guard let rig else { return false }
        let ready = rig.ready
        let units = ready.map(\.unit)
        if units != lastUnits {
            // A module that went: the notes it played through routes are let go.
            for u in lastUnits where !units.contains(u) {
                followers[u] = nil
                deliver(router.gone(.module(u)))
            }
            syncNoNotes()
            lastUnits = units
            let gone = unsupported.filter { !units.contains($0) }
            if !gone.isEmpty { unsupported.subtract(gone) }
            sentMask = sentMask.filter { units.contains($0.key) }
            for p in plugged.values { refreshIdle(p) }
            updateMasks()
        }
        let routed = active ? forwarding : []
        let now = ProcessInfo.processInfo.systemUptime
        for (device, messages) in midiOut.flush(now: now) { sendOut(device, messages) }
        deliver(router.flush(now: now))
        for m in ready {
            let u = m.unit
            var present = ctrlPresent(for: u, controllers: routed, ready: units)
            if active && router.feedsKeyboard(unit: u) { present.insert(.keyboard) }
            if !m.hasCtrl || unsupported.contains(u) {
                if !present.isEmpty && !unsupported.contains(u) { markUnsupported(u) }
                sessions[u] = nil
                continue
            }
            if present.isEmpty {
                // Tell the module once that its controllers have gone.
                if let s = sessions[u], !s.inFlight {
                    sessions[u] = nil
                    sentMask[u] = nil
                    exchange(m, s, present: [], now: now)
                }
                continue
            }
            let s = session(u)
            if s.due(now: now, controlPeriod: m.kind.controlPeriod, statePeriod: m.kind.statePeriod) {
                exchange(m, s, present: present, now: now)
            }
        }
        for u in sessions.keys where !units.contains(u) { sessions[u] = nil }
        // While a module's MIDI out holds a note, keep watching for it going away.
        let moduleNotesOn = followers.values.contains { $0.sung != nil || !$0.harmony.isEmpty }
        return !sessions.isEmpty || !routed.isEmpty || midiOut.hasPending || router.hasPending || moduleNotesOn
    }

    private func exchange(_ m: ModuleStore, _ s: CtrlSession, present: CtrlPresent, now: Double) {
        let batch = s.begin(now: now, maxEvents: CtrlRequest.maxEvents(maxBody: m.maxBody))
        let unit = m.unit
        Task { [weak self] in
            do {
                let reply = try await m.ctrl(present: present, events: batch)
                s.end()
                guard let self else { return }
                if let reply {
                    self.router.ctrlReply(unit: unit, asked: present, seen: reply.presentSeen)
                    self.syncNoNotes()
                    self.absorb(reply, from: unit)
                } else {
                    self.markUnsupported(unit)
                }
            } catch {
                s.end(failed: batch)
            }
        }
    }

    private func markUnsupported(_ u: UInt32) {
        unsupported.insert(u)
        sessions[u] = nil
        for p in plugged.values { refreshIdle(p) }
    }

    /// The module's LEDs onto the controllers that show this module.
    private func absorb(_ r: CtrlReply, from unit: UInt32) {
        for p in plugged.values where p.isOpen && ledSource(p) == unit {
            switch p.kind {
            case .launchpad: showLEDs(p, r.lpLeds)
            case .launchControlXL: showLEDs(p, r.xlLeds)
            default: break
            }
        }
    }
}
