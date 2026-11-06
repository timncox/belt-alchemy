// HostLink v1 framing (the Alchemy SDK's docs/hostlink-protocol.md §1), a
// port of the web Control page's hostlink.js. Little-endian throughout.
//
//   decoded frame: proto(1) type(1) seq(2) len(2) body(len) crc32(4)
//   on the wire:   COBS(decoded frame) 0x00

import Foundation

public enum Cmd: UInt8 {
    case hello = 0x01, descriptor = 0x02
    case listSlots = 0x10, readSlot = 0x11, blobBegin = 0x12, blobData = 0x13, blobCommit = 0x14, eraseSlot = 0x15
    case getLive = 0x20, saveToSlot = 0x30, loadFromSlot = 0x31, reboot = 0x40
    case fsInfo = 0x50, fsList = 0x51, fsStat = 0x52, fsOpenRead = 0x53, fsRead = 0x54, fsClose = 0x55
    case fsOpenWrite = 0x56, fsWrite = 0x57, fsCommit = 0x58, fsDelete = 0x59, fsMkdir = 0x5A, fsRename = 0x5B
    // The phone block (firmware/phone_ext.h).
    case phoneInfo = 0x70, screen = 0x71, fwList = 0x72, fwFlash = 0x73, fwStatus = 0x74, press = 0x75
}

public enum Status: UInt8, CustomStringConvertible {
    case ok = 0, unsupported, badArgs, badState, badCrc, badSlot, tooLarge, schemaMismatch, flashFail, busy, frameError
    case noCard, noFile, exists, locked, full, io

    public var description: String {
        switch self {
        case .ok: return "ok"
        case .unsupported: return "not supported by this firmware"
        case .badArgs: return "bad request"
        case .badState: return "not now"
        case .badCrc: return "checksum mismatch"
        case .badSlot: return "empty slot"
        case .tooLarge: return "too large"
        case .schemaMismatch: return "made by another firmware"
        case .flashFail: return "flash write failed"
        case .busy: return "busy"
        case .frameError: return "garbled request"
        case .noCard: return "no SD card"
        case .noFile: return "no such file"
        case .exists: return "already exists"
        case .locked: return "in use by the module"
        case .full: return "card full"
        case .io: return "card error"
        }
    }
}

public let liveTarget: UInt8 = 0xFF   // BLOB_BEGIN target: the running state

public enum HostLinkError: Error, CustomStringConvertible {
    case status(Cmd, UInt8)
    case timeout(Cmd)
    case notConnected
    case badReply(String)

    public var description: String {
        switch self {
        case let .status(c, s):
            return "\(c): " + (Status(rawValue: s)?.description ?? "device error \(s)")
        case let .timeout(c): return "\(c): no answer"
        case .notConnected: return "module not connected"
        case let .badReply(why): return "unexpected reply: \(why)"
        }
    }

    public var statusCode: Status? {
        if case let .status(_, s) = self { return Status(rawValue: s) }
        return nil
    }
}

// MARK: - CRC32 and COBS

public enum CRC32 {
    static let table: [UInt32] = (0..<256).map { n -> UInt32 in
        var c = UInt32(n)
        for _ in 0..<8 { c = (c & 1) != 0 ? 0xEDB8_8320 ^ (c >> 1) : c >> 1 }
        return c
    }

    public static func of<C: Collection>(_ buf: C, seed: UInt32 = 0) -> UInt32 where C.Element == UInt8 {
        var c = seed ^ 0xFFFF_FFFF
        for b in buf { c = table[Int((c ^ UInt32(b)) & 0xFF)] ^ (c >> 8) }
        return c ^ 0xFFFF_FFFF
    }
}

public enum COBS {
    public static func encode(_ src: [UInt8]) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: src.count + src.count / 254 + 2)
        var o = 1, code: UInt8 = 1, codeAt = 0
        for b in src {
            if b == 0 { out[codeAt] = code; codeAt = o; o += 1; code = 1; continue }
            out[o] = b; o += 1
            code += 1
            if code == 0xFF { out[codeAt] = code; codeAt = o; o += 1; code = 1 }
        }
        out[codeAt] = code
        return Array(out[0..<o])
    }

    public static func decode(_ src: [UInt8]) -> [UInt8]? {
        var out: [UInt8] = []
        out.reserveCapacity(src.count)
        var i = 0
        while i < src.count {
            let code = Int(src[i]); i += 1
            if code == 0 || i + code - 1 > src.count { return nil }
            for _ in 1..<code { out.append(src[i]); i += 1 }
            if code < 0xFF && i < src.count { out.append(0) }
        }
        return out
    }
}

// MARK: - Frames

public struct Frame: Equatable {
    public var type: UInt8
    public var seq: UInt16
    public var body: [UInt8]

    public init(type: UInt8, seq: UInt16, body: [UInt8]) {
        self.type = type; self.seq = seq; self.body = body
    }

    /// COBS-encoded, with the trailing 0x00 delimiter.
    public func wire() -> [UInt8] {
        var dec: [UInt8] = [1, type, UInt8(seq & 0xFF), UInt8(seq >> 8),
                            UInt8(body.count & 0xFF), UInt8(body.count >> 8)]
        dec += body
        let crc = CRC32.of(dec)
        dec += [UInt8(crc & 0xFF), UInt8((crc >> 8) & 0xFF), UInt8((crc >> 16) & 0xFF), UInt8(crc >> 24)]
        return COBS.encode(dec) + [0]
    }

    /// A decoded (un-COBSed) frame, or nil if it fails any check.
    public static func parse(_ dec: [UInt8]) -> Frame? {
        guard dec.count >= 10, dec[0] == 1 else { return nil }
        let len = Int(dec[4]) | Int(dec[5]) << 8
        guard dec.count == 10 + len else { return nil }
        let crc = UInt32(dec[6 + len]) | UInt32(dec[7 + len]) << 8 | UInt32(dec[8 + len]) << 16 | UInt32(dec[9 + len]) << 24
        guard CRC32.of(dec[0..<(6 + len)]) == crc else { return nil }
        return Frame(type: dec[1], seq: UInt16(dec[2]) | UInt16(dec[3]) << 8, body: Array(dec[6..<(6 + len)]))
    }
}

/// Splits a HostLink byte stream at 0x00 delimiters into frames.
public struct FrameSplitter {
    var acc: [UInt8] = []
    public init() {}

    public mutating func feed(_ b: UInt8, _ onFrame: (Frame) -> Void) {
        if b != 0 { if acc.count < 8192 { acc.append(b) }; return }
        defer { acc.removeAll(keepingCapacity: true) }
        guard !acc.isEmpty, let dec = COBS.decode(acc), let f = Frame.parse(dec) else { return }
        onFrame(f)
    }
}

// MARK: - Body helpers

public struct Writer {
    public private(set) var bytes: [UInt8] = []
    public init() {}
    // Each call returns a longer copy, so requests read as one chain:
    //   Writer().u32(off).u16(n).bytes
    public func u8(_ v: UInt8) -> Writer { var c = self; c.bytes.append(v); return c }
    public func u16(_ v: UInt16) -> Writer { var c = self; c.bytes += [UInt8(v & 0xFF), UInt8(v >> 8)]; return c }
    public func u32(_ v: UInt32) -> Writer {
        var c = self
        c.bytes += [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8(v >> 24)]
        return c
    }
    public func raw<C: Collection>(_ b: C) -> Writer where C.Element == UInt8 { var c = self; c.bytes += b; return c }
    public func str(_ s: String) -> Writer {
        let e = Array(s.utf8.prefix(255))
        var c = self; c.bytes.append(UInt8(e.count)); c.bytes += e; return c
    }
}

public struct Reader {
    let b: [UInt8]
    public private(set) var i = 0
    public init(_ b: [UInt8]) { self.b = b }

    public var remaining: Int { b.count - i }

    public mutating func u8() throws -> UInt8 {
        guard i < b.count else { throw HostLinkError.badReply("short body") }
        defer { i += 1 }
        return b[i]
    }
    public mutating func u16() throws -> UInt16 {
        let lo = try u8(), hi = try u8()
        return UInt16(lo) | UInt16(hi) << 8
    }
    public mutating func u32() throws -> UInt32 {
        let lo = try u16(), hi = try u16()
        return UInt32(lo) | UInt32(hi) << 16
    }
    public mutating func bytes(_ n: Int) throws -> [UInt8] {
        guard n >= 0, i + n <= b.count else { throw HostLinkError.badReply("short body") }
        defer { i += n }
        return Array(b[i..<(i + n)])
    }
    public mutating func str() throws -> String {
        let n = try u8()
        let raw = try bytes(Int(n))
        return String(decoding: raw, as: UTF8.self)
    }
    public mutating func rest() -> [UInt8] { defer { i = b.count }; return Array(b[i...]) }
}
