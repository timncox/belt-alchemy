import XCTest
@testable import AlchemyLink

final class LinkingTests: XCTestCase {
    /// The app's field ids are the firmware's own: the first twelve
    /// Ident("...") in src/belt_alchemy.cpp, in order. The pretend module
    /// and the app once agreed on "p0.0" while real Belt said "key", so
    /// sharing key and scale did nothing on hardware.
    func testFieldIdsAreTheFirmwares() throws {
        let src = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("src/belt_alchemy.cpp")
        let text = try String(contentsOf: src, encoding: .utf8)
        var ids: [String] = []
        for part in text.components(separatedBy: ".Ident(\"").dropFirst() {
            if let end = part.firstIndex(of: "\"") { ids.append(String(part[..<end])) }
        }
        XCTAssertEqual(Array(ids.prefix(12)), BeltFields.perf)
        let desc = try Descriptor(json: Data(FakeModule().descJSON))
        for id in BeltFields.perf { XCTAssertNotNil(desc.field(id), id) }
    }

    let L: UInt32 = 0xA, R: UInt32 = 0xB

    func testSharedKey() {
        var s = LinkSettings()
        XCTAssertTrue(LinkEngine.propagate(unit: L, field: BeltFields.key, value: 0.3, settings: s, units: [L, R]).isEmpty)
        s.shareKey = true
        let out = LinkEngine.propagate(unit: R, field: BeltFields.scale, value: 0.4, settings: s, units: [L, R])
        XCTAssertEqual(out, [ControlTarget(unit: L, field: BeltFields.scale): 0.4])
        // Split harmonies shares key too.
        s.shareKey = false; s.splitHarmonies = true
        XCTAssertEqual(LinkEngine.propagate(unit: L, field: BeltFields.key, value: 0.7, settings: s, units: [L, R]).count, 1)
    }

    func testFollowWithRange() {
        var s = LinkSettings()
        s.follows = [Follow(from: ControlTarget(unit: L, field: "harm_level"), to: ControlTarget(unit: R, field: "double_amt"), lo: 0.2, hi: 0.8)]
        let out = LinkEngine.propagate(unit: L, field: "harm_level", value: 0.5, settings: s, units: [L, R])
        XCTAssertEqual(out[ControlTarget(unit: R, field: "double_amt")]!, 0.5, accuracy: 1e-9)
        // One way: Right's knob doesn't drive Left.
        XCTAssertTrue(LinkEngine.propagate(unit: R, field: "double_amt", value: 0.1, settings: s, units: [L, R]).isEmpty)
        // A module that isn't here gets nothing.
        XCTAssertTrue(LinkEngine.propagate(unit: L, field: "harm_level", value: 0.5, settings: s, units: [L]).isEmpty)
    }

    func testLoopsRefused() {
        let a = ControlTarget(unit: L, field: "retune"), b = ControlTarget(unit: R, field: "retune"), c = ControlTarget(unit: R, field: "amount")
        let existing = [Follow(from: a, to: b), Follow(from: b, to: c)]
        XCTAssertTrue(LinkEngine.makesLoop(Follow(from: c, to: a), with: existing))
        XCTAssertFalse(LinkEngine.makesLoop(Follow(from: c, to: ControlTarget(unit: L, field: "formant")), with: existing))
    }

    func testSplitHarmonies() {
        let w = LinkEngine.splitWrites(plan: [7, 9, 11, 1], units: [L, R])
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: L, field: "harm1")]!), 7)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: L, field: "harm2")]!), 9)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: L, field: "harm3")]!), 0)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: R, field: "harm1")]!), 11)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: R, field: "harm2")]!), 1)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: R, field: "harm4")]!), 0)
        XCTAssertEqual(LinkEngine.planSlot(unit: R, field: "harm2", units: [L, R]), 3)
        XCTAssertNil(LinkEngine.planSlot(unit: R, field: "harm3", units: [L, R]))
        XCTAssertEqual(LinkEngine.plan(fromLeftVoices: [7.5 / 12, 9.5 / 12, 0.5 / 12, 0.5 / 12]), [7, 9, 0, 0])
        XCTAssertTrue(LinkEngine.splitWrites(plan: [7, 9, 0, 0], units: [L]).isEmpty)
    }

    /// The pretend modules agree: the voice value written is the one the
    /// singer's harmony reads back.
    func testSplitOnTheDemoModules() async throws {
        let a = HostLinkClient(transport: FakeModule(viaSysEx: true, unit: L))
        let d = try await a.descriptor()
        let w = LinkEngine.splitWrites(plan: [5, 11, 0, 0], units: [L, R])
        var changes: [String: Double] = [:]
        for (t, v) in w where t.unit == L { changes[t.field] = v }
        let cur = try await a.getLive()
        try await a.setLive(d.encode(cur, changes: changes))
        let st = try await a.state(since: nil)
        XCTAssertEqual(st.belt?.voices, [5, 11, 0, 0])
    }
}
