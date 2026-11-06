import XCTest
@testable import AlchemyLink

final class ControlsTests: XCTestCase {
    func testRoutesAcrossTwoModules() {
        let a = ControlTarget(unit: 1, field: "p0.2"), b = ControlTarget(unit: 2, field: "p1.4")
        var m = ControlMatrix(routes: [
            ControlRoute(source: .padX, target: a),
            ControlRoute(source: .padY, target: b, lo: 1, hi: 0.2),     // upside down, partial
            ControlRoute(source: .pressure, target: a, lo: 0.5, hi: 1),  // later route wins on a
        ])
        var out = m.apply([.padX: 0.25, .padY: 0.5])
        XCTAssertEqual(out[a]!, 0.25, accuracy: 1e-9)
        XCTAssertEqual(out[b]!, 0.6, accuracy: 1e-9)
        out = m.apply([.padX: 0.25, .pressure: 1])
        XCTAssertEqual(out[a]!, 1, accuracy: 1e-9)
        m.prune { $0.unit == 1 }
        XCTAssertEqual(m.routes.count, 2)
        XCTAssertEqual(m.targets(for: .padY).count, 0)
    }

    func testAngles() {
        var s = AngleShaper(rangeDegrees: 45, zero: 0.1)
        XCTAssertEqual(s.value(0.1), 0.5, accuracy: 1e-9)
        XCTAssertEqual(s.value(0.1 + .pi / 4), 1, accuracy: 1e-9)
        XCTAssertEqual(s.value(0.1 - .pi / 8), 0.25, accuracy: 1e-9)
        s.zero = .pi - 0.05                      // turn wraps round
        XCTAssertEqual(s.value(-.pi + 0.05), 0.5 + 0.1 * 180 / .pi / 90, accuracy: 1e-6)
    }

    func testSmootherAndLevel() {
        var sm = Smoother(seconds: 0.1)
        XCTAssertEqual(sm.step(0, dt: 0.01), 0)
        let v = sm.step(1, dt: 0.1)
        XCTAssertEqual(v, 1 - exp(-1), accuracy: 1e-9)
        var lf = LevelFollower()
        for _ in 0..<100 { _ = lf.step(rms: 0.5, dt: 0.01) }      // about -6 dBFS
        XCTAssertGreaterThan(lf.value, 0.95)
        for _ in 0..<300 { _ = lf.step(rms: 0, dt: 0.01) }
        XCTAssertLessThan(lf.value, 0.01)
    }

    func testPitchOfASungA() {
        let sr = 48_000.0
        for hz in [110.0, 220.0, 440.0, 196.0] {
            var x = [Float](repeating: 0, count: 2048)
            for i in 0..<x.count {
                let w: Double = 2 * Double.pi * hz * Double(i) / sr
                let a: Double = 0.6 * sin(w)
                let b: Double = 0.25 * sin(2 * w)
                let c: Double = 0.1 * sin(3 * w)
                x[i] = Float(a + b + c)
            }
            let f = try! XCTUnwrap(PitchDetector.frequency(x, sampleRate: sr))
            XCTAssertEqual(f, hz, accuracy: hz * 0.005, "\(hz)")
        }
        XCTAssertEqual(PitchDetector.note(440), 69, accuracy: 1e-9)
        // Noise isn't a pitch.
        var g = SystemRandomNumberGenerator()
        let noise = (0..<2048).map { _ in Float.random(in: -1...1, using: &g) }
        XCTAssertNil(PitchDetector.frequency(noise, sampleRate: sr))
    }

    func testPadMotionLoops() {
        var m = PadMotion()
        m.start()
        m.add(t: 10, x: 0, y: 0, p: 0)
        m.add(t: 11, x: 1, y: 0.5, p: 1)
        m.add(t: 12, x: 0, y: 1, p: 0)
        m.finish()
        XCTAssertEqual(m.length, 2)
        XCTAssertEqual(m.at(0.5)!.x, 0.5, accuracy: 1e-9)
        XCTAssertEqual(m.at(2.5)!.x, 0.5, accuracy: 1e-9)        // second time round
        XCTAssertEqual(m.at(1.5)!.y, 0.75, accuracy: 1e-9)
    }

    func testRateGateAndLinkKinds() {
        var g = RateGate(period: LinkKind.bluetooth.controlPeriod)
        XCTAssertTrue(g.ready(now: 0))
        XCTAssertFalse(g.ready(now: 0.02))
        XCTAssertTrue(g.ready(now: 0.06))
        XCTAssertEqual(LinkKind(driverOwner: "com.apple.AppleMIDIBluetoothDriver"), .bluetooth)
        XCTAssertEqual(LinkKind(driverOwner: "com.apple.AppleMIDIUSBDriver"), .usb)
        XCTAssertEqual(LinkKind(driverOwner: "com.apple.AppleMIDIRTPDriver"), .network)
    }
}
