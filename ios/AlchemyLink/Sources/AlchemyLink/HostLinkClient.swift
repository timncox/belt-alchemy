// The host side of HostLink: one request in flight at a time (stop-and-wait,
// protocol §7), the commands the web Control page uses, the filesystem
// block (§8) and the phone block (firmware/phone_ext.h). Any transport that
// moves the HostLink byte stream works: USB MIDI SysEx to a real module
// (MIDILink in the app) or the in-process FakeModule.

import Foundation

/// Moves HostLink bytes. The client hands it whole wire frames; it calls
/// the receive handler with whatever bytes arrive, from any thread.
public protocol ByteTransport: AnyObject {
    func send(_ bytes: [UInt8])
    func setReceiveHandler(_ handler: @escaping ([UInt8]) -> Void)
}

/// FIFO gate: one request at a time, in call order.
actor Gate {
    private var busy = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    func enter() async {
        if !busy { busy = true; return }
        await withCheckedContinuation { waiters.append($0) }
    }

    func leave() {
        if waiters.isEmpty { busy = false } else { waiters.removeFirst().resume() }
    }
}

public struct Hello {
    public var proto: UInt8, board: UInt8, slotCount: UInt8, bootSlot: UInt8
    public var schemaHash: UInt32, blobCapacity: UInt32, descriptorLen: UInt32, descriptorCrc: UInt32
    public var maxBody: UInt16, liveSize: UInt16
    public var moduleId: String, moduleName: String, fw: String, git: String, sdk: String
}

public struct SlotInfo: Identifiable {
    public var id: Int { slot }
    public var slot: Int, valid: Bool, ours: Bool, seq: UInt32, length: UInt16, schemaHash: UInt32
}

public final class HostLinkClient: @unchecked Sendable {
    public let transport: ByteTransport
    private let gate = Gate()
    private let lock = NSLock()
    private var splitter = FrameSplitter()
    private var pending: (seq: UInt16, type: UInt8, cont: CheckedContinuation<Frame, Error>)?
    private var seq: UInt16 = 1

    public private(set) var hello: Hello?
    public var maxBody: Int { Int(hello?.maxBody ?? 512) }
    var chunk: Int { maxBody - 16 }

    public init(transport: ByteTransport) {
        self.transport = transport
        transport.setReceiveHandler { [weak self] bytes in self?.ingest(bytes) }
    }

    // MARK: request / response

    private func ingest(_ bytes: [UInt8]) {
        lock.lock()
        var done: [(CheckedContinuation<Frame, Error>, Frame)] = []
        for b in bytes {
            splitter.feed(b) { f in
                if let p = pending, f.seq == p.seq, f.type == (p.type | 0x80) || f.type == 0xFF {
                    pending = nil
                    done.append((p.cont, f))
                }
            }
        }
        lock.unlock()
        for (c, f) in done { c.resume(returning: f) }
    }

    private func failPending(seq: UInt16, with error: Error) {
        lock.lock()
        guard let p = pending, p.seq == seq else { lock.unlock(); return }
        pending = nil
        lock.unlock()
        p.cont.resume(throwing: error)
    }

    private func once(_ cmd: Cmd, _ body: [UInt8], timeout: Double) async throws -> Frame {
        lock.lock()
        seq = seq &+ 1
        if seq == 0 { seq = 1 }
        let s = seq
        lock.unlock()
        return try await withCheckedThrowingContinuation { cont in
            lock.lock()
            pending = (s, cmd.rawValue, cont)
            lock.unlock()
            transport.send(Frame(type: cmd.rawValue, seq: s, body: body).wire())
            Task { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(timeout * 1e9))
                self?.failPending(seq: s, with: HostLinkError.timeout(cmd))
            }
        }
    }

    /// One request; returns a reader past the status byte. Throws on a
    /// non-OK status unless `allow` lists it. `retries` is for idempotent
    /// reads: a SysEx message lost on the way costs a retry, not a failure.
    @discardableResult
    public func request(_ cmd: Cmd, _ body: [UInt8] = [], timeout: Double = 2, retries: Int = 0,
                        allow: Set<Status> = []) async throws -> (status: Status, r: Reader) {
        let g = gate
        await g.enter()
        defer { Task { await g.leave() } }
        var attempt = 0
        while true {
            do {
                let f = try await once(cmd, body, timeout: timeout)
                guard let code = f.body.first else { throw HostLinkError.badReply("empty body") }
                let st = Status(rawValue: code)
                let allowed = st.map { allow.contains($0) } ?? false
                if code != 0 && !allowed {
                    throw HostLinkError.status(cmd, code)
                }
                return (st ?? .ok, Reader(Array(f.body.dropFirst())))
            } catch HostLinkError.timeout(let c) where attempt < retries {
                attempt += 1
                _ = c
            }
        }
    }

    // MARK: identity and state

    @discardableResult
    public func sayHello() async throws -> Hello {
        var r = try await request(.hello, retries: 2).r
        let proto = try r.u8(), board = try r.u8(), slotCount = try r.u8(), bootSlot = try r.u8()
        _ = try r.bytes(12)
        let schema = try r.u32(), cap = try r.u32(), dlen = try r.u32(), dcrc = try r.u32()
        let maxBody = try r.u16(), live = try r.u16()
        let id = try r.str(), name = try r.str(), fw = try r.str(), git = try r.str(), sdk = try r.str()
        let h = Hello(proto: proto, board: board, slotCount: slotCount, bootSlot: bootSlot,
                      schemaHash: schema, blobCapacity: cap, descriptorLen: dlen, descriptorCrc: dcrc,
                      maxBody: maxBody, liveSize: live, moduleId: id, moduleName: name, fw: fw, git: git, sdk: sdk)
        lock.lock(); hello = h; lock.unlock()
        return h
    }

    private func needHello() async throws -> Hello {
        if let h = hello { return h }
        return try await sayHello()
    }

    public func descriptor() async throws -> Descriptor {
        let h = try await needHello()
        let total = Int(h.descriptorLen)
        guard total > 0 else { throw HostLinkError.badReply("this firmware has no descriptor") }
        var out: [UInt8] = []
        out.reserveCapacity(total)
        while out.count < total {
            var r = try await request(.descriptor, Writer().u32(UInt32(out.count)).u16(UInt16(min(chunk, total - out.count))).bytes,
                                      retries: 2).r
            _ = try r.u32()
            let n = Int(try r.u16())
            if n == 0 { break }
            out += try r.bytes(n)
        }
        guard CRC32.of(out) == h.descriptorCrc else { throw HostLinkError.badReply("descriptor CRC mismatch") }
        return try Descriptor(json: Data(out))
    }

    /// GET_LIVE: the running state, as the bytes the descriptor describes.
    public func getLive() async throws -> [UInt8] { try await readChunked(.getLive, prefix: []) }

    public func readSlot(_ slot: Int) async throws -> [UInt8] {
        try await readChunked(.readSlot, prefix: [UInt8(slot)])
    }

    private func readChunked(_ cmd: Cmd, prefix: [UInt8]) async throws -> [UInt8] {
        var out: [UInt8] = []
        var total = Int.max
        while out.count < total {
            var r = try await request(cmd, Writer().raw(prefix).u32(UInt32(out.count)).u16(UInt16(chunk)).bytes, retries: 2).r
            _ = try r.u8(); _ = try r.u32()
            let n = Int(try r.u16())
            total = Int(try r.u16())
            _ = try r.u32()
            if n == 0 { break }
            out += try r.bytes(n)
        }
        return out
    }

    /// BLOB_* to a slot (0…15) or `liveTarget`: the bytes must be this firmware's schema.
    public func writeBlob(target: UInt8, _ bytes: [UInt8]) async throws {
        let h = try await needHello()
        try await request(.blobBegin, Writer().u8(target).u32(UInt32(bytes.count)).u32(h.schemaHash).bytes)
        let step = maxBody - 4
        var off = 0
        while off < bytes.count {
            let n = min(step, bytes.count - off)
            try await request(.blobData, Writer().u32(UInt32(off)).raw(bytes[off..<(off + n)]).bytes)
            off += n
        }
        try await request(.blobCommit, Writer().u32(CRC32.of(bytes)).bytes, timeout: 10)
    }

    public func setLive(_ bytes: [UInt8]) async throws { try await writeBlob(target: liveTarget, bytes) }

    public func listSlots() async throws -> [SlotInfo] {
        var r = try await request(.listSlots, retries: 2).r
        let n = Int(try r.u8())
        var out: [SlotInfo] = []
        for i in 0..<n {
            let flags = try r.u8(), seq = try r.u32(), len = try r.u16(), hash = try r.u32()
            out.append(SlotInfo(slot: i, valid: flags & 1 != 0, ours: flags & 2 != 0, seq: seq, length: len, schemaHash: hash))
        }
        return out
    }

    public func saveToSlot(_ slot: Int) async throws { try await request(.saveToSlot, [UInt8(slot)], timeout: 10) }
    public func loadFromSlot(_ slot: Int) async throws { try await request(.loadFromSlot, [UInt8(slot)]) }
    public func eraseSlot(_ slot: Int) async throws { try await request(.eraseSlot, [UInt8(slot)], timeout: 10) }
}

// MARK: - Filesystem block (§8)

public struct FsInfo {
    public var mounted: Bool, totalKiB: UInt32, freeKiB: UInt32?, busy: Bool
}

public struct FsEntry: Identifiable, Hashable {
    public var id: String { path }
    public var path: String, name: String, isDir: Bool, locked: Bool, size: UInt32, mtime: UInt32
}

extension HostLinkClient {
    /// nil when the module has a card slot but no card.
    public func fsInfo() async throws -> FsInfo? {
        let (st, r0) = try await request(.fsInfo, timeout: 10, allow: [.noCard])
        if st == .noCard { return nil }
        var r = r0
        let state = try r.u8(); _ = try r.u8()
        let flags = try r.u8(), total = try r.u32(), free = try r.u32()
        return FsInfo(mounted: state == 1, totalKiB: total, freeKiB: flags & 1 != 0 ? free : nil, busy: flags & 2 != 0)
    }

    public func fsList(_ path: String) async throws -> [FsEntry] {
        var cookie: UInt32 = 0
        var out: [FsEntry] = []
        repeat {
            var r = try await request(.fsList, Writer().u32(cookie).str(path).bytes, timeout: 5, retries: 2).r
            cookie = try r.u32()
            let n = Int(try r.u8())
            for _ in 0..<n {
                let flags = try r.u8(), size = try r.u32(), mtime = try r.u32(), name = try r.str()
                let full = path.hasSuffix("/") ? path + name : path + "/" + name
                out.append(FsEntry(path: full, name: name, isDir: flags & 1 != 0, locked: flags & 4 != 0, size: size, mtime: mtime))
            }
        } while cookie != 0xFFFF_FFFF
        return out
    }

    public func fsDelete(_ path: String) async throws { try await request(.fsDelete, Writer().str(path).bytes, timeout: 10) }
    public func fsMkdir(_ path: String) async throws { try await request(.fsMkdir, Writer().str(path).bytes, timeout: 10) }
    public func fsRename(_ from: String, to: String) async throws {
        try await request(.fsRename, Writer().str(from).str(to).bytes, timeout: 10)
    }

    /// Upload, atomically (the module stages into .part and renames only on
    /// a CRC-checked commit). `progress` gets 0…1 from the caller's task.
    public func fsUpload(_ data: [UInt8], to path: String, overwrite: Bool = true, makeDirs: Bool = true,
                         progress: ((Double) -> Void)? = nil) async throws {
        let flags: UInt8 = (overwrite ? 1 : 0) | (makeDirs ? 2 : 0) | 4   // resume when possible
        var r = try await request(.fsOpenWrite, Writer().u8(flags).u32(UInt32(data.count)).str(path).bytes, timeout: 10).r
        var off = Int(try r.u32())
        let step = maxBody - 4
        while off < data.count {
            try Task.checkCancellation()
            let n = min(step, data.count - off)
            var w = try await request(.fsWrite, Writer().u32(UInt32(off)).raw(data[off..<(off + n)]).bytes, timeout: 5).r
            off = Int(try w.u32())
            progress?(Double(off) / Double(max(1, data.count)))
        }
        try await request(.fsCommit, Writer().u32(CRC32.of(data)).bytes, timeout: 10)
        progress?(1)
    }

    public func fsDownload(_ path: String, progress: ((Double) -> Void)? = nil) async throws -> [UInt8] {
        var r = try await request(.fsOpenRead, Writer().str(path).bytes, timeout: 10).r
        let size = Int(try r.u32())
        var out: [UInt8] = []
        out.reserveCapacity(size)
        while out.count < size {
            try Task.checkCancellation()
            var c = try await request(.fsRead, Writer().u32(UInt32(out.count)).u16(UInt16(maxBody - 7)).bytes, retries: 2).r
            _ = try c.u32()
            let n = Int(try c.u16())
            if n == 0 { break }
            out += try c.bytes(n)
            progress?(Double(out.count) / Double(max(1, size)))
        }
        try? await request(.fsClose)
        return out
    }
}

// MARK: - Phone block (firmware/phone_ext.h)

public struct PhoneInfo {
    public var version: UInt8, hasScreen: Bool, hasFirmware: Bool, hasPress: Bool, ledCount: Int, layout: String
    /// The module answers STATE (0x76): the phone draws its own screen.
    public var hasState: Bool = false
    /// Tells two modules apart (a hash of the MCU's unique ID); 0 = unknown.
    public var unit: UInt32 = 0
    /// The module takes the phone's controllers (0x77 CTRL, flags bit 4).
    public var hasCtrl: Bool = false
}

public struct ScreenFrame {
    public var frame: UInt32
    public var rgb: [UInt8]   // ledCount × 3
}

public enum FwState: UInt8 {
    case idle = 0, comparing, erasing, writing, verifying, done, same, error
}

public struct FwStatus {
    public var state: FwState, progress: Double, error: String
}

extension HostLinkClient {
    /// nil when this firmware has no phone block.
    public func phoneInfo() async throws -> PhoneInfo? {
        let (st, r0) = try await request(.phoneInfo, retries: 2, allow: [.unsupported])
        if st == .unsupported { return nil }
        var r = r0
        let v = try r.u8(), flags = try r.u8(), n = try r.u16(), layout = try r.str()
        var info = PhoneInfo(version: v, hasScreen: flags & 1 != 0, hasFirmware: flags & 2 != 0,
                             hasPress: flags & 4 != 0, ledCount: Int(n), layout: layout)
        info.hasState = flags & 8 != 0
        info.hasCtrl = flags & 16 != 0
        if r.remaining >= 4 { info.unit = try r.u32() }   // version 2 on
        return info
    }

    public func screen() async throws -> ScreenFrame {
        var r = try await request(.screen, timeout: 1, retries: 1).r
        let frame = try r.u32(), n = Int(try r.u16())
        return ScreenFrame(frame: frame, rgb: try r.bytes(3 * n))
    }

    public func fwList() async throws -> [(name: String, size: UInt32)] {
        var r = try await request(.fwList, timeout: 10).r
        let n = Int(try r.u8())
        var out: [(name: String, size: UInt32)] = []
        for _ in 0..<n {
            let name = try r.str(), size = try r.u32()
            out.append((name: name, size: size))
        }
        return out
    }

    public func fwFlash(_ name: String) async throws { try await request(.fwFlash, Writer().str(name).bytes, timeout: 10) }

    public func fwStatus() async throws -> FwStatus {
        var r = try await request(.fwStatus, retries: 2).r
        let st = try r.u8(), p = try r.u8(), err = try r.str()
        return FwStatus(state: FwState(rawValue: st) ?? .error, progress: Double(p) / 255, error: err)
    }

    public enum PressGesture: UInt8 { case tap = 0, down = 1, up = 2 }

    public func press(button: Int, _ g: PressGesture) async throws {
        try await request(.press, [UInt8(button), g.rawValue])
    }

    /// What the phone's own screen shows (0x76). `since` is the last trace
    /// sample already held, so the module sends only newer ones.
    public func state(since: UInt16?, timeout: Double = 1) async throws -> ModuleState {
        let body = since.map { [UInt8($0 & 0xFF), UInt8($0 >> 8)] } ?? []
        let r = try await request(.state, body, timeout: timeout, retries: 1).r
        return try ModuleState(reading: r)
    }
}
