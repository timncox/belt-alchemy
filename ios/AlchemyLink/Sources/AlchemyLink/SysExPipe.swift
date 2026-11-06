// HostLink over USB MIDI SysEx: the byte pipe. A byte-for-byte port of the
// firmware's hlmidi.h, checked against the same golden vectors
// (Tests/AlchemyLinkTests/Resources/hlmidi_golden.json).
//
//   F0 7D 48 4C 01  <packed data ...>  F7
//   |  |  'H''L' version
//   |  non-commercial manufacturer id
//
// Packed data is the raw bytes 7 at a time: one byte holding the top bits
// (bit i = top bit of data byte i), then up to 7 data bytes with their top
// bit cleared. The HostLink stream is the concatenation of every message's
// raw bytes; HostLink's own COBS framing and CRC32 sit on top.

public enum SysExPipe {
    public static let start: UInt8 = 0xF0
    public static let end: UInt8 = 0xF7
    public static let manufacturer: UInt8 = 0x7D
    public static let tag0: UInt8 = 0x48   // 'H'
    public static let tag1: UInt8 = 0x4C   // 'L'
    public static let version: UInt8 = 0x01
    public static let headerLen = 5

    /// Raw bytes per SysEx message, the same limit as the firmware.
    public static let maxRawPerMessage = 140

    public static func packedLen(_ raw: Int) -> Int { raw + (raw + 6) / 7 }
    public static func messageLen(_ raw: Int) -> Int { headerLen + packedLen(raw) + 1 }

    /// 8-bit to 7-bit.
    public static func pack7(_ src: ArraySlice<UInt8>) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(packedLen(src.count))
        var i = src.startIndex
        while i < src.endIndex {
            let g = min(7, src.endIndex - i)
            var top: UInt8 = 0
            for k in 0..<g { top |= ((src[i + k] >> 7) & 1) << UInt8(k) }
            out.append(top)
            for k in 0..<g { out.append(src[i + k] & 0x7F) }
            i += g
        }
        return out
    }

    /// One whole SysEx message (F0 … F7) for up to maxRawPerMessage bytes.
    public static func encodeMessage(_ raw: ArraySlice<UInt8>) -> [UInt8] {
        precondition(raw.count <= maxRawPerMessage, "split the stream first")
        var out: [UInt8] = [start, manufacturer, tag0, tag1, version]
        out += pack7(raw)
        out.append(end)
        return out
    }

    /// A HostLink byte stream as however many SysEx messages it needs.
    public static func encodeStream(_ raw: [UInt8]) -> [[UInt8]] {
        if raw.isEmpty { return [] }
        var out: [[UInt8]] = []
        var i = 0
        while i < raw.count {
            let n = min(maxRawPerMessage, raw.count - i)
            out.append(encodeMessage(raw[i..<(i + n)]))
            i += n
        }
        return out
    }

    /// A SysEx message as USB-MIDI 1.0 event packets on cable 0 (what the
    /// firmware sends; CoreMIDI does this for us, the tests check it).
    public static func sysexToUsb(_ msg: [UInt8]) -> [UInt8] {
        var out: [UInt8] = []
        var i = 0
        while msg.count - i > 3 {
            out += [0x04, msg[i], msg[i + 1], msg[i + 2]]
            i += 3
        }
        let last = msg.count - i
        out.append(UInt8(0x04 + last))
        for k in 0..<3 { out.append(k < last ? msg[i + k] : 0) }
        return out
    }

    /// Streaming decoder: MIDI bytes in, HostLink bytes out. Ignores notes,
    /// clock and other SysEx; real-time bytes inside a message are skipped,
    /// any other status byte abandons the message.
    public struct Decoder {
        enum State { case idle, mfrWait, tagHWait, tagLWait, versionWait, data }
        var state = State.idle
        var pos: UInt8 = 0
        var top: UInt8 = 0
        public private(set) var messages = 0

        public init() {}

        public mutating func feed(_ b: UInt8, _ sink: (UInt8) -> Void) {
            if b >= 0xF8 { return }
            if b == SysExPipe.start { state = .mfrWait; pos = 0; return }
            if b & 0x80 != 0 {
                if state == .data && b == SysExPipe.end { messages += 1 }
                state = .idle
                return
            }
            switch state {
            case .idle: return
            case .mfrWait: state = b == SysExPipe.manufacturer ? .tagHWait : .idle
            case .tagHWait: state = b == SysExPipe.tag0 ? .tagLWait : .idle
            case .tagLWait: state = b == SysExPipe.tag1 ? .versionWait : .idle
            case .versionWait: state = b == SysExPipe.version ? .data : .idle; pos = 0
            case .data:
                if pos == 0 { top = b } else { sink(b | (((top >> (pos - 1)) & 1) << 7)) }
                pos = (pos + 1) & 7
            }
        }

        public mutating func feed<S: Sequence>(_ bytes: S, _ sink: (UInt8) -> Void) where S.Element == UInt8 {
            for b in bytes { feed(b, sink) }
        }

        /// USB-MIDI 1.0 event packets, for the tests.
        public mutating func feedUsb(_ buf: [UInt8], _ sink: (UInt8) -> Void) {
            let cinSize: [Int] = [0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1]
            var i = 0
            while i + 4 <= buf.count {
                let n = cinSize[Int(buf[i] & 0x0F)]
                for k in 0..<n { feed(buf[i + 1 + k], sink) }
                i += 4
            }
        }
    }
}
