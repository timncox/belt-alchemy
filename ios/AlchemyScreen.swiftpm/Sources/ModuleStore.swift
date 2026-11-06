// Everything the screens show, kept in step with the module: the LED
// screen (polled ~15 times a second), the knob values (every 400 ms, like
// the web Control page), presets, the card's files and firmware switching.
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
final class ModuleStore: ObservableObject {
    enum Link: Equatable { case none, usb(String), demo }
    enum Phase: Equatable { case searching, connecting, ready, failed(String) }

    @Published private(set) var link: Link = .none
    @Published private(set) var phase: Phase = .searching
    @Published private(set) var ports: [MIDIPort] = []

    @Published private(set) var hello: Hello?
    @Published private(set) var desc: Descriptor?
    @Published private(set) var phone: PhoneInfo?
    @Published private(set) var values: [String: Double] = [:]
    @Published private(set) var screen: [UInt8] = []
    @Published var pageIndex = 0
    @Published private(set) var slots: [SlotInfo] = []

    @Published private(set) var fs: FsInfo?
    @Published private(set) var fsAvailable = false
    @Published private(set) var cwd = "/"
    @Published private(set) var entries: [FsEntry] = []
    @Published private(set) var transfer: (name: String, progress: Double, rate: Double)?

    @Published private(set) var firmwares: [FirmwareFile] = []
    @Published private(set) var fw: FwStatus?
    @Published var notice: String?

    private var midi: MIDILink?
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

    init() {
        do {
            let m = try MIDILink()
            m.onSetupChanged = { [weak self] in Task { @MainActor in self?.setupChanged() } }
            midi = m
        } catch {
            phase = .failed(String(describing: error))
        }
        rescan()
    }

    // MARK: connecting

    func rescan() {
        guard let m = midi else { return }
        ports = m.ports()
        if case .usb = link { return }
        if link == .demo { return }
        if let p = m.likelyModule() { connect(p) } else { phase = .searching }
    }

    private func setupChanged() {
        guard let m = midi else { return }
        ports = m.ports()
        if case .usb = link, !m.stillPresent {
            drop(reason: nil)          // unplugged, or rebooting into new firmware
        }
        if link == .none, let p = m.likelyModule() { connect(p) }
    }

    func connect(_ port: MIDIPort) {
        guard let m = midi else { return }
        stop()
        phase = .connecting
        guard m.connect(port) else { phase = .failed("Couldn't open \(port.name)."); return }
        link = .usb(port.name)
        start(HostLinkClient(transport: m))
    }

    func startDemo() {
        stop()
        midi?.disconnect()
        link = .demo
        phase = .connecting
        start(HostLinkClient(transport: FakeModule(viaSysEx: true)))
    }

    func disconnect() { drop(reason: nil) }

    private func drop(reason: String?) {
        stop()
        midi?.disconnect()
        link = .none
        hello = nil; desc = nil; phone = nil; values = [:]; screen = []; slots = []
        fs = nil; fsAvailable = false; entries = []; firmwares = []
        phase = reason.map { .failed($0) } ?? .searching
        if reason == nil, let p = midi?.likelyModule() { connect(p) }
    }

    private func stop() {
        pollTask?.cancel(); pollTask = nil
        fwTask?.cancel(); fwTask = nil
        client = nil
    }

    private func start(_ c: HostLinkClient) {
        client = c
        pollTask = Task { [weak self] in
            guard let self else { return }
            do {
                let h = try await c.sayHello()
                let d = try await c.descriptor()
                let p = try await c.phoneInfo()
                let bytes = try await c.getLive()
                self.hello = h; self.desc = d; self.phone = p
                self.live = bytes; self.values = d.decode(bytes)
                self.pageIndex = 0
                self.phase = .ready
                self.fsAvailable = d.hasStorage
                await self.refreshSlots()
            } catch {
                self.phase = .failed("The module didn't answer. Is its USB port setting on Phone? (\(error))")
                return
            }
            await self.pollLoop(c)
        }
    }

    private func pollLoop(_ c: HostLinkClient) async {
        var tick = 0
        var misses = 0
        while !Task.isCancelled {
            do {
                if phone?.hasScreen == true && !transferring {
                    screen = try await c.screen().rgb
                }
                if tick % 6 == 0 && dirty.isEmpty && !flushing && !transferring {
                    let b = try await c.getLive()
                    if dirty.isEmpty && !flushing, let d = desc { live = b; values = d.decode(b) }
                }
                misses = 0
            } catch {
                misses += 1
                if misses >= 5 { drop(reason: "Lost the module.") ; return }
            }
            tick += 1
            try? await Task.sleep(nanoseconds: transferring ? 400_000_000 : 66_000_000)
        }
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
                let changes = dirty
                dirty = [:]
                let bytes = d.encode(live, changes: changes)
                do { try await c.setLive(bytes); live = bytes }
                catch { notice = "That change didn't reach the module (\(error))." }
            }
            flushing = false
        }
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
    func upload(_ urls: [URL], into folder: String? = nil, then: (() async -> Void)? = nil) {
        guard let c = client else { return }
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

    func uploadFirmware(_ urls: [URL]) {
        upload(urls.filter { $0.pathExtension.lowercased() == "bin" }, into: "/alchemy") { [weak self] in
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
