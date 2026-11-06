import XCTest
@testable import AlchemyLink

final class LinkingTests: XCTestCase {
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
        s.follows = [Follow(from: ControlTarget(unit: L, field: "p0.4"), to: ControlTarget(unit: R, field: "p1.4"), lo: 0.2, hi: 0.8)]
        let out = LinkEngine.propagate(unit: L, field: "p0.4", value: 0.5, settings: s, units: [L, R])
        XCTAssertEqual(out[ControlTarget(unit: R, field: "p1.4")]!, 0.5, accuracy: 1e-9)
        // One way: Right's knob doesn't drive Left.
        XCTAssertTrue(LinkEngine.propagate(unit: R, field: "p1.4", value: 0.1, settings: s, units: [L, R]).isEmpty)
        // A module that isn't here gets nothing.
        XCTAssertTrue(LinkEngine.propagate(unit: L, field: "p0.4", value: 0.5, settings: s, units: [L]).isEmpty)
    }

    func testLoopsRefused() {
        let a = ControlTarget(unit: L, field: "p0.2"), b = ControlTarget(unit: R, field: "p0.2"), c = ControlTarget(unit: R, field: "p0.3")
        let existing = [Follow(from: a, to: b), Follow(from: b, to: c)]
        XCTAssertTrue(LinkEngine.makesLoop(Follow(from: c, to: a), with: existing))
        XCTAssertFalse(LinkEngine.makesLoop(Follow(from: c, to: ControlTarget(unit: L, field: "p0.5")), with: existing))
    }

    func testSplitHarmonies() {
        let w = LinkEngine.splitWrites(plan: [7, 9, 11, 1], units: [L, R])
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: L, field: "p1.0")]!), 7)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: L, field: "p1.1")]!), 9)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: L, field: "p1.2")]!), 0)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: R, field: "p1.0")]!), 11)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: R, field: "p1.1")]!), 1)
        XCTAssertEqual(BeltFields.voiceIndex(w[ControlTarget(unit: R, field: "p1.3")]!), 0)
        XCTAssertEqual(LinkEngine.planSlot(unit: R, field: "p1.1", units: [L, R]), 3)
        XCTAssertNil(LinkEngine.planSlot(unit: R, field: "p1.2", units: [L, R]))
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
