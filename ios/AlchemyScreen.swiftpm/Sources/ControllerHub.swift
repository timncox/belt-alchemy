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
    /// Modules (by unit) whose firmware answered UNSUPPORTED or lacks the CTRL flag.
    @Published private(set) var unsupported: Set<UInt32> = []
    /// The control last moved on each controller, for the list.
    @Published private(set) var lastMoved: [String: String] = [:]

    @AppStorage("controllerRoutes") private var routesRaw = ""
    @AppStorage("controllerMap") private var mapRaw = ""

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
        var open: [MIDIUniqueID: RawMIDIPort] = [:]
        var parsers: [MIDIUniqueID: MIDIStreamParser] = [:]
        var latch = PortLatch()
        var leds: LEDDiffer
        var gamepad: GCController?
        var held: Set<XInputButton> = []
        var extras: Set<String> = []

        init(id: String, name: String, kind: ControllerKind, ports: [MIDIDevice]) {
            self.id = id; self.name = name; self.kind = kind; self.ports = ports
            leds = LEDDiffer(count: kind == .launchControlXL ? LaunchControlXL.leds : LaunchpadMK3.cells)
        }
        var isOpen: Bool { !open.isEmpty }
    }

    private var plugged: [String: Plugged] = [:]
    private var sessions: [UInt32: CtrlSession] = [:]
    /// The gamepad mask last queued for each module.
    private var sentMask: [UInt32: UInt32] = [:]
    private var lastUnits: [UInt32] = []
    private var loop: Task<Void, Never>?
    /// CCs and notes to other MIDI devices.
    private let midiOut = MIDIOutQueue(period: LinkKind.usb.controlPeriod)

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
        reconcile(rig.devices)
        reconcilePads()
    }

    /// The app came forward (true) or went to the background (false).
    func setActive(_ on: Bool) {
        guard on != active else { return }
        active = on
        for p in plugged.values where p.isOpen { on ? start(p) : release(p) }
        if on { ensureLoop() }
    }

    private func load() {
        if let d = routesRaw.data(using: .utf8), let r = try? JSONDecoder().decode([String: ControllerRoute].self, from: d) {
            routes = r
        }
        if let d = mapRaw.data(using: .utf8), let m = try? JSONDecoder().decode(ControllerMap.self, from: d) { map = m }
        syncMap(save: false)
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
        for (id, p) in plugged where p.gamepad == nil {
            if let g = groups[id], Set(g.ports.map(\.id)) == Set(p.ports.map(\.id)) { continue }
            unplug(id)
        }
        for (id, g) in groups where plugged[id] == nil {
            let p = Plugged(id: id, name: g.name, kind: g.kind, ports: g.ports.sorted { $0.name < $1.name })
            plugged[id] = p
            if route(p) != .off { open(p) }
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
        guard p.gamepad == nil, !p.isOpen, let midi = rig?.midi else { return }
        let inbox = inbox, id = p.id
        for port in p.ports {
            let pid = port.id
            // On CoreMIDI's thread: queued in order, taken on the main actor.
            let raw = midi.connectRaw(port) { @Sendable [weak self] bytes in
                if inbox.push(.midi(controller: id, port: pid, bytes: bytes)) {
                    Task { @MainActor in self?.drain() }
                }
            }
            if let raw { p.open[port.id] = raw }
        }
        midiOut.forget(device: p.name)
        if active { start(p) }
        ensureLoop()
    }

    private func close(_ p: Plugged) {
        guard p.isOpen else { return }
        release(p)
        for id in p.open.keys { rig?.midi?.disconnectRaw(id) }
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
        for port in p.open.keys { rig?.midi?.disconnectRaw(port) }
        p.gamepad?.extendedGamepad?.valueChangedHandler = nil
        lastMoved[id] = nil
        updateMasks()
    }

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
        routes[c.name] = r
        if let d = try? JSONEncoder().encode(routes), let s = String(data: d, encoding: .utf8) { routesRaw = s }
        for p in plugged.values where p.name == c.name {
            if r == .off { close(p) } else if !p.isOpen { open(p) } else { refreshIdle(p) }
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
        guard p.isOpen, p.kind == .launchpad || p.kind == .launchControlXL, ledSource(p) == nil else { return }
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
        var parser = p.parsers[port] ?? MIDIStreamParser()
        var messages: [MIDIMessage] = []
        parser.feed(bytes) { messages.append($0) }
        p.parsers[port] = parser
        for m in messages {
            switch p.kind {
            case .launchpad:
                // Each press comes on both ports; the first port heard is the one used.
                guard let e = LaunchpadMK3.event(m), p.latch.accept(Int64(port)) else { continue }
                control(p, e)
            case .launchControlXL:
                if LaunchControlXL.templateChanged(m) {
                    send(p, [LaunchControlXL.selectTemplate])
                    p.leds.forget()
                    refreshIdle(p)
                    continue
                }
                if let e = LaunchControlXL.event(m) { control(p, e) }
            case .midi:
                guard let hit = ControlElement.from(midi: m) else { continue }
                let (element, v) = hit
                moved(p, element)
                if let out = map.handle(ControllerControl(device: p.name, element: element), value: v) { apply(out) }
            case .gamepad:
                break
            }
        }
    }

    /// A Launchpad or XL control: the app's mapping, else on to the module(s).
    private func control(_ p: Plugged, _ e: CtrlEvent) {
        guard let element = e.element else { return }
        moved(p, element)
        if let out = map.handle(ControllerControl(device: p.name, element: element), value: e.value) {
            apply(out)
            return
        }
        for u in route(p).units(readyUnits) where !unsupported.contains(u) { session(u).enqueue(e) }
    }

    private func padIn(_ id: String, held: Set<XInputButton>, extras: Set<String>) {
        guard let p = plugged[id], active, route(p) != .off else { return }
        for b in p.held.symmetricDifference(held).sorted(by: { $0.bit < $1.bit }) {
            moved(p, .gamepad(b))
            if let out = map.handle(ControllerControl(device: p.name, element: .gamepad(b)), value: held.contains(b) ? 1 : 0) {
                apply(out)
            }
        }
        for name in p.extras.symmetricDifference(extras).sorted() {
            moved(p, .gamepadExtra(name))
            if let out = map.handle(ControllerControl(device: p.name, element: .gamepadExtra(name)), value: extras.contains(name) ? 1 : 0) {
                apply(out)
            }
        }
        p.held = held
        p.extras = extras
        updateMasks()
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

    func beginLearn() { map.beginLearn(); syncMap(save: false) }
    func cancelLearn() { map.cancelLearn(); syncMap(save: false) }

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

    // MARK: CTRL to the modules

    private func session(_ u: UInt32) -> CtrlSession {
        if let s = sessions[u] { return s }
        let s = CtrlSession()
        sessions[u] = s
        return s
    }

    private var forwarding: [(kind: ControllerKind, route: ControllerRoute)] {
        plugged.values.filter { $0.kind.forwards && ($0.isOpen || $0.gamepad != nil) }.map { ($0.kind, route($0)) }
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
        for m in ready {
            let u = m.unit
            let present = ctrlPresent(for: u, controllers: routed, ready: units)
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
        return !sessions.isEmpty || !routed.isEmpty || midiOut.hasPending
    }

    private func exchange(_ m: ModuleStore, _ s: CtrlSession, present: CtrlPresent, now: Double) {
        let batch = s.begin(now: now, maxEvents: CtrlRequest.maxEvents(maxBody: m.maxBody))
        let unit = m.unit
        Task { [weak self] in
            do {
                let reply = try await m.ctrl(present: present, events: batch)
                s.end()
                guard let self else { return }
                if let reply { self.absorb(reply, from: unit) } else { self.markUnsupported(unit) }
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
