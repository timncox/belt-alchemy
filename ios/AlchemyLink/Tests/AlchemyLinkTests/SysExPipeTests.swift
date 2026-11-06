import XCTest
@testable import AlchemyLink

/// The Swift pipe must match the firmware's byte for byte: these are the
/// vectors firmware/tests/hlmidi_test.cpp --emit-golden wrote.
final class SysExPipeTests: XCTestCase {
    struct Case: Decodable { let raw: String; let sysex: String; let usb: String }
    struct File: Decodable { let cases: [Case] }

    func hex(_ s: String) -> [UInt8] {
        var out: [UInt8] = []
        var i = s.startIndex
        while i < s.endIndex {
            let j = s.index(i, offsetBy: 2)
            out.append(UInt8(s[i..<j], radix: 16)!)
            i = j
        }
        return out
    }

    func golden() throws -> [Case] {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "hlmidi_golden", withExtension: "json"))
        return try JSONDecoder().decode(File.self, from: Data(contentsOf: url)).cases
    }

    func testMatchesFirmwareVectors() throws {
        let cases = try golden()
        XCTAssertGreaterThanOrEqual(cases.count, 10)
        for c in cases {
            let raw = hex(c.raw)
            let msg = SysExPipe.encodeMessage(raw[...])
            XCTAssertEqual(msg, hex(c.sysex), "sysex for \(c.raw.count / 2) bytes")
            XCTAssertEqual(msg.count, SysExPipe.messageLen(raw.count))
            XCTAssertEqual(SysExPipe.sysexToUsb(msg), hex(c.usb), "usb for \(c.raw.count / 2) bytes")

            var d = SysExPipe.Decoder(), back: [UInt8] = []
            d.feed(msg) { back.append($0) }
            XCTAssertEqual(back, raw)
            var u = SysExPipe.Decoder(), viaUsb: [UInt8] = []
            u.feedUsb(hex(c.usb)) { viaUsb.append($0) }
            XCTAssertEqual(viaUsb, raw)
        }
    }

    func testStreamSurvivesOtherMidi() {
        let raw = (0..<1000).map { UInt8(truncatingIfNeeded: $0 &* 37 &+ 11) }
        var wire: [UInt8] = []
        for (k, m) in SysExPipe.encodeStream(raw).enumerated() {
            wire += [0x90, 60, 100]                       // a note
            wire += [0xF0, 0x43, 0x10, 0x7F, 0xF7]        // someone else's SysEx
            var m = m
            if k % 3 == 0 { m.insert(0xF8, at: 7) }       // clock inside ours
            wire += m
        }
        var d = SysExPipe.Decoder(), back: [UInt8] = []
        d.feed(wire) { back.append($0) }
        XCTAssertEqual(back, raw)
        XCTAssertEqual(d.messages, SysExPipe.encodeStream(raw).count)
    }

    func testFramesRoundTrip() {
        let f = Frame(type: 0x81, seq: 0xBEEF, body: [0, 1, 2, 0, 255, 0])
        var s = FrameSplitter(), got: [Frame] = []
        for b in [0x13, 0x77, 0x00] + f.wire() { s.feed(b) { got.append($0) } }   // a garbled frame first
        XCTAssertEqual(got, [f])
        XCTAssertEqual(CRC32.of(Array("123456789".utf8)), 0xCBF4_3926)
        let long = (0..<700).map { UInt8($0 % 251 + 1) }
        XCTAssertEqual(COBS.decode(COBS.encode(long)), long)
    }
}
