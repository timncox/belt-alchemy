import Foundation
import XCTest
@testable import AlchemyLink

/// Splits bytes into chunks of awkward sizes, as TCP may.
private func oddChunks(_ bytes: [UInt8], sizes: [Int] = [1, 2, 3, 5, 7, 11, 13, 64]) -> [[UInt8]] {
    var out: [[UInt8]] = []
    var i = 0, k = 0
    while i < bytes.count {
        let n = min(sizes[k % sizes.count], bytes.count - i)
        out.append(Array(bytes[i..<(i + n)]))
        i += n; k += 1
    }
    return out
}

/// A stream that only records what's written; reads are fed by hand.
private final class HandStream: MIDIByteStream {
    var written: [UInt8] = []
    var reader: (([UInt8]) -> Void)?
    func write(_ bytes: [UInt8]) { written += bytes }
    func setReadHandler(_ h: @escaping ([UInt8]) -> Void) { reader = h }
}

/// The module emulator's --hostlink socket, in-process: plain MIDI bytes
/// both ways, the module's answers delivered in odd-sized chunks.
private final class LoopbackEmulator: MIDIByteStream, @unchecked Sendable {
    let module: FakeModule
    private let lock = NSLock()
    private var decoder = SysExPipe.Decoder()
    private var reader: (([UInt8]) -> Void)?

    init(unit: UInt32) {
        module = FakeModule(viaSysEx: false, unit: unit)
        module.setReceiveHandler { [weak self] raw in
            guard let self else { return }
            self.lock.lock(); let r = self.reader; self.lock.unlock()
            // Some MIDI that isn't HostLink in between, as a module may send.
            r?([0x90, 0x3C, 0x40, 0xF8])
            for c in oddChunks(MIDIStreamLink.encode(raw)) { r?(c) }
        }
    }

    func write(_ bytes: [UInt8]) {
        var raw: [UInt8] = []
        lock.lock()
        decoder.feed(bytes) { raw.append($0) }
        lock.unlock()
        if !raw.isEmpty { module.send(raw) }
    }

    func setReadHandler(_ h: @escaping ([UInt8]) -> Void) { lock.lock(); reader = h; lock.unlock() }
}

final class EmulatorStreamTests: XCTestCase {
    func testStreamDecodesAcrossOddSplits() {
        let a = Frame(type: 0x01, seq: 7, body: Array(0..<200).map { UInt8($0 & 0xFF) }).wire()
        let b = Frame(type: 0x76, seq: 8, body: [0x80, 0xFF, 0x00, 0x7F]).wire()
        var midi: [UInt8] = [0x90, 0x40, 0x7F]                       // a note before
        var first = MIDIStreamLink.encode(a)
        first.insert(0xF8, at: 9)                                     // clock inside the SysEx
        midi += first
        midi += [0xF0, 0x43, 0x10, 0x01, 0xF7]                        // someone else's SysEx
        midi += MIDIStreamLink.encode(b)
        for sizes in [[1], [2, 3], [5, 7, 11], [4096]] {
            let s = HandStream()
            let link = MIDIStreamLink(stream: s)
            var got: [UInt8] = []
            link.setReceiveHandler { got += $0 }
            for c in oddChunks(midi, sizes: sizes) { s.reader?(c) }
            XCTAssertEqual(got, a + b, "chunks \(sizes)")
            var frames: [Frame] = []
            var split = FrameSplitter()
            for x in got { split.feed(x) { frames.append($0) } }
            XCTAssertEqual(frames.map(\.seq), [7, 8])
        }
    }

    func testSendIsTheSameSysExAsUSB() {
        let s = HandStream()
        let link = MIDIStreamLink(stream: s)
        let wire = Frame(type: 0x10, seq: 1, body: Array(repeating: 0xAA, count: 300)).wire()
        link.send(wire)
        XCTAssertEqual(s.written, SysExPipe.encodeStream(wire).flatMap { $0 })
        XCTAssertEqual(s.written.first, 0xF0)
        XCTAssertEqual(Array(s.written.prefix(5)), [0xF0, 0x7D, 0x48, 0x4C, 0x01])
        XCTAssertEqual(s.written.last, 0xF7)
        XCTAssertTrue(s.written.filter { $0 == 0xF0 }.count == 3)    // 300 bytes: three messages
    }

    func testClientTalksToEmulatorOverTheStream() async throws {
        for unit in [UInt32(0xE1E1_0001), 0xE1E1_0002] {
            let emu = LoopbackEmulator(unit: unit)
            let client = HostLinkClient(transport: MIDIStreamLink(stream: emu))
            let h = try await client.sayHello()
            XCTAssertEqual(h.moduleId, "belt_alchemy")
            let info = try await client.phoneInfo()
            XCTAssertEqual(info?.unit, unit)
            let d = try await client.descriptor()
            XCTAssertEqual(d.pages.first?.name, "Play")
            let reply = try await client.ctrl(present: [.launchpad], events: [.launchpad(.grid, x: 2, y: 3, down: true)])
            XCTAssertEqual(reply?.lpLeds[LaunchpadMK3.index(.grid, x: 2, y: 3)], 21)
        }
    }

    func testEmulatorSettings() {
        let s = EmulatorSettings()
        XCTAssertEqual(s.host, "127.0.0.1")
        XCTAssertEqual(s.ports, [5071, 5072])
        XCTAssertEqual(EmulatorSettings.port(" 5073 "), 5073)
        XCTAssertNil(EmulatorSettings.port("0"))
        XCTAssertNil(EmulatorSettings.port("70000"))
        let back = try? JSONDecoder().decode(EmulatorSettings.self, from: JSONEncoder().encode(s))
        XCTAssertEqual(back, s)
        XCTAssertEqual(LinkKind.emulator.statePeriod, LinkKind.usb.statePeriod)
    }
}

final class VirtualDeviceTests: XCTestCase {
    private func parse(_ bytes: [UInt8]) -> [MIDIMessage] {
        var p = MIDIStreamParser()
        var out: [MIDIMessage] = []
        p.feed(bytes) { out.append($0) }
        return out
    }

    func testLaunchpadPressesReadAsTheRealOne() {
        for row in 0..<9 {
            for col in 0..<9 {
                guard let down = VirtualLaunchpad.press(row: row, col: col, down: true),
                      let up = VirtualLaunchpad.press(row: row, col: col, down: false) else {
                    XCTAssertTrue(row == 0 && col == 8)       // the logo isn't a button
                    continue
                }
                let want: CtrlEvent
                if row == 0 { want = .launchpad(.top, x: UInt8(col), y: 0, down: true) }
                else if col == 8 { want = .launchpad(.side, x: 0, y: UInt8(row - 1), down: true) }
                else { want = .launchpad(.grid, x: UInt8(col), y: UInt8(row - 1), down: true) }
                XCTAssertEqual(LaunchpadMK3.event(parse(down)[0]), want, "\(row),\(col)")
                guard case let .launchpad(k, x, y, _) = want else { continue }
                XCTAssertEqual(LaunchpadMK3.event(parse(up)[0]), .launchpad(k, x: x, y: y, down: false))
                XCTAssertEqual(VirtualLaunchpad.ledIndex(row: row, col: col), LaunchpadMK3.index(k, x: Int(x), y: Int(y)))
            }
        }
    }

    func testLaunchpadShowsWhatTheAppLights() {
        var lp = VirtualLaunchpad()
        lp.receive(LaunchpadMK3.programmerMode)
        XCTAssertTrue(lp.programmerMode)
        let frame = (0..<CtrlReply.lpCount).map { UInt8(($0 * 7 + 1) % 128) }
        var diff = LEDDiffer(count: LaunchpadMK3.cells)
        let msgs = LaunchpadMK3.messages(frame, diff: &diff)
        for c in oddChunks(msgs.flatMap { $0 }) { lp.receive(c) }
        XCTAssertEqual(lp.leds, frame)
        XCTAssertEqual(lp.colour(row: 0, col: 8), frame[80])          // logo
        XCTAssertEqual(lp.colour(row: 1, col: 8), frame[64])          // top side button
        XCTAssertEqual(lp.colour(row: 8, col: 0), frame[56])          // bottom-left pad
        lp.receive(LaunchpadMK3.liveMode)
        XCTAssertFalse(lp.programmerMode)
        XCTAssertEqual(Set(lp.leds), [0])
        XCTAssertEqual(LaunchpadPalette.rgb.count, 128)
        XCTAssertEqual(LaunchpadPalette.components(21).g, 1)
    }

    func testLaunchControlXL() {
        var xl = VirtualLaunchControlXL()
        for row in 0..<3 {
            for col in 0..<8 {
                let m = xl.turn(row: row, col: col, to: 100)!
                XCTAssertEqual(m[0], 0xB8)
                XCTAssertEqual(LaunchControlXL.event(parse(m)[0]), .xlKnob(row: UInt8(row), col: UInt8(col), value: 100))
                XCTAssertNil(xl.turn(row: row, col: col, to: 100))     // didn't move
            }
        }
        for col in 0..<8 {
            let m = xl.slide(col: col, to: 5)!
            XCTAssertEqual(LaunchControlXL.event(parse(m)[0]), .xlFader(col: UInt8(col), value: 5))
        }
        for row in 0..<2 {
            for col in 0..<8 {
                let d = VirtualLaunchControlXL.press(row: row, col: col, down: true)!
                let u = VirtualLaunchControlXL.press(row: row, col: col, down: false)!
                XCTAssertEqual(LaunchControlXL.event(parse(d)[0]), .xlButton(row: UInt8(row), col: UInt8(col), down: true))
                XCTAssertEqual(LaunchControlXL.event(parse(u)[0]), .xlButton(row: UInt8(row), col: UInt8(col), down: false))
            }
        }
        XCTAssertEqual(ControllerKind.classify(midiName: VirtualDeviceNames.launchControlXL), .launchControlXL)
        XCTAssertEqual(ControllerKind.classify(midiName: VirtualDeviceNames.launchpad), .launchpad)

        let frame = (0..<CtrlReply.xlCount).map { LaunchControlXL.colour(red: UInt8($0 % 4), green: UInt8($0 / 4 % 4)) }
        var diff = LEDDiffer(count: LaunchControlXL.leds)
        xl.receive(LaunchControlXL.selectTemplate)
        for c in oddChunks(LaunchControlXL.messages(frame, diff: &diff).flatMap { $0 }) { xl.receive(c) }
        XCTAssertEqual(xl.leds, frame)
        XCTAssertEqual(xl.buttonLED(row: 1, col: 7), frame[39])
        let (r, g) = VirtualLaunchControlXL.components(LaunchControlXL.colour(red: 3, green: 1))
        XCTAssertEqual(r, 1); XCTAssertEqual(g, 1.0 / 3, accuracy: 1e-9)
    }

    func testGamepad() {
        var pad = VirtualGamepad()
        XCTAssertTrue(pad.set(.a, down: true))
        XCTAssertFalse(pad.set(.a, down: true))
        pad.set(.lt, down: true)
        XCTAssertEqual(XInput.mask(pad.held), 0x1000 | 1 << 16)
        pad.set(.a, down: false)
        XCTAssertEqual(pad.held, [.lt])
    }

    func testDaisyPatch() {
        var d = VirtualDaisyPatch()
        let cc = d.turn(2, to: 64)!
        XCTAssertNil(d.turn(2, to: 64))
        let hit = ControlElement.from(midi: parse(cc)[0])!
        XCTAssertEqual(hit.0, .midiCC(channel: 0, number: 23))
        XCTAssertEqual(hit.1, 64.0 / 127, accuracy: 1e-9)
        let on = d.setGate(true)!
        XCTAssertNil(d.setGate(true))
        XCTAssertEqual(ControlElement.from(midi: parse(on)[0])?.0, .midiNote(channel: 0, number: 60))
        XCTAssertEqual(d.setGate(false), [0x80, 60, 0])
        XCTAssertEqual(ControllerKind.classify(midiName: VirtualDeviceNames().daisyPatch), .midi)

        d.receive([0x91, 60, 100, 0xB0, 74])
        d.receive([20, 0xF8, 0xE0, 0x00, 0x40])
        XCTAssertEqual(d.monitor.map(\.text), ["Note on \(BeltTheory.name(60)) vel 100, ch 2", "CC 74 = 20, ch 1", "Clock",
                                               "Pitch bend 0, ch 1"])
        for i in 0..<60 { d.receive([0xB0, 1, UInt8(i)]) }
        XCTAssertEqual(d.monitor.count, VirtualDaisyPatch.monitorLimit)
        XCTAssertEqual(d.monitor.last?.text, "CC 1 = 59, ch 1")
        XCTAssertEqual(d.monitor.last?.id, 64)
    }
}
