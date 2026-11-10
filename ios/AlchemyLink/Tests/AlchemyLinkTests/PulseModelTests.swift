import XCTest
@testable import AlchemyLink

/// Pulse's own screen: the model against the descriptors the real module
/// sent (0.4.0 from Lab 2 on 2026-11-10, and the older 0.3.0), and the
/// maths against values worked from pulse_engine.cpp.
final class PulseModelTests: XCTestCase {
    private func descriptor(_ name: String) throws -> Descriptor {
        let url = try XCTUnwrap(Bundle.module.url(forResource: name, withExtension: "json"))
        return try Descriptor(json: Data(contentsOf: url))
    }

    func testDrivesOnlyAPulseWithEveryField() throws {
        let new = try descriptor("pulse_0_4_0.descriptor")
        XCTAssertTrue(Pulse.drives(new))
        XCTAssertEqual(new.moduleId, Pulse.moduleId)
        for o in 0..<Pulse.outs {
            for k in Pulse.Key.allCases { XCTAssertNotNil(new.field(Pulse.id(o, k)), Pulse.id(o, k)) }
        }
        XCTAssertFalse(Pulse.drives(try descriptor("pulse_0_3_0.descriptor")), "0.3.0 has no tempo or width fields")
        XCTAssertFalse(Pulse.drives(nil))
    }

    func testTempoKnobIsTenToThreeHundred() throws {
        let d = try descriptor("pulse_0_4_0.descriptor")
        let f = try XCTUnwrap(d.field(Pulse.tempoId))
        XCTAssertEqual(f.show(Pulse.knob(fromBpm: 120)), "120 BPM")
        XCTAssertEqual(Pulse.bpm(fromKnob: 0), 10)
        XCTAssertEqual(Pulse.bpm(fromKnob: 1), 300)
        XCTAssertEqual(Pulse.bpm(fromKnob: Pulse.knob(fromBpm: 97.5)), 97.5, accuracy: 1e-9)
        XCTAssertEqual(Pulse.knob(fromBpm: 900), 1)
    }

    func testEveryRatioLabelOnTheModuleParses() throws {
        let d = try descriptor("pulse_0_4_0.descriptor")
        let labels = try XCTUnwrap(d.field(Pulse.id(0, .ratio))?.disp.labels)
        XCTAssertEqual(labels.count, 26)
        let rates = labels.map { Pulse.cyclesPerBeat($0) }
        XCTAssertFalse(rates.contains(nil), "\(labels)")
        XCTAssertEqual(rates.first!!, 1.0 / 64, accuracy: 1e-12)
        XCTAssertEqual(rates[13]!, 1)                              // x1, the default
        XCTAssertEqual(rates[12]!, 1 / 1.5, accuracy: 1e-12)       // /1.5
        XCTAssertEqual(rates.last!!, 32)
        XCTAssertEqual(rates.compactMap { $0 }, rates.compactMap { $0 }.sorted(), "slowest to fastest")
        XCTAssertNil(Pulse.cyclesPerBeat("Off"))
    }

    func testWaveLabelsMatchTheEnum() throws {
        let d = try descriptor("pulse_0_4_0.descriptor")
        let labels = try XCTUnwrap(d.field(Pulse.id(0, .wave))?.disp.labels)
        XCTAssertEqual(labels, ["Gate", "Trig", "Saw", "Ramp", "Tri", "Sine", "Env", "Random", "Smooth"])
        XCTAssertEqual(labels.count, Pulse.Wave.allCases.count)
    }

    func testEuclidMatchesTheFirmware() {
        // (j * fills) % steps < fills, j = (i + rotate) % steps
        XCTAssertEqual(Pulse.pattern(steps: 8, fills: 3, rotate: 0), [true, false, false, true, false, false, true, false])
        XCTAssertEqual(Pulse.pattern(steps: 8, fills: 3, rotate: 1), [false, false, true, false, false, true, false, true])
        XCTAssertEqual(Pulse.pattern(steps: 4, fills: 4, rotate: 0), [true, true, true, true])
        XCTAssertEqual(Pulse.pattern(steps: 5, fills: 0, rotate: 0), [false, false, false, false, false])
        XCTAssertEqual(Pulse.pattern(steps: 0, fills: 0, rotate: 0), [])
        XCTAssertTrue(Pulse.euclidHit(7, steps: 0, fills: 0, rotate: 0), "Off plays every cycle")
        for steps in 1...Pulse.maxSteps {
            for fills in 0...steps {
                XCTAssertEqual(Pulse.pattern(steps: steps, fills: fills, rotate: 3 % steps).filter { $0 }.count, fills)
            }
        }
    }

    func testShapesMatchTheFirmware() {
        XCTAssertEqual(Pulse.shape(.gate, phase: 0.49, width: 0.5, cycleSeconds: 0.5), 1)
        XCTAssertEqual(Pulse.shape(.gate, phase: 0.51, width: 0.5, cycleSeconds: 0.5), 0)
        XCTAssertEqual(Pulse.shape(.gate, phase: 0.99, width: 1, cycleSeconds: 0.5), 1, "full width ties")
        XCTAssertEqual(Pulse.shape(.trig, phase: 0.01, width: 0, cycleSeconds: 1), 0, "5 ms of a 1 s cycle is over by 1%")
        XCTAssertEqual(Pulse.shape(.trig, phase: 0.004, width: 0, cycleSeconds: 1), 1)
        XCTAssertEqual(Pulse.shape(.trig, phase: 0.4, width: 1, cycleSeconds: 0.02), 1, "never longer than half the cycle")
        XCTAssertEqual(Pulse.shape(.trig, phase: 0.6, width: 1, cycleSeconds: 0.02), 0)
        XCTAssertEqual(Pulse.shape(.saw, phase: 0.25, width: 0.5, cycleSeconds: 1), 0.25)
        XCTAssertEqual(Pulse.shape(.ramp, phase: 0.25, width: 0.5, cycleSeconds: 1), 0.75)
        XCTAssertEqual(Pulse.shape(.tri, phase: 0.25, width: 0.5, cycleSeconds: 1), 0.5, accuracy: 1e-12)
        XCTAssertEqual(Pulse.shape(.tri, phase: 0.75, width: 0.5, cycleSeconds: 1), 0.5, accuracy: 1e-12)
        XCTAssertEqual(Pulse.shape(.sine, phase: 0.5, width: 0, cycleSeconds: 1), 1, accuracy: 1e-12)
        XCTAssertEqual(Pulse.shape(.sine, phase: 0, width: 0, cycleSeconds: 1), 0, accuracy: 1e-12)
        XCTAssertEqual(Pulse.shape(.env, phase: 0, width: 0.5, cycleSeconds: 1), 1)
        XCTAssertEqual(Pulse.shape(.env, phase: 0.33, width: 0.5, cycleSeconds: 1), exp(-1), accuracy: 1e-12)
        XCTAssertEqual(Pulse.shape(.stepRandom, phase: 0.3, width: 0, cycleSeconds: 1, held: 0.2, next: 0.9), 0.9)
        XCTAssertEqual(Pulse.shape(.smoothRandom, phase: 0.5, width: 0, cycleSeconds: 1, held: 0.2, next: 0.8), 0.5, accuracy: 1e-12)
    }

    func testOutReadsTheModulesValuesAndDrawsVolts() {
        var values: [String: Double] = [:]
        values[Pulse.id(2, .ratio)] = 15; values[Pulse.id(2, .wave)] = 2      // x2 saw
        values[Pulse.id(2, .level)] = 0.5; values[Pulse.id(2, .offset)] = 0.75 // 2.5 V high, +2.5 V
        values[Pulse.id(2, .steps)] = 4; values[Pulse.id(2, .fills)] = 2
        let o = Pulse.Out(2, values: values)
        XCTAssertEqual(o.ratio, 15); XCTAssertEqual(o.wave, .saw)
        XCTAssertEqual(o.offsetVolts, 2.5, accuracy: 1e-12)
        XCTAssertEqual(o.hits, [true, false, true, false])
        XCTAssertEqual(Pulse.Out(5, values: values), Pulse.Out(), "an output with nothing set reads as the defaults")

        let c = o.curve(points: 401, cycles: 4, cycleSeconds: 0.25)
        XCTAssertEqual(c.count, 401)
        XCTAssertEqual(c[0], 2.5, accuracy: 1e-9, "a saw starts at the offset")
        XCTAssertEqual(c[50], 2.5 + 2.5 * 0.5, accuracy: 0.02, "half way up the first cycle")
        XCTAssertEqual(c[150], 2.5, accuracy: 1e-9, "the second cycle is a rest: it sits at the offset")
        XCTAssertTrue(c.allSatisfy { $0 >= -5 && $0 <= 5 })

        var loud = Pulse.Out(); loud.offset = 1; loud.level = 1
        XCTAssertEqual(loud.curve(points: 10, cycles: 1, cycleSeconds: 0.5).max(), 5, "clipped at the jack's +5 V")
    }

    func testRandomPatternsAreAlwaysValid() {
        var rng = SystemRandomNumberGenerator()
        for _ in 0..<500 {
            let p = Pulse.randomPattern(using: &rng)
            XCTAssertTrue((3...16).contains(p.steps))
            XCTAssertTrue((1...p.steps).contains(p.fills))
            XCTAssertTrue((0..<p.steps).contains(p.rotate))
        }
    }
}
