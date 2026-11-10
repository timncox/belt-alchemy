import XCTest
@testable import AlchemyLink

/// The AUv3's parameter scheme: fixed addresses, value ranges, the CC map.
final class AUParamsTests: XCTestCase {
    func testAddressesRoundTrip() {
        XCTAssertEqual(AUParams.count, 28)
        XCTAssertEqual(Set(AUParams.allAddresses).count, 28)
        for slot in 0..<2 {
            for p in 0..<14 {
                let a = AUParams.address(slot: slot, param: p)
                let d = AUParams.decode(a)
                XCTAssertEqual(d?.slot, slot)
                XCTAssertEqual(d?.param, p)
                XCTAssertEqual(AUParams.address(index: AUParams.index(of: a)!), a)
            }
        }
        // The documented values, which hosts store: never change them.
        XCTAssertEqual(AUParams.address(slot: 0, param: 0), 0x000)
        XCTAssertEqual(AUParams.address(slot: 0, param: 13), 0x00D)
        XCTAssertEqual(AUParams.address(slot: 1, param: 2), 0x102)
        XCTAssertEqual(AUParams.address(slot: 1, param: 13), 0x10D)
        XCTAssertNil(AUParams.decode(0x00E))
        XCTAssertNil(AUParams.decode(0x200))
        XCTAssertNil(AUParams.index(of: 0xFFFF))
    }

    func testSpecsMatchBelt() {
        let s = AUParams.specs
        XCTAssertEqual(s.map(\.name), ["Key", "Scale", "Retune", "Amount", "Harmony", "Formant",
                                       "Voice 1", "Voice 2", "Voice 3", "Voice 4", "Doubler", "Spread", "HARD", "Mute"])
        XCTAssertEqual(s.compactMap(\.fieldId), BeltFields.perf)
        // The same fields, by name, as the pretend module's descriptor.
        let desc = try! Descriptor(json: Data(FakeModule().descJSON))
        for spec in s where spec.fieldId != nil {
            XCTAssertEqual(desc.field(spec.fieldId!)?.name, spec.name)
        }
        XCTAssertEqual(s[12].button, 0)
        XCTAssertEqual(s[13].button, 1)
        XCTAssertEqual(AUParams.identifier(0x109), "moduleB_voice4")
        XCTAssertEqual(AUParams.displayName(0x003), "A Amount")
    }

    func testRangesAndStrings() {
        let key = AUParams.specs[0], formant = AUParams.specs[5], retune = AUParams.specs[2], hard = AUParams.specs[12]
        XCTAssertEqual(key.minValue, 0); XCTAssertEqual(key.maxValue, 11)
        XCTAssertEqual(key.valueStrings, BeltTheory.keys)
        XCTAssertEqual(AUParams.specs[1].maxValue, 8)
        XCTAssertEqual(AUParams.specs[1].valueStrings, BeltTheory.scales)
        XCTAssertEqual(AUParams.specs[6].valueStrings, BeltTheory.intervals)
        XCTAssertEqual(formant.minValue, -100); XCTAssertEqual(formant.maxValue, 100)
        XCTAssertEqual(retune.minValue, 0); XCTAssertEqual(retune.maxValue, 100)
        XCTAssertEqual(hard.minValue, 0); XCTAssertEqual(hard.maxValue, 1)
        for s in AUParams.specs {
            XCTAssertGreaterThanOrEqual(s.defaultValue, s.minValue)
            XCTAssertLessThanOrEqual(s.defaultValue, s.maxValue)
        }
        XCTAssertEqual(AUParams.valueString(0x000, 9), "A")
        XCTAssertEqual(AUParams.valueString(0x101, 1), "Major")
        XCTAssertEqual(AUParams.valueString(0x007, 7), "+3rd")
        XCTAssertEqual(AUParams.valueString(0x002, 37.4), "37 %")
        XCTAssertEqual(AUParams.valueString(0x005, 12), "+12")
        XCTAssertEqual(AUParams.valueString(0x005, -40), "-40")
        XCTAssertEqual(AUParams.valueString(0x10C, 1), "On")
    }

    func testModuleValuesRoundTrip() {
        // Indexed: every index to the middle of its zone and back.
        for (p, n) in [(0, 12), (1, 9), (6, 12), (9, 12)] {
            for i in 0..<n {
                let norm = AUParams.moduleValue(fromAU: Float(i), param: p)
                XCTAssertEqual(norm, (Double(i) + 0.5) / Double(n), accuracy: 1e-9)
                XCTAssertEqual(AUParams.auValue(fromModule: norm, param: p), Float(i))
            }
            // The zone edges as the module may hold them.
            XCTAssertEqual(AUParams.auValue(fromModule: 0, param: p), 0)
            XCTAssertEqual(AUParams.auValue(fromModule: 1, param: p), Float(n - 1))
        }
        // Voices agree with the app's own encoding.
        XCTAssertEqual(AUParams.moduleValue(fromAU: 7, param: 6), BeltFields.voiceValue(7), accuracy: 1e-9)
        // Continuous.
        XCTAssertEqual(AUParams.moduleValue(fromAU: 25, param: 2), 0.25, accuracy: 1e-9)
        XCTAssertEqual(AUParams.auValue(fromModule: 0.8, param: 3), 80, accuracy: 1e-4)
        XCTAssertEqual(AUParams.moduleValue(fromAU: -100, param: 5), 0, accuracy: 1e-9)
        XCTAssertEqual(AUParams.moduleValue(fromAU: 0, param: 5), 0.5, accuracy: 1e-9)
        XCTAssertEqual(AUParams.auValue(fromModule: 0.75, param: 5), 50, accuracy: 1e-4)
        // Out of range is clamped.
        XCTAssertEqual(AUParams.moduleValue(fromAU: 140, param: 2), 1, accuracy: 1e-9)
        XCTAssertEqual(AUParams.moduleValue(fromAU: 20, param: 0), 11.5 / 12, accuracy: 1e-9)
        XCTAssertEqual(AUParams.moduleValue(fromAU: 1, param: 12), 1)
        XCTAssertEqual(AUParams.moduleValue(fromAU: 0.2, param: 13), 0)
    }

    func testCCMap() {
        XCTAssertEqual(AUParams.address(forCC: 20), 0x000)
        XCTAssertEqual(AUParams.address(forCC: 33), 0x00D)
        XCTAssertEqual(AUParams.address(forCC: 52), 0x100)
        XCTAssertEqual(AUParams.address(forCC: 65), 0x10D)
        for cc: UInt8 in [0, 1, 7, 19, 34, 51, 64 + 2, 127] { XCTAssertNil(AUParams.address(forCC: cc)) }
        for a in AUParams.allAddresses { XCTAssertEqual(AUParams.address(forCC: AUParams.cc(for: a)!), a) }
        let table = AUParams.ccIndexTable()
        XCTAssertEqual(table.count, 128)
        for cc in 0..<128 {
            XCTAssertEqual(table[cc], AUParams.address(forCC: UInt8(cc)).flatMap { AUParams.index(of: $0) } ?? -1)
        }
        XCTAssertEqual(table.filter { $0 >= 0 }.count, 28)

        // Values: the whole range for every kind.
        XCTAssertEqual(AUParams.auValue(fromCC: 0, param: 2), 0)
        XCTAssertEqual(AUParams.auValue(fromCC: 127, param: 2), 100)
        XCTAssertEqual(AUParams.auValue(fromCC: 0, param: 5), -100)
        XCTAssertEqual(AUParams.auValue(fromCC: 127, param: 5), 100)
        XCTAssertEqual(AUParams.auValue(fromCC: 0, param: 0), 0)
        XCTAssertEqual(AUParams.auValue(fromCC: 127, param: 0), 11)
        XCTAssertEqual(AUParams.auValue(fromCC: 127, param: 1), 8)
        XCTAssertEqual(AUParams.auValue(fromCC: 63, param: 12), 0)
        XCTAssertEqual(AUParams.auValue(fromCC: 64, param: 12), 1)
        // Each of 12 zones gets an even share of the CC range.
        let hits = (0...127).map { Int(AUParams.auValue(fromCC: UInt8($0), param: 6)) }
        for i in 0..<12 { XCTAssertTrue((10...11).contains(hits.filter { $0 == i }.count)) }
    }

    func testSlotsByUnit() {
        XCTAssertEqual(AUParams.assignSlots([0xBE17_0002, 0xBE17_0001]), [0xBE17_0001, 0xBE17_0002])
        XCTAssertEqual(AUParams.assignSlots([9, 3, 5]), [3, 5])
        XCTAssertEqual(AUParams.assignSlots([7]), [7])
    }

    func testHostWinsForAMoment() {
        var sync = AUParamSync(hold: 1)
        let a = AUParams.address(slot: 0, param: 2), b = AUParams.address(slot: 1, param: 0)
        sync.hostChanged(a, at: 10)
        let read: [UInt64: Float] = [a: 20, b: 3]
        let current: [UInt64: Float] = [a: 90, b: 0]
        // Inside the hold: the host's lane stays; the other moves.
        XCTAssertEqual(sync.readback(read, current: { current[$0] }, at: 10.5), [b: 3])
        // After it: the module's value is shown again.
        XCTAssertEqual(sync.readback(read, current: { current[$0] }, at: 11.1), [a: 20, b: 3])
        // Unchanged values aren't sent back to the host.
        XCTAssertEqual(sync.readback(read, current: { _ in 20 }, at: 12)[a], nil)
        sync.prune(at: 12)
        XCTAssertFalse(sync.hostOwns(a, at: 12))
    }
}
