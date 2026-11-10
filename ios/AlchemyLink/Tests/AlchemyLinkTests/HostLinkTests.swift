import XCTest
@testable import AlchemyLink

/// The client against the pretend module, every byte through the SysEx
/// pipe: the same path the app takes to a real module.
final class HostLinkTests: XCTestCase {
    func testWholeSession() async throws {
        let module = FakeModule(viaSysEx: true)
        let link = HostLinkClient(transport: module)

        let h = try await link.sayHello()
        XCTAssertEqual(h.moduleId, "belt_alchemy")
        let desc = try await link.descriptor()
        XCTAssertEqual(desc.pages.map(\.name), ["Play", "Setup", "Settings"])
        XCTAssertTrue(desc.hasStorage)
        XCTAssertTrue(desc.field("usb")!.severs)

        // Knobs: read, change one, read back.
        var live = try await link.getLive()
        XCTAssertEqual(live.count, desc.size)
        var v = desc.decode(live)
        XCTAssertEqual(desc.field("key")!.show(v["key"]!), "A")
        XCTAssertEqual(desc.field("scale")!.show(v["scale"]!), "Major")
        live = desc.encode(live, changes: ["retune": 0.875, "s0.1": 1])
        try await link.setLive(live)
        let after = try await link.getLive()
        v = desc.decode(after)
        XCTAssertEqual(v["retune"]!, 0.875, accuracy: 1e-6)
        XCTAssertEqual(desc.field("s0.1")!.show(v["s0.1"]!), "Normal")

        // Presets.
        try await link.saveToSlot(3)
        let slots = try await link.listSlots()
        XCTAssertTrue(slots[3].valid)

        // Screen.
        let info = try await link.phoneInfo()
        XCTAssertEqual(info?.ledCount, 102)
        XCTAssertEqual(info?.version, 2)
        XCTAssertEqual(info?.hasState, true)
        XCTAssertEqual(info?.unit, 0xBE17_0001)
        let s = try await link.screen()
        XCTAssertEqual(s.rgb.count, 306)

        // Samples: upload, list, delete.
        let wav = (0..<5000).map { UInt8($0 & 0xFF) }
        try await link.fsUpload(wav, to: "/samples/new.wav")
        let listed = try await link.fsList("/samples")
        XCTAssertTrue(listed.contains { $0.name == "new.wav" && $0.size == 5000 })
        try await link.fsDelete("/samples/new.wav")
        let gone = try await link.fsList("/samples")
        XCTAssertFalse(gone.contains { $0.name == "new.wav" })

        // Firmware: list, start, watch it finish.
        let bins = try await link.fwList()
        XCTAssertTrue(bins.contains { $0.name == "smack_alchemy.bin" })
        try await link.fwFlash("smack_alchemy.bin")
        var st = try await link.fwStatus()
        var polls = 0
        while st.state != .done && polls < 100 { st = try await link.fwStatus(); polls += 1 }
        XCTAssertEqual(st.state, .done)

        // A bad name comes back as a status, not a hang.
        do { try await link.fwFlash("nope.bin"); XCTFail("should fail") }
        catch let e as HostLinkError { XCTAssertEqual(e.statusCode, .noFile) }
    }

    func testPanelLayoutCoversEveryLed() {
        XCTAssertEqual(PanelLayout.leds.map(\.index), Array(0..<102))
        XCTAssertEqual(PanelLayout.leds.filter { $0.pot != nil }.count, 96)
    }

    func testStateFromThePretendSinger() async throws {
        let module = FakeModule(viaSysEx: true)
        let link = HostLinkClient(transport: module)
        _ = try await link.sayHello()
        try await Task.sleep(nanoseconds: 700_000_000)
        let st = try await link.state(since: nil)
        let b = try XCTUnwrap(st.belt)
        XCTAssertEqual(b.key, 9)            // A
        XCTAssertEqual(b.scale, 1)          // Major
        XCTAssertEqual(b.voices, [7, 9, 0, 0])
        XCTAssertEqual(b.trace.count, 32)
        XCTAssertNil(st.touchPot)
        if let s = b.sung { XCTAssertGreaterThan(s, 40); XCTAssertLessThan(s, 90) }

        // Asking from the last frame seen gets only what is new.
        try await Task.sleep(nanoseconds: 100_000_000)
        let next = try await link.state(since: b.traceLast)
        let nb = try XCTUnwrap(next.belt)
        XCTAssertGreaterThan(nb.trace.count, 0)
        XCTAssertLessThan(nb.trace.count, 32)
        XCTAssertEqual(nb.traceLast &- b.traceLast, UInt16(nb.trace.count))

        // HARD latches from a tap, mute holds while down.
        try await link.press(button: 0, .tap)
        try await link.press(button: 1, .down)
        var s2 = try await link.state(since: nil)
        var b2 = try XCTUnwrap(s2.belt)
        XCTAssertTrue(b2.hard && b2.hardLatched && b2.muted && !b2.muteLatched)
        try await link.press(button: 1, .up)
        s2 = try await link.state(since: nil)
        b2 = try XCTUnwrap(s2.belt)
        XCTAssertFalse(b2.muted)

        // A knob turned from the phone shows up as the touched one.
        let desc = try await link.descriptor()
        let cur = try await link.getLive()
        let live = desc.encode(cur, changes: ["double_amt": 0.9])
        try await link.setLive(live)
        let t = try await link.state(since: nil)
        XCTAssertEqual(t.touchPage, 1)
        XCTAssertEqual(t.touchPot, 4)
    }

    func testTwoModulesTellApart() async throws {
        let a = HostLinkClient(transport: FakeModule(viaSysEx: true, unit: 0xA, name: "Belt A"))
        let b = HostLinkClient(transport: FakeModule(viaSysEx: true, unit: 0xB, name: "Belt B"))
        let ia = try await a.phoneInfo(), ib = try await b.phoneInfo()
        XCTAssertNotEqual(ia?.unit, ib?.unit)
        let ha = try await a.sayHello(), hb = try await b.sayHello()
        XCTAssertEqual(ha.moduleId, hb.moduleId)
    }

    func testBeltTheory() {
        XCTAssertEqual(BeltTheory.name(60), "C4")
        XCTAssertEqual(BeltTheory.name(69), "A4")
        XCTAssertTrue(BeltTheory.inScale(61, key: 9, scale: 1))   // C# in A major
        XCTAssertFalse(BeltTheory.inScale(60, key: 9, scale: 1))  // C is not
        // A 3rd above A in A major is C#, a 5th is E.
        XCTAssertEqual(BeltTheory.harmTarget(69, interval: 7, key: 9, scale: 1), 73, accuracy: 0.01)
        XCTAssertEqual(BeltTheory.harmTarget(69, interval: 9, key: 9, scale: 1), 76, accuracy: 0.01)
        XCTAssertEqual(BeltTheory.harmTarget(69, interval: 11, key: 9, scale: 1), 81, accuracy: 0.01)
    }
}
