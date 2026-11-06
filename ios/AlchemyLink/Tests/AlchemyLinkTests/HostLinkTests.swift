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
        XCTAssertEqual(desc.pages.map(\.name), ["Play", "Space", "Setup"])
        XCTAssertTrue(desc.hasStorage)
        XCTAssertTrue(desc.field("usb")!.severs)

        // Knobs: read, change one, read back.
        var live = try await link.getLive()
        XCTAssertEqual(live.count, desc.size)
        var v = desc.decode(live)
        XCTAssertEqual(v["p0.0"]!, 0.25, accuracy: 1e-6)
        live = desc.encode(live, changes: ["p0.2": 0.875, "s0.1": 1])
        try await link.setLive(live)
        let after = try await link.getLive()
        v = desc.decode(after)
        XCTAssertEqual(v["p0.2"]!, 0.875, accuracy: 1e-6)
        XCTAssertEqual(desc.field("s0.1")!.show(v["s0.1"]!), "Minor")

        // Presets.
        try await link.saveToSlot(3)
        let slots = try await link.listSlots()
        XCTAssertTrue(slots[3].valid)

        // Screen.
        let info = try await link.phoneInfo()
        XCTAssertEqual(info?.ledCount, 102)
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
}
