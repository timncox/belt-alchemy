// The modules the app is talking to, however they are reached: one on the
// USB-C port, two on a USB-C hub, two each on their own Bluetooth MIDI
// adapter, or the pretend ones in the demo. The same list, names, screens
// and controls whatever the link.
//
// A module is told apart by the unit number it reports (Phone INFO), which
// stays the same when it moves from the hub to an adapter. Its name in the
// app ("Left", "Right", or one Tim types) is kept against that number.

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

    private var hub: MIDIHub?
    private var trying: Set<MIDIUniqueID> = []
    private var declined: Set<MIDIUniqueID> = []

    var selected: ModuleStore? { modules.first { $0.id == selectedID } ?? modules.first }
    var ready: [ModuleStore] { modules.filter { $0.phase == .ready } }
    var isDemo: Bool { modules.contains { $0.isDemo } }

    init() {
        if let d = controlsRaw.data(using: .utf8), let m = try? JSONDecoder().decode(ControlMatrix.self, from: d) {
            controls = m
        }
        do {
            let h = try MIDIHub()
            h.onSetupChanged = { [weak self] in Task { @MainActor in self?.rescan() } }
            hub = h
        } catch {
            notice = String(describing: error)
        }
        rescan()
    }

    // MARK: finding modules

    private var known: Set<MIDIUniqueID> {
        get { Set(knownRaw.split(separator: ",").compactMap { MIDIUniqueID($0) }) }
        set { knownRaw = newValue.map(String.init).joined(separator: ",") }
    }

    /// Look at what's plugged in or paired: connect anything that is (or
    /// was before) an Alchemy Lab, and drop modules that went away.
    func rescan() {
        guard let hub else { return }
        devices = hub.devices()
        for m in modules where !m.isDemo {
            if let d = m.device, !hub.isPresent(d.id) { remove(m) }
        }
        for d in devices where d.looksLikeModule || known.contains(d.id) {
            guard !declined.contains(d.id), !modules.contains(where: { $0.device?.id == d.id }) else { continue }
            connect(d)
        }
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
        if selectedID == m.id { selectedID = modules.first?.id }
    }

    // MARK: the demo

    /// One or two pretend modules inside the app, for trying everything
    /// without hardware (and for the Simulator and App Review).
    func startDemo(count: Int) {
        stopDemo()
        for i in 0..<count {
            let fake = FakeModule(viaSysEx: true, unit: 0xBE17_0001 + UInt32(i), name: "Belt (demo)")
            let m = ModuleStore(device: nil, kind: .demo, transport: fake)
            m.onLost = { [weak self] m in self?.remove(m) }
            modules.append(m)
        }
        selectedID = modules.first?.id
    }

    func stopDemo() {
        for m in modules where m.isDemo { remove(m) }
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
    }

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
}
