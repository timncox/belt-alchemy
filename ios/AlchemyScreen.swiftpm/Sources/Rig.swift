// The modules the app is talking to, however they are reached: one on the
// USB-C port, two on a USB-C hub, two each on their own Bluetooth MIDI
// adapter, or the pretend ones in the demo. The same list, names, screens
// and controls whatever the link.
//
// A module is told apart by the unit number it reports (Phone INFO), which
// stays the same when it moves from the hub to an adapter. Its name in the
// app ("Left", "Right", or one Tim types) is kept against that number.
//
// Without hardware: the demo (pretend modules inside the app), or the
// module emulator on a Mac over TCP (EmulatorLink), each with the
// on-screen controllers (VirtualDevices) if they're switched on.

import CoreMIDI
import Foundation
import SwiftUI
import AlchemyLink

@MainActor
final class Rig: ObservableObject {
    @Published private(set) var modules: [ModuleStore] = []
    @Published private(set) var devices: [MIDIDevice] = []
    @Published var selectedID: String?
    @Published var notice: String?
    @Published var controls = ControlMatrix() { didSet { saveControls() } }
    /// Devices that answered as an Alchemy Lab before though their name
    /// doesn't say so (a renamed Bluetooth adapter): connected on sight.
    @AppStorage("knownDevices") private var knownRaw = ""
    @AppStorage("nicknames") private var nicknamesRaw = ""
    @AppStorage("controls") private var controlsRaw = ""
    @AppStorage("linking") private var linkingRaw = ""
    /// How the two modules are linked (shared key, follows, split harmonies).
    @Published private(set) var linking = LinkSettings() { didSet { saveLinking() } }

    /// The on-screen Launchpad, XL, gamepad and Daisy Patch.
    let virtual: VirtualDevices
    /// Show them in the demo and with the emulator (Controls > On-screen devices).
    @Published var onScreenDevices = true {
        didSet { UserDefaults.standard.set(onScreenDevices, forKey: "onScreenDevices"); updateVirtual() }
    }
    /// Connected to (or trying) the module emulators.
    @Published private(set) var emulatorOn = false
    @Published private(set) var emulator = EmulatorSettings()
    /// Each emulator port's state, for the Connect screen.
    @Published private(set) var emulatorStatus: [UInt16: String] = [:]
    private var endpoints: [EmulatorEndpoint] = []
    private var emulatorModules: [UInt16: ModuleStore] = [:]

    private var hub: MIDIHub?
    private var trying: Set<MIDIUniqueID> = []
    private var declined: Set<MIDIUniqueID> = []

    var selected: ModuleStore? { modules.first { $0.id == selectedID } ?? modules.first }
    /// CoreMIDI, for the controllers on the hub (ControllerHub).
    var midi: MIDIHub? { hub }
    var ready: [ModuleStore] { modules.filter { $0.phase == .ready } }
    var isDemo: Bool { modules.contains { $0.isDemo } }
    var isEmulator: Bool { emulatorOn }
    /// The demo or the emulator: where the on-screen devices can be on.
    var withoutHardware: Bool { isDemo || emulatorOn }
    /// The on-screen devices are showing.
    var virtualOn: Bool { onScreenDevices && withoutHardware }

    init() {
        virtual = VirtualDevices()
        if let d = controlsRaw.data(using: .utf8), let m = try? JSONDecoder().decode(ControlMatrix.self, from: d) {
            controls = m
        }
        if let d = linkingRaw.data(using: .utf8), let l = try? JSONDecoder().decode(LinkSettings.self, from: d) {
            linking = l
        }
        let defaults = UserDefaults.standard
        onScreenDevices = defaults.object(forKey: "onScreenDevices") as? Bool ?? true
        if let d = defaults.data(forKey: "emulator"), let e = try? JSONDecoder().decode(EmulatorSettings.self, from: d) {
            emulator = e
        }
        virtual.onDevicesChanged = { [weak self] in self?.listDevices() }
        do {
            let h = try MIDIHub()
            h.onSetupChanged = { [weak self] in Task { @MainActor in self?.rescan() } }
            hub = h
        } catch {
            notice = String(describing: error)
        }
        rescan()
        // Back where it was: the emulator again if it was on when the app quit.
        if defaults.bool(forKey: "emulatorMode") { startEmulator() }
    }

    // MARK: finding modules

    private var known: Set<MIDIUniqueID> {
        get { Set(knownRaw.split(separator: ",").compactMap { MIDIUniqueID($0) }) }
        set { knownRaw = newValue.map(String.init).joined(separator: ",") }
    }

    /// Look at what's plugged in or paired: connect anything that is (or
    /// was before) an Alchemy Lab, and drop modules that went away.
    func rescan() {
        listDevices()
        guard let hub else { return }
        for m in modules where !m.isDemo {
            if let d = m.device, !hub.isPresent(d.id) { remove(m) }
        }
        for d in devices where !d.isVirtual && (d.looksLikeModule || known.contains(d.id)) {
            guard !declined.contains(d.id), !modules.contains(where: { $0.device?.id == d.id }) else { continue }
            connect(d)
        }
    }

    /// CoreMIDI's devices and the on-screen ones, as one list.
    private func listDevices() {
        let list = (hub?.devices() ?? []) + virtual.midiDevices
        if list != devices { devices = list }
    }

    /// A plain MIDI port to a controller (ControllerHub): CoreMIDI's, or an
    /// on-screen device's. `receive` may be called on any thread.
    func connectRaw(_ d: MIDIDevice, receive: @escaping @Sendable ([UInt8]) -> Void) -> MIDIPortOut? {
        if d.isVirtual { return virtual.connect(d, receive: receive) }
        return hub?.connectRaw(d, receive: receive)
    }

    func disconnectRaw(_ id: MIDIUniqueID) {
        if virtual.owns(id) { virtual.disconnect(id) } else { hub?.disconnectRaw(id) }
    }

    func isConnected(_ d: MIDIDevice) -> Bool { modules.contains { $0.device?.id == d.id } }

    /// Try a device as a module (a Bluetooth adapter shows the adapter's
    /// name, not the module's). If it answers, it is remembered.
    func connect(_ d: MIDIDevice) {
        guard let hub, !trying.contains(d.id), !isConnected(d) else { return }
        guard let link = hub.connect(d) else { notice = "Couldn't open \(d.name)."; return }
        if modules.contains(where: { $0.isDemo }) { stopDemo() }
        declined.remove(d.id)
        let m = ModuleStore(device: d, kind: d.kind, transport: link)
        m.onLost = { [weak self] m in self?.lost(m) }
        m.onChange = { [weak self] m, c in self?.moduleChanged(m, c) }
        m.onState = { [weak self] m, st in self?.onModuleState?(m.unit, st) }
        modules.append(m)
        trying.insert(d.id)
        if selectedID == nil { selectedID = m.id }
        watchReady(m)
    }

    private func watchReady(_ m: ModuleStore) {
        Task { [weak self] in
            // Wait for INFO, then remember the device and sort by name.
            for _ in 0..<100 {
                if m.phase != .connecting { break }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            guard let self else { return }
            if let d = m.device { self.trying.remove(d.id) }
            if m.phase == .ready, let d = m.device { self.known.insert(d.id) }
            self.sortModules()
            self.applyLinksOnJoin()
        }
    }

    private func lost(_ m: ModuleStore) {
        if let d = m.device { trying.remove(d.id) }
        // A module that never answered is set aside until it's tried again
        // by hand; one that answered before will be picked up on replug.
        if m.hello == nil, let d = m.device {
            declined.insert(d.id)
            if !d.looksLikeModule { notice = "\(d.name) didn't answer as an Alchemy Lab." }
        }
        remove(m)
    }

    func disconnect(_ m: ModuleStore) {
        if let d = m.device { declined.insert(d.id) }
        remove(m)
    }

    func forget(_ d: MIDIDevice) { known.remove(d.id) }

    private func remove(_ m: ModuleStore) {
        m.stop()
        if let d = m.device { hub?.disconnect(d.id) }
        modules.removeAll { $0 === m }
        for (p, e) in emulatorModules where e === m { emulatorModules[p] = nil }
        if selectedID == m.id { selectedID = modules.first?.id }
        updateVirtual()
    }

    // MARK: the demo

    /// One or two pretend modules inside the app, for trying everything
    /// without hardware (and for the Simulator and App Review).
    func startDemo(count: Int) {
        stopDemo()
        stopEmulator()
        // Two have distinct unit numbers (BE170001, BE170002): Left and Right.
        for i in 0..<count {
            let fake = FakeModule(viaSysEx: true, unit: 0xBE17_0001 + UInt32(i), name: "Belt (demo)")
            let m = ModuleStore(device: nil, kind: .demo, transport: fake)
            m.onLost = { [weak self] m in self?.remove(m) }
            m.onChange = { [weak self] m, c in self?.moduleChanged(m, c) }
            m.onState = { [weak self] m, st in self?.onModuleState?(m.unit, st) }
            modules.append(m)
        }
        selectedID = modules.first?.id
        updateVirtual()
    }

    func stopDemo() {
        for m in modules where m.isDemo { remove(m) }
    }

    // MARK: the module emulator

    /// Connect to the module emulators (Left and Right ports), and keep trying.
    func startEmulator() {
        stopDemo()
        stopEndpoints()
        emulatorOn = true
        UserDefaults.standard.set(true, forKey: "emulatorMode")
        for port in emulator.ports {
            let ep = EmulatorEndpoint(host: emulator.host, port: port)
            ep.onStatus = { [weak self] st in Task { @MainActor in self?.emulatorStatus[port] = st.label } }
            ep.onConnected = { [weak self, weak ep] link in
                Task { @MainActor in if let self, let ep { self.emulatorConnected(ep, link) } }
            }
            ep.onDisconnected = { [weak self] in Task { @MainActor in self?.emulatorDropped(port) } }
            endpoints.append(ep)
            ep.start()
        }
        updateVirtual()
    }

    func stopEmulator() {
        guard emulatorOn || !endpoints.isEmpty else { return }
        stopEndpoints()
        emulatorOn = false
        UserDefaults.standard.set(false, forKey: "emulatorMode")
        emulatorStatus = [:]
        updateVirtual()
    }

    private func stopEndpoints() {
        for e in endpoints { e.stop() }
        endpoints = []
        for m in modules where m.kind == .emulator { remove(m) }
        emulatorModules = [:]
    }

    /// New host or ports: saved, and the emulator reconnected if it's on.
    func setEmulator(_ e: EmulatorSettings) {
        guard e != emulator else { return }
        emulator = e
        if let d = try? JSONEncoder().encode(e) { UserDefaults.standard.set(d, forKey: "emulator") }
        if emulatorOn { startEmulator() }
    }

    private func emulatorConnected(_ ep: EmulatorEndpoint, _ link: MIDIStreamLink) {
        guard endpoints.contains(where: { $0 === ep }) else { return }
        if let old = emulatorModules[ep.port] { remove(old) }
        let m = ModuleStore(device: nil, kind: .emulator, transport: link)
        // Stopped answering on an open socket: hang up and try again.
        m.onLost = { [weak self, weak ep] m in self?.remove(m); ep?.restart() }
        m.onChange = { [weak self] m, c in self?.moduleChanged(m, c) }
        m.onState = { [weak self] m, st in self?.onModuleState?(m.unit, st) }
        modules.append(m)
        emulatorModules[ep.port] = m
        if selectedID == nil { selectedID = m.id }
        watchReady(m)
        updateVirtual()
    }

    private func emulatorDropped(_ port: UInt16) {
        if let m = emulatorModules[port] { remove(m) }
    }

    // MARK: on-screen devices

    private func updateVirtual() {
        virtual.setOn(virtualOn)
    }

    // MARK: names

    private var nicknames: [String: String] {
        get { (try? JSONDecoder().decode([String: String].self, from: Data(nicknamesRaw.utf8))) ?? [:] }
        set { nicknamesRaw = (try? String(data: JSONEncoder().encode(newValue), encoding: .utf8)) ?? "" }
    }

    /// "Left" and "Right" by order (lowest unit first, so the same module
    /// keeps its name), unless renamed.
    func name(_ m: ModuleStore) -> String {
        if let n = nicknames[String(m.unit)], !n.isEmpty { return n }
        if modules.count < 2 { return m.hello?.moduleName ?? m.device?.name ?? "Alchemy Lab" }
        let i = modules.firstIndex { $0 === m } ?? 0
        return i == 0 ? "Left" : i == 1 ? "Right" : "Module \(i + 1)"
    }

    func rename(_ m: ModuleStore, to n: String) {
        var all = nicknames
        all[String(m.unit)] = n.trimmingCharacters(in: .whitespaces)
        nicknames = all
        objectWillChange.send()
    }

    private func sortModules() {
        modules.sort { ($0.unit, $0.id) < ($1.unit, $1.id) }
    }

    func module(unit: UInt32) -> ModuleStore? { modules.first { $0.unit == unit && $0.phase == .ready } }

    // MARK: controls (pad, pressure, tilt, mic)

    /// New values from any of the app's controls: each routed knob moves.
    func drive(_ inputs: [ControlSource: Double]) {
        for (t, v) in controls.apply(inputs) {
            module(unit: t.unit)?.set(fieldId: t.field, v)
        }
        onDrive?(inputs)
    }

    /// The same values for MIDI out to other devices (ControllerHub).
    var onDrive: (([ControlSource: Double]) -> Void)?

    func route(_ s: ControlSource, to t: ControlTarget?) {
        var c = controls
        c.routes.removeAll { $0.source == s }
        if let t { c.routes.append(ControlRoute(source: s, target: t)) }
        controls = c
    }

    func setRange(_ s: ControlSource, lo: Double, hi: Double) {
        var c = controls
        for i in c.routes.indices where c.routes[i].source == s { c.routes[i].lo = lo; c.routes[i].hi = hi }
        controls = c
    }

    func target(of s: ControlSource) -> ControlRoute? { controls.routes.first { $0.source == s } }

    /// "Left · Retune" for a route's knob.
    func label(_ t: ControlTarget) -> String {
        guard let m = module(unit: t.unit) else { return "Not connected" }
        let f = m.desc?.field(t.field)?.name ?? t.field
        return modules.count > 1 ? "\(name(m)) · \(f)" : f
    }

    private func saveControls() {
        if let d = try? JSONEncoder().encode(controls), let s = String(data: d, encoding: .utf8) { controlsRaw = s }
    }

    // MARK: linking the two modules

    /// The linked modules, left first (lowest unit first, as named).
    var linkUnits: [UInt32] { ready.map(\.unit) }
    var canLink: Bool { ready.count >= 2 }

    /// True while the Rig itself is writing a linked knob, so that write
    /// isn't copied back to the module it came from (which would fight a
    /// hand still on the pot).
    private var applyingLinks = false

    /// Knob moves and STATE reads, for the module's MIDI out (ControllerHub's routing).
    var onModuleKnobs: ((UInt32, [String: Double]) -> Void)?
    var onModuleState: ((UInt32, ModuleState) -> Void)?

    private func moduleChanged(_ m: ModuleStore, _ changes: [String: Double]) {
        if m.phase == .ready { onModuleKnobs?(m.unit, changes) }
        guard canLink, !applyingLinks else { return }
        let units = linkUnits
        for (field, v) in changes {
            if linking.splitHarmonies, let slot = LinkEngine.planSlot(unit: m.unit, field: field, units: units) {
                let idx = BeltFields.voiceIndex(v)
                if linking.plan[slot] != idx { linking.plan[slot] = idx }
            }
            write(LinkEngine.propagate(unit: m.unit, field: field, value: v, settings: linking, units: units))
        }
    }

    private func write(_ w: [ControlTarget: Double]) {
        applyingLinks = true
        defer { applyingLinks = false }
        for (t, v) in w { module(unit: t.unit)?.set(fieldId: t.field, v) }
    }

    /// Left's key and scale onto the others.
    private func copyKey() {
        guard let left = ready.first else { return }
        for f in [BeltFields.key, BeltFields.scale] {
            guard let v = left.values[f] else { continue }
            write(Dictionary(uniqueKeysWithValues: ready.dropFirst().map { (ControlTarget(unit: $0.unit, field: f), v) }))
        }
    }

    func setShareKey(_ on: Bool) {
        linking.shareKey = on
        if on { copyKey() }
    }

    func setSplit(_ on: Bool) {
        guard on != linking.splitHarmonies else { return }
        if on, let left = ready.first {
            linking.plan = LinkEngine.plan(fromLeftVoices: BeltFields.voices.map { left.values[$0] ?? BeltFields.voiceOff })
        }
        linking.splitHarmonies = on
        if on { copyKey(); write(LinkEngine.splitWrites(plan: linking.plan, units: linkUnits)) }
        else if canLink {
            // Back to all four voices on the left; the right keeps its two.
            write([ControlTarget(unit: ready[0].unit, field: BeltFields.voices[2]): BeltFields.voiceValue(linking.plan[2]),
                   ControlTarget(unit: ready[0].unit, field: BeltFields.voices[3]): BeltFields.voiceValue(linking.plan[3])])
        }
    }

    func setPlan(_ slot: Int, _ interval: Int) {
        guard linking.plan.indices.contains(slot) else { return }
        linking.plan[slot] = interval
        if linking.splitHarmonies { write(LinkEngine.splitWrites(plan: linking.plan, units: linkUnits)) }
    }

    /// Add "this knob drives that one"; refused if it would bounce back.
    @discardableResult
    func addFollow(_ f: Follow) -> Bool {
        guard !LinkEngine.makesLoop(f, with: linking.follows) else {
            notice = "That would link a knob back to itself through the other module."
            return false
        }
        linking.follows.append(f)
        if let v = module(unit: f.from.unit)?.values[f.from.field] {
            write(LinkEngine.propagate(unit: f.from.unit, field: f.from.field, value: v,
                                       settings: LinkSettings.only(f), units: linkUnits))
        }
        return true
    }

    func removeFollow(_ id: UUID) { linking.follows.removeAll { $0.id == id } }

    /// A second module came in: bring it into line with what's linked.
    private func applyLinksOnJoin() {
        guard canLink else { return }
        if linking.keyShared { copyKey() }
        if linking.splitHarmonies { write(LinkEngine.splitWrites(plan: linking.plan, units: linkUnits)) }
    }

    private func saveLinking() {
        if let d = try? JSONEncoder().encode(linking), let s = String(data: d, encoding: .utf8) { linkingRaw = s }
    }
}

extension LinkSettings {
    static func only(_ f: Follow) -> LinkSettings { var s = LinkSettings(); s.follows = [f]; return s }
}
