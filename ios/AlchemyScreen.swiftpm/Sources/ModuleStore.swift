// One module, kept in step: its own state for the app's screen (or the LED
// screen, for firmwares without STATE), polled as often as its link allows,
// the knob values (every 400 ms, like the web Control page), presets, the
// card's files and firmware switching. The Rig holds one of these for each
// module connected, over any link.
// Edits are written to the running state as you turn, coalesced so a fast
// drag never queues up a backlog.

import Foundation
import SwiftUI
import AlchemyLink

struct FirmwareFile: Identifiable {
    var id: String { name }
    var name: String
    var size: UInt32
}

@MainActor
final class ModuleStore: ObservableObject, Identifiable {
    enum Phase: Equatable { case connecting, ready, failed(String) }

    /// Which module this is: its unit number once INFO has answered (the
    /// same over USB, a hub or Bluetooth), the MIDI device until then.
    nonisolated let id: String
    let device: MIDIDevice?
    let kind: LinkKind
    @Published private(set) var phase: Phase = .connecting
    @Published private(set) var unit: UInt32 = 0

    @Published private(set) var hello: Hello?
    @Published private(set) var desc: Descriptor?
    @Published private(set) var phone: PhoneInfo?
    @Published private(set) var values: [String: Double] = [:]
    @Published private(set) var screen: [UInt8] = []
    @Published var pageIndex = 0
    @Published private(set) var slots: [SlotInfo] = []

    /// The module's own state (Belt: the sung note, harmonies, buttons),
    /// for firmwares that answer STATE; the app draws its screen from it.
    @Published private(set) var state: ModuleState?
    /// The sung pitch for the last ~5 s (MIDI notes, nil for silence),
    /// oldest first, at the module's 60 frames a second.
    @Published private(set) var trace: [Double?] = []
    static let traceLength = 300
    private var traceLast: UInt16?

    @Published private(set) var fs: FsInfo?
    @Published private(set) var fsAvailable = false
    @Published private(set) var cwd = "/"
    @Published private(set) var entries: [FsEntry] = []
    @Published private(set) var transfer: (name: String, progress: Double, rate: Double)?
    /// A transfer that will be slow on this link, waiting for an OK.
    @Published var slowTransfer: (urls: [URL], folder: String?, seconds: Int, firmware: Bool)?

    @Published private(set) var firmwares: [FirmwareFile] = []
    @Published private(set) var fw: FwStatus?
    @Published var notice: String?

    /// Called once when the module stops answering or is unplugged.
    var onLost: ((ModuleStore) -> Void)?

    private var client: HostLinkClient?
    private var live: [UInt8] = []
    private var dirty: [String: Double] = [:]
    private var flushing = false
    private var pollTask: Task<Void, Never>?
    private var fwTask: Task<Void, Never>?
    private var transferring = false

    var pages: [ControlPage] { desc?.pages ?? [] }
    var page: ControlPage? { pages.indices.contains(pageIndex) ? pages[pageIndex] : nil }
    /// Knob pages laid out like the panel; the LED screen edits these.
    var gridPages: [ControlPage] { pages.filter { $0.grid && !$0.isSettings } }
    var isDemo: Bool { kind == .demo }
    /// Every knob, for the pad and the other controls to pick from.
    var allFields: [Field] { gridPages.flatMap { $0.fields } }

    init(device: MIDIDevice?, kind: LinkKind, transport: ByteTransport) {
        self.device = device
        self.kind = kind
        self.id = device.map { "midi:\($0.id)" } ?? "demo:\(UUID().uuidString)"
        start(HostLinkClient(transport: transport))
    }

    func stop() {
        pollTask?.cancel(); pollTask = nil
        fwTask?.cancel(); fwTask = nil
        client = nil
    }

    private func lost(_ why: String) {
        guard client != nil else { return }
        stop()
        phase = .failed(why)
        onLost?(self)
    }

    private func start(_ c: HostLinkClient) {
        client = c
        pollTask = Task { [weak self] in
            guard let self else { return }
            do {
                // A Bluetooth adapter may be idle for a moment after pairing:
                // a couple of tries before giving up on it.
                var h: Hello?
                for attempt in 0..<3 where h == nil {
                    do { h = try await c.sayHello() }
                    catch { if attempt == 2 { throw error }; try? await Task.sleep(nanoseconds: 300_000_000) }
                }
                let d = try await c.descriptor()
                let p = try await c.phoneInfo()
                let bytes = try await c.getLive()
                self.hello = h; self.desc = d; self.phone = p
                self.unit = p?.unit ?? 0
                self.live = bytes; self.values = d.decode(bytes)
                self.pageIndex = 0
                self.phase = .ready
                self.fsAvailable = d.hasStorage
                await self.refreshSlots()
            } catch {
                self.phase = .failed("No answer. Is the module's USB port setting on Phone? (\(error))")
                self.onLost?(self)
                return
            }
            await self.pollLoop(c)
        }
    }

    private func pollLoop(_ c: HostLinkClient) async {
        var tick = 0
        var misses = 0
        let period = kind.statePeriod
        // Knob values are read back about every 400 ms, like the web page.
        let liveEvery = max(1, Int((0.4 / period).rounded()))
        while !Task.isCancelled {
            do {
                if !transferring {
                    if phone?.hasState == true {
                        let st = try await c.state(since: traceLast)
                        absorb(st)
                    } else if phone?.hasScreen == true {
                        screen = try await c.screen().rgb
                    }
                }
                if tick % liveEvery == 0 && dirty.isEmpty && !flushing && !transferring {
                    let b = try await c.getLive()
                    if dirty.isEmpty && !flushing, let d = desc { live = b; values = d.decode(b) }
                }
                misses = 0
            } catch {
                misses += 1
                if misses >= 5 { lost("Lost the module."); return }
            }
            tick += 1
            try? await Task.sleep(nanoseconds: UInt64((transferring ? 0.4 : period) * 1e9))
        }
    }

    private func absorb(_ st: ModuleState) {
        state = st
        guard let b = st.belt else { return }
        // A gap (a stall, or a link that fell behind) is drawn as silence.
        if let last = traceLast {
            let gap = Int(b.traceLast &- last) - b.trace.count
            if gap > 0 { trace += Array(repeating: nil, count: min(gap, Self.traceLength)) }
        }
        trace += b.trace
        if trace.count > Self.traceLength { trace.removeFirst(trace.count - Self.traceLength) }
        traceLast = b.traceLast
    }

    /// The field Belt's panel last saw a hand on, and how long ago.
    var touchedField: Field? {
        guard let st = state, let pg = st.touchPage, let pot = st.touchPot,
              let age = st.touchAge, age < 1.5 else { return nil }
        let grid = gridPages
        return grid.indices.contains(pg) ? grid[pg].at(pot: pot) : nil
    }

    // MARK: knobs

    func value(_ f: Field) -> Double { values[f.id] ?? 0 }

    func set(_ f: Field, _ v: Double) {
        guard !f.severs else { return }
        let q = f.isEnum ? v.rounded() : f.quantize(v)
        values[f.id] = q
        dirty[f.id] = q
        flush()
    }

    private func flush() {
        guard !flushing, let c = client, let d = desc else { return }
        flushing = true
        Task {
            while !dirty.isEmpty {
                let t0 = Date()
                let changes = dirty
                dirty = [:]
                let bytes = d.encode(live, changes: changes)
                do { try await c.setLive(bytes); live = bytes }
                catch { notice = "That change didn't reach the module (\(error))." }
                // At most one write per control period on this link; what
                // moves meanwhile is merged into the next one.
                let wait = kind.controlPeriod - Date().timeIntervalSince(t0)
                if wait > 0 { try? await Task.sleep(nanoseconds: UInt64(wait * 1e9)) }
            }
            flushing = false
        }
    }

    /// A knob moved by the pad, tilt or the mic: no quantizing of
    /// continuous knobs, so a sweep stays smooth.
    func set(fieldId: String, _ v: Double) {
        guard let f = desc?.field(fieldId), !f.severs, !f.isEnum else {
            if let f = desc?.field(fieldId), f.isEnum, !f.severs { set(f, v * Double(max(1, f.zones - 1))) }
            return
        }
        let q = f.steps > 0 ? f.quantize(v) : min(1, max(0, v))
        guard values[f.id] != q else { return }
        values[f.id] = q
        dirty[f.id] = q
        flush()
    }

    /// A pot on the LED screen: the field at that position on the knob page
    /// being shown (or the first knob page).
    func field(atPot pot: Int) -> Field? {
        let pg = page.flatMap { $0.grid ? $0 : nil } ?? gridPages.first
        return pg?.at(pot: pot)
    }

    func press(button: Int, _ g: HostLinkClient.PressGesture) {
        guard let c = client, phone?.hasPress == true else { return }
        Task { try? await c.press(button: button, g) }
    }

    // MARK: presets

    func refreshSlots() async {
        guard let c = client else { return }
        slots = (try? await c.listSlots()) ?? []
    }

    func load(_ slot: Int) {
        guard let c = client else { return }
        Task {
            do { try await c.loadFromSlot(slot); live = try await c.getLive(); values = desc?.decode(live) ?? [:] }
            catch { notice = "Couldn't load preset \(slot + 1): \(error)" }
        }
    }

    func save(_ slot: Int) {
        guard let c = client else { return }
        Task {
            do { try await c.saveToSlot(slot); await refreshSlots() }
            catch { notice = "Couldn't save preset \(slot + 1): \(error)" }
        }
    }

    func erase(_ slot: Int) {
        guard let c = client else { return }
        Task {
            do { try await c.eraseSlot(slot); await refreshSlots() }
            catch { notice = "Couldn't clear preset \(slot + 1): \(error)" }
        }
    }

    // MARK: samples and files

    func open(_ path: String) {
        guard let c = client else { return }
        Task {
            do {
                if fs == nil { fs = try await c.fsInfo() }
                guard fs != nil else { notice = "No SD card in the module."; return }
                let list = try await c.fsList(path)
                cwd = path
                entries = list.sorted { ($0.isDir ? 0 : 1, $0.name.lowercased()) < ($1.isDir ? 0 : 1, $1.name.lowercased()) }
            } catch let e as HostLinkError where e.statusCode == .unsupported {
                fsAvailable = false
            } catch {
                notice = "Couldn't read \(path): \(error)"
            }
        }
    }

    func up() {
        guard cwd != "/" else { return }
        let parent = (cwd as NSString).deletingLastPathComponent
        open(parent.isEmpty ? "/" : parent)
    }

    func delete(_ e: FsEntry) {
        guard let c = client else { return }
        Task {
            do { try await c.fsDelete(e.path); open(cwd) }
            catch { notice = "Couldn't delete \(e.name): \(error)" }
        }
    }

    func makeFolder(_ name: String) {
        guard let c = client, !name.isEmpty else { return }
        let path = cwd == "/" ? "/" + name : cwd + "/" + name
        Task {
            do { try await c.fsMkdir(path); open(cwd) }
            catch { notice = "Couldn't make \(name): \(error)" }
        }
    }

    /// Files picked in the Files app, sent to the folder being shown.
    func upload(_ urls: [URL], into folder: String? = nil, confirmed: Bool = false, then: (() async -> Void)? = nil) {
        guard let c = client else { return }
        if !confirmed {
            let total = urls.reduce(0) { sum, url in
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                return sum + ((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
            }
            if total > kind.bigFile {
                slowTransfer = (urls, folder, Int(Double(total) / kind.bytesPerSecond), folder == "/alchemy")
                return
            }
        }
        let dir = folder ?? cwd
        Task {
            transferring = true
            defer { transferring = false; transfer = nil }
            for url in urls {
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                guard let data = try? Data(contentsOf: url) else { notice = "Couldn't read \(url.lastPathComponent)."; continue }
                let name = url.lastPathComponent
                let path = dir == "/" ? "/" + name : dir + "/" + name
                let t0 = Date()
                transfer = (name, 0, 0)
                do {
                    try await c.fsUpload(Array(data), to: path) { p in
                        let secs = max(0.001, Date().timeIntervalSince(t0))
                        Task { @MainActor in self.transfer = (name, p, p * Double(data.count) / secs) }
                    }
                } catch {
                    notice = "\(name) didn't finish: \(error)"
                }
            }
            if folder == nil { open(cwd) }
            await then?()
        }
    }

    // MARK: firmware

    func refreshFirmwares() {
        guard let c = client, phone?.hasFirmware == true else { return }
        Task {
            do { firmwares = try await c.fwList().map { FirmwareFile(name: $0.name, size: $0.size) } }
            catch let e as HostLinkError where e.statusCode == .noCard { firmwares = []; notice = "No SD card in the module." }
            catch { notice = "Couldn't list firmware: \(error)" }
        }
    }

    func uploadFirmware(_ urls: [URL], confirmed: Bool = false) {
        upload(urls.filter { $0.pathExtension.lowercased() == "bin" }, into: "/alchemy", confirmed: confirmed) { [weak self] in
            await MainActor.run { self?.refreshFirmwares() }
        }
    }

    func switchFirmware(to name: String) {
        guard let c = client else { return }
        fwTask?.cancel()
        fwTask = Task {
            do {
                try await c.fwFlash(name)
                while !Task.isCancelled {
                    let s = try await c.fwStatus()
                    fw = s
                    if s.state == .done || s.state == .same || s.state == .error { break }
                    try await Task.sleep(nanoseconds: 250_000_000)
                }
            } catch {
                // After "done" the module reboots; losing it then is expected.
                if fw?.state != .done { notice = "Firmware switch stopped: \(error)" }
            }
        }
    }
}
