// Between the extension's parameters and the modules: finds Alchemy Labs
// running Belt over CoreMIDI, puts the two lowest unit numbers in slots A
// and B (the app's Left and Right), sends the host's changes to the right
// module and reads every module back about every 400 ms.
//
// All of its own state lives on one serial queue; the HostLink work runs in
// tasks (BeltParamDriver, from AlchemyLink, which the tests run against the
// pretend module). Results come back to the main queue through onReadback
// and onStatus.
//
// Sharing a module with the app: each side has its own CoreMIDI connection
// and both hear every reply. The extension's requests go through
// SeqTagTransport, which gives them sequence numbers with the top bit set
// and drops replies that aren't its own; the app's client numbers from 1
// and ignores ours. Polling doubles while both are open (the app asks for
// STATE up to 15 times a second; the extension reads the knobs and STATE
// every 400 ms), and a write of the running state from both at the same
// instant can fail once and be retried. Close the app's connection (or the
// app) while a host is using the extension. See ios/PLAN.md, "AUv3".

import CoreMIDI
import Foundation
import AlchemyLink

struct SlotStatus: Equatable {
    enum State: Equatable { case empty, connecting, ready, failed(String) }
    var state: State = .empty
    /// The module's name (HELLO), or the MIDI device's until it answers.
    var name = ""
    /// The MIDI device and how it's reached: "Alchemy Lab · USB".
    var detail = ""
}

final class ModuleBridge: @unchecked Sendable {
    private final class Entry {
        let device: AUMIDIDevice
        let driver: BeltParamDriver
        var ready = false
        var reading = false
        var misses = 0
        init(device: AUMIDIDevice, driver: BeltParamDriver) { self.device = device; self.driver = driver }
    }

    static let readEvery = 0.4
    /// A device that didn't answer (or isn't Belt) is tried again after this.
    static let retryAfter = 5.0

    private let q = DispatchQueue(label: "alchemy.au.modules")
    private var hub: AUMIDIHub?
    private var entries: [MIDIUniqueID: Entry] = [:]
    private var refused: [MIDIUniqueID: (name: String, why: String, at: Double)] = [:]
    private var sync = AUParamSync(hold: 1.0)
    private var timer: DispatchSourceTimer?
    private var running = false
    private var tick = 0
    private var lastStatus: [SlotStatus] = []
    /// Main thread only: the MIDI client has been made (once per bridge).
    private var madeHub = false

    /// Values read from the modules that the host isn't moving (main queue).
    var onReadback: (([UInt64: Float]) -> Void)?
    /// Slot A and B, whenever either changes (main queue).
    var onStatus: (([SlotStatus]) -> Void)?
    /// What a parameter holds now; read on the bridge's queue.
    var currentValue: ((UInt64) -> Float?)?
    /// Why CoreMIDI couldn't be opened, if it couldn't.
    private(set) var midiError: String?

    private static func now() -> Double { ProcessInfo.processInfo.systemUptime }

    // MARK: running

    /// Opens CoreMIDI and starts looking. Safe to call more than once and
    /// from any thread. The MIDI client is made on the main thread, whose
    /// run loop delivers CoreMIDI's setup notifications.
    func start() {
        DispatchQueue.main.async { [self] in
            var h: AUMIDIHub?
            var err: String?
            if !madeHub {
                madeHub = true
                do { h = try AUMIDIHub() } catch { err = String(describing: error) }
                h?.onSetupChanged = { [weak self] in self?.q.async { self?.rescan() } }
            }
            q.async { [self] in
                if let h { hub = h }
                if let err { midiError = err }
                guard !running else { return }
                running = true
                rescan()
                let t = DispatchSource.makeTimerSource(queue: q)
                t.schedule(deadline: .now() + Self.readEvery, repeating: Self.readEvery, leeway: .milliseconds(40))
                t.setEventHandler { [weak self] in self?.poll() }
                t.resume()
                timer = t
                publish()
            }
        }
    }

    /// Lets go of the modules (the MIDI client stays until the bridge goes).
    func stop() {
        q.async { [self] in
            running = false
            timer?.cancel(); timer = nil
            for id in entries.keys { hub?.disconnect(id) }
            entries = [:]
            refused = [:]
            publish()
        }
    }

    // MARK: the host's changes

    /// A parameter the host (automation, a CC, the extension's own view)
    /// changed. Any thread.
    func hostSet(_ address: UInt64, _ value: Float) {
        let t = Self.now()
        q.async { [self] in
            sync.hostChanged(address, at: t)
            guard let at = AUParams.decode(address) else { return }
            let o = ordered()
            // No module in that slot: the value stays on the parameter. When
            // one arrives its own state wins (it may be mid-song), and the
            // next read shows it.
            guard at.slot < o.count else { return }
            o[at.slot].driver.submit(param: at.param, value: value)
        }
    }

    // MARK: finding modules

    /// Ready modules in slot order: lowest unit first, like Left and Right.
    private func ordered() -> [Entry] {
        let ready = entries.values.filter { $0.ready }.sorted { ($0.driver.unit, $0.device.id) < ($1.driver.unit, $1.device.id) }
        return Array(ready.prefix(AUParams.slots))
    }

    private func rescan() {
        guard running, let hub else { publish(); return }
        let t = Self.now()
        for id in Array(entries.keys) where !hub.isPresent(id) { drop(id) }
        for id in Array(refused.keys) where !hub.isPresent(id) { refused[id] = nil }
        for d in hub.modules() where entries[d.id] == nil {
            if let r = refused[d.id], t - r.at < Self.retryAfter { continue }
            connect(d)
        }
        publish()
    }

    private func connect(_ d: AUMIDIDevice) {
        guard let hub, let link = hub.connect(d) else { return }
        refused[d.id] = nil
        let client = HostLinkClient(transport: SeqTagTransport(link))
        let driver = BeltParamDriver(client: client, controlPeriod: d.kind.controlPeriod)
        let e = Entry(device: d, driver: driver)
        entries[d.id] = e
        Task { [weak self] in
            do {
                try await driver.start()
                self?.q.async { [weak self] in
                    guard let self, self.entries[d.id] === e else { return }
                    e.ready = true
                    self.publish()
                }
            } catch {
                let why: String
                if let n = error as? BeltParamDriver.DriverError { why = n.description }
                else { why = "No answer. Is its USB port setting on Phone?" }
                self?.q.async { [weak self] in
                    guard let self, self.entries[d.id] === e else { return }
                    self.refuse(d, why)
                }
            }
        }
    }

    private func drop(_ id: MIDIUniqueID) {
        entries[id] = nil
        hub?.disconnect(id)
    }

    private func refuse(_ d: AUMIDIDevice, _ why: String) {
        drop(d.id)
        refused[d.id] = (d.name, why, Self.now())
        publish()
    }

    // MARK: reading back

    private func poll() {
        guard running else { return }
        tick += 1
        // Every few seconds, try devices that didn't answer before.
        if tick % Int(Self.retryAfter / Self.readEvery) == 0 { rescan(); sync.prune(at: Self.now()) }
        for e in ordered() where !e.reading {
            e.reading = true
            Task { [weak self] in
                let values = try? await e.driver.read()
                self?.q.async { [weak self] in self?.absorb(e, values) }
            }
        }
    }

    private func absorb(_ e: Entry, _ values: [Int: Float]?) {
        e.reading = false
        guard entries[e.device.id] === e else { return }
        guard let values else {
            e.misses += 1
            if e.misses >= 5 { refuse(e.device, "Stopped answering.") }
            return
        }
        e.misses = 0
        // The slot is worked out now, not when the read began: a module
        // that arrived meanwhile may have moved this one to B.
        guard let slot = ordered().firstIndex(where: { $0 === e }) else { return }
        var read: [UInt64: Float] = [:]
        for (p, v) in values { read[AUParams.address(slot: slot, param: p)] = v }
        let current = currentValue
        let apply = sync.readback(read, current: { current?($0) }, at: Self.now())
        guard !apply.isEmpty else { return }
        DispatchQueue.main.async { [weak self] in self?.onReadback?(apply) }
    }

    // MARK: status

    private func publish() {
        var out = [SlotStatus(), SlotStatus()]
        for (i, e) in ordered().enumerated() {
            out[i] = SlotStatus(state: .ready, name: e.driver.hello?.moduleName ?? e.device.name,
                                detail: "\(e.device.name) · \(e.device.kind.label)")
        }
        for e in entries.values where !e.ready {
            guard let i = out.firstIndex(where: { $0.state == .empty }) else { break }
            out[i] = SlotStatus(state: .connecting, name: e.device.name, detail: e.device.kind.label)
        }
        for r in refused.values {
            guard let i = out.firstIndex(where: { $0.state == .empty }) else { break }
            out[i] = SlotStatus(state: .failed(r.why), name: r.name, detail: "")
        }
        if let err = midiError, out[0].state == .empty {
            out[0] = SlotStatus(state: .failed(err), name: "MIDI", detail: "")
        }
        guard out != lastStatus else { return }
        lastStatus = out
        DispatchQueue.main.async { [weak self] in self?.onStatus?(out) }
    }
}
