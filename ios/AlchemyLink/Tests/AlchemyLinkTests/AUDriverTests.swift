import XCTest
@testable import AlchemyLink

/// The AUv3's per-module driver against the pretend module, and two hosts
/// (the app and the extension) sharing one module.
final class AUDriverTests: XCTestCase {
    func testReadsBeltAsParameters() async throws {
        let module = FakeModule(viaSysEx: true)
        let driver = BeltParamDriver(client: HostLinkClient(transport: module), controlPeriod: 1.0 / 60)
        let unit = try await driver.start()
        XCTAssertEqual(unit, 0xBE17_0001)
        let v = try await driver.read()
        XCTAssertEqual(v[0], 9)                         // Key A
        XCTAssertEqual(v[1], 1)                         // Major
        XCTAssertEqual(v[2]!, 20, accuracy: 1e-3)       // Retune 20 %
        XCTAssertEqual(v[5]!, 0, accuracy: 1e-3)        // Formant centre
        XCTAssertEqual(v[6], 7)                         // Voice 1 +3rd
        XCTAssertEqual(v[7], 9)                         // Voice 2 +5th
        XCTAssertEqual(v[8], 0)                         // Voice 3 off
        XCTAssertEqual(v[12], 0)
        XCTAssertEqual(v[13], 0)
        XCTAssertEqual(v.count, 14)
    }

    func testFastAutomationIsCoalesced() async throws {
        let module = FakeModule(viaSysEx: true)
        let driver = BeltParamDriver(client: HostLinkClient(transport: module), controlPeriod: 1.0 / 60)
        try await driver.start()
        for i in 0...200 { driver.submit(param: 2, value: Float(i) / 2) }   // Retune 0 ... 100 %
        driver.submit(param: 0, value: 4)                                   // Key E
        await driver.idle()
        XCTAssertLessThan(driver.writes, 20, "a lane of 200 moves should take a handful of writes")
        let v = try await driver.read()
        XCTAssertEqual(v[2]!, 100, accuracy: 1e-3)
        XCTAssertEqual(v[0], 4)
        // The module itself has it, stored the way the app stores it.
        let desc = try Descriptor(json: Data(module.descJSON))
        XCTAssertEqual(desc.decode(module.live)["p0.0"]!, 4.5 / 12, accuracy: 1e-6)
    }

    func testHardAndMuteArePressedOnlyWhenTheyChange() async throws {
        let module = FakeModule(viaSysEx: true)
        let driver = BeltParamDriver(client: HostLinkClient(transport: module), controlPeriod: 1.0 / 60)
        try await driver.start()
        driver.submit(param: 12, value: 1)
        await driver.idle()
        XCTAssertEqual(module.presses.map { $0.0 }, [0])
        XCTAssertEqual(module.presses.map { $0.1 }, [HostLinkClient.PressGesture.tap.rawValue])
        driver.submit(param: 12, value: 1)            // already on: no press
        driver.submit(param: 13, value: 0)            // already off: no press
        await driver.idle()
        XCTAssertEqual(module.presses.count, 1)
        var v = try await driver.read()
        XCTAssertEqual(v[12], 1)
        XCTAssertEqual(v[13], 0)
        driver.submit(param: 13, value: 1)
        driver.submit(param: 12, value: 0)
        await driver.idle()
        v = try await driver.read()
        XCTAssertEqual(v[12], 0)
        XCTAssertEqual(v[13], 1)
        XCTAssertEqual(module.presses.count, 3)
    }

    func testARecentWriteOutranksAnOlderRead() async throws {
        let module = FakeModule(viaSysEx: true)
        let client = HostLinkClient(transport: module)
        let driver = BeltParamDriver(client: client, controlPeriod: 0)
        try await driver.start()
        driver.submit(param: 3, value: 10)
        await driver.idle()
        // Someone else (the app, the panel) moves Amount back right after.
        let desc = try Descriptor(json: Data(module.descJSON))
        try await client.setLive(desc.encode(module.live, changes: ["p0.3": 0.9]))
        var v = try await driver.read()
        XCTAssertEqual(v[3]!, 10, accuracy: 1e-3, "inside the hold our write is what we report")
        driver.writeHold = 0
        v = try await driver.read()
        XCTAssertEqual(v[3]!, 90, accuracy: 1e-3, "after it, the module's value")
    }

    func testOtherFirmwareGetsNoParameters() async throws {
        // A module whose descriptor lacks Belt's fields is refused.
        final class NotBelt: ByteTransport, @unchecked Sendable {
            var h: (([UInt8]) -> Void)?
            var split = FrameSplitter()
            let json = Array(#"{"module":{"id":"smack","name":"Smack"},"size":4,"components":[{"id":"x","kind":"pager","off":0,"fields":[{"id":"p0.0","off":0,"type":"f32"}]}]}"#.utf8)
            func setReceiveHandler(_ h: @escaping ([UInt8]) -> Void) { self.h = h }
            func send(_ bytes: [UInt8]) {
                var frames: [Frame] = []
                for b in bytes { split.feed(b) { frames.append($0) } }
                for f in frames {
                    var r = Reader(f.body)
                    let w: Writer
                    switch Cmd(rawValue: f.type) {
                    case .hello:
                        w = Writer().u8(0).u8(1).u8(2).u8(16).u8(0).raw([UInt8](repeating: 0, count: 12)).u32(1).u32(0)
                            .u32(UInt32(json.count)).u32(CRC32.of(json)).u16(512).u16(4)
                            .str("smack").str("Smack").str("1").str("0").str("0")
                    case .descriptor:
                        let off = Int(try! r.u32()), n = min(Int(try! r.u16()), json.count - off)
                        w = Writer().u8(0).u32(UInt32(off)).u16(UInt16(n)).raw(json[off..<(off + n)])
                    default: w = Writer().u8(Status.unsupported.rawValue)
                    }
                    let reply = Frame(type: f.type | 0x80, seq: f.seq, body: w.bytes).wire()
                    let h = self.h
                    DispatchQueue.global().async { h?(reply) }
                }
            }
        }
        let driver = BeltParamDriver(client: HostLinkClient(transport: NotBelt()), controlPeriod: 0)
        do { try await driver.start(); XCTFail("should refuse") }
        catch let e as BeltParamDriver.DriverError { XCTAssertTrue(e.description.contains("Smack")) }
    }

    /// One module heard by two hosts: every reply goes to both, as the USB
    /// MIDI stream does when the app and the extension each connect.
    final class SharedModule: @unchecked Sendable {
        final class Port: ByteTransport, @unchecked Sendable {
            weak var owner: SharedModule?
            var handler: (([UInt8]) -> Void)?
            let lock = NSLock()
            func setReceiveHandler(_ h: @escaping ([UInt8]) -> Void) { lock.lock(); handler = h; lock.unlock() }
            func send(_ bytes: [UInt8]) { owner?.module.send(bytes) }
            func hear(_ bytes: [UInt8]) { lock.lock(); let h = handler; lock.unlock(); h?(bytes) }
        }
        let module = FakeModule(viaSysEx: true)
        let ports = [Port(), Port()]
        init() {
            for p in ports { p.owner = self }
            module.setReceiveHandler { [ports] bytes in for p in ports { p.hear(bytes) } }
        }
    }

    func testTwoHostsShareOneModule() async throws {
        let shared = SharedModule()
        let app = HostLinkClient(transport: shared.ports[0])                    // the app, untagged
        let tagged = SeqTagTransport(shared.ports[1])
        let au = HostLinkClient(transport: tagged)                              // the extension
        // Both number from the same place and ask the same command at the
        // same moments, each for a different folder: without the tag, one
        // would take the other's listing.
        _ = try await app.sayHello()
        _ = try await au.sayHello()
        async let a: Int = {
            var n = 0
            for _ in 0..<40 where try await app.fsList("/samples").contains(where: { $0.name == "kick.wav" }) { n += 1 }
            return n
        }()
        async let b: Int = {
            var n = 0
            for _ in 0..<40 where try await au.fsList("/alchemy").contains(where: { $0.name == "belt_alchemy.bin" }) { n += 1 }
            return n
        }()
        let (na, nb) = try await (a, b)
        XCTAssertEqual(na, 40)
        XCTAssertEqual(nb, 40)
        XCTAssertGreaterThan(tagged.dropped, 0, "the app's replies reached the extension and were dropped")
        let desc = try await au.descriptor()
        let live = try await au.getLive()
        XCTAssertEqual(desc.decode(live).count, desc.fields.count)
    }

    func testTagsStayInTheirHalf() {
        XCTAssertEqual(SeqTagTransport.wireSeq(2), 0x8002)
        XCTAssertEqual(SeqTagTransport.wireSeq(0x8005), 0x8005)
        XCTAssertEqual(SeqTagTransport.wireSeq(0xFFFF), 0xFFFF)
    }
}
