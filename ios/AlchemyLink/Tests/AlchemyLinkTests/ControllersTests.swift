import XCTest
@testable import AlchemyLink

/// The hub's controllers: CTRL's bytes, the Novation devices' MIDI both
/// ways, the gamepad's mask, the app's own mappings, and a whole round
/// trip through the pretend module.
final class ControllersTests: XCTestCase {
    func testCtrlRequestBytes() {
        let events: [CtrlEvent] = [
            .launchpad(.grid, x: 3, y: 5, down: true), .launchpad(.side, x: 0, y: 7, down: false),
            .xlKnob(row: 2, col: 6, value: 100), .xlFader(col: 7, value: 127),
            .xlButton(row: 1, col: 4, down: true), .gamepad(0x3_1001),
        ]
        let body = CtrlRequest.body(present: [.launchpad, .gamepad], events: events)
        XCTAssertEqual(body, [5, 6,
                              1, 0, 3, 0x85,  1, 1, 0, 7,
                              2, 2, 6, 100,  3, 7, 127, 0,
                              4, 1, 4, 1,  5, 0x01, 0x10, 0x03])
        for e in events { XCTAssertEqual(CtrlEvent(bytes: e.bytes), e) }
        XCTAssertEqual(CtrlRequest.body(present: [], events: []), [0, 0])
        XCTAssertEqual(CtrlRequest.maxEvents(maxBody: 512), 64)
        XCTAssertEqual(CtrlRequest.maxEvents(maxBody: 130), 32)
        XCTAssertEqual(CtrlPresent.launchControlXL.rawValue, 2)

        let lp = (0..<81).map { UInt8($0) }, xl = (0..<40).map { UInt8(100 + $0) }
        let reply = try! CtrlReply(reading: Reader([1, 3] + lp + xl + [9, 9]))   // extra bytes ignored
        XCTAssertEqual(reply.version, 1)
        XCTAssertEqual(reply.presentSeen, [.launchpad, .launchControlXL])
        XCTAssertEqual(reply.lpLeds, lp)
        XCTAssertEqual(reply.xlLeds, xl)
        XCTAssertThrowsError(try CtrlReply(reading: Reader([1, 0] + lp)))
    }

    func testLaunchpadInAndOut() {
        func ev(_ st: UInt8, _ d1: UInt8, _ d2: UInt8) -> CtrlEvent? {
            LaunchpadMK3.event(.channel(status: st, d1: d1, d2: d2))
        }
        XCTAssertEqual(ev(0x90, 11, 127), .launchpad(.grid, x: 0, y: 7, down: true))     // bottom left
        XCTAssertEqual(ev(0x90, 88, 127), .launchpad(.grid, x: 7, y: 0, down: true))     // top right
        XCTAssertEqual(ev(0x90, 45, 0), .launchpad(.grid, x: 4, y: 4, down: false))      // velocity 0
        XCTAssertEqual(ev(0x80, 45, 64), .launchpad(.grid, x: 4, y: 4, down: false))
        XCTAssertEqual(ev(0xB0, 89, 127), .launchpad(.side, x: 0, y: 0, down: true))
        XCTAssertEqual(ev(0xB0, 19, 0), .launchpad(.side, x: 0, y: 7, down: false))
        XCTAssertEqual(ev(0xB0, 91, 127), .launchpad(.top, x: 0, y: 0, down: true))
        XCTAssertEqual(ev(0xB0, 98, 127), .launchpad(.top, x: 7, y: 0, down: true))
        XCTAssertNil(ev(0xB0, 99, 127))                                                   // the logo
        XCTAssertNil(ev(0xB0, 5, 127))
        XCTAssertNil(ev(0xE0, 11, 127))
        XCTAssertEqual(LaunchpadMK3.programmerMode, [0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x0E, 0x01, 0xF7])

        XCTAssertEqual(LaunchpadMK3.message(index: 0, colour: 21), [0x90, 81, 21])      // grid top left
        XCTAssertEqual(LaunchpadMK3.message(index: 63, colour: 5), [0x90, 18, 5])
        XCTAssertEqual(LaunchpadMK3.message(index: 64, colour: 5), [0xB0, 89, 5])        // side, top
        XCTAssertEqual(LaunchpadMK3.message(index: 71, colour: 5), [0xB0, 19, 5])
        XCTAssertEqual(LaunchpadMK3.message(index: 72, colour: 5), [0xB0, 91, 5])        // top row
        XCTAssertEqual(LaunchpadMK3.message(index: 79, colour: 5), [0xB0, 98, 5])
        XCTAssertEqual(LaunchpadMK3.message(index: 80, colour: 200), [0xB0, 99, 72])     // logo, 7 bits
        // Every pad's LED goes back to the note or CC the pad sends.
        for k in LPKind.allCases {
            for n in 0..<8 {
                let (x, y) = k == .side ? (0, n) : k == .top ? (n, 0) : (n % 8, (n * 3) % 8)
                let m = LaunchpadMK3.message(index: LaunchpadMK3.index(k, x: x, y: y), colour: 1)
                XCTAssertEqual(ev(m[0], m[1], 127), .launchpad(k, x: UInt8(x), y: UInt8(y), down: true))
            }
        }
    }

    func testLEDsOnlyWhatChanged() {
        var d = LEDDiffer(count: 81)
        var frame = [UInt8](repeating: 0, count: 81)
        XCTAssertEqual(LaunchpadMK3.messages(frame, diff: &d).count, 81)   // nothing known yet
        XCTAssertEqual(LaunchpadMK3.messages(frame, diff: &d).count, 0)
        frame[9] = 21; frame[80] = 3
        XCTAssertEqual(LaunchpadMK3.messages(frame, diff: &d), [[0x90, 72, 21], [0xB0, 99, 3]])
        d.forget()
        XCTAssertEqual(LaunchpadMK3.messages(frame, diff: &d).count, 81)

        var x = LEDDiffer(count: 40)
        var xf = [UInt8](repeating: LaunchControlXL.colour(red: 0, green: 0), count: 40)
        XCTAssertEqual(LaunchControlXL.messages(xf, diff: &x).count, 40)
        xf[25] = LaunchControlXL.colour(red: 3, green: 3)
        XCTAssertEqual(LaunchControlXL.messages(xf, diff: &x),
                       [[0xF0, 0x00, 0x20, 0x29, 0x02, 0x11, 0x78, 0x08, 25, 0x3F, 0xF7]])
    }

    func testLaunchControlXL() {
        func ev(_ st: UInt8, _ d1: UInt8, _ d2: UInt8) -> CtrlEvent? {
            LaunchControlXL.event(.channel(status: st, d1: d1, d2: d2))
        }
        XCTAssertEqual(ev(0xB0, 13, 5), .xlKnob(row: 0, col: 0, value: 5))
        XCTAssertEqual(ev(0xB3, 36, 6), .xlKnob(row: 1, col: 7, value: 6))   // channel ignored
        XCTAssertEqual(ev(0xB0, 49, 7), .xlKnob(row: 2, col: 0, value: 7))
        XCTAssertEqual(ev(0xB0, 56, 8), .xlKnob(row: 2, col: 7, value: 8))
        XCTAssertEqual(ev(0xB0, 77, 9), .xlFader(col: 0, value: 9))
        XCTAssertEqual(ev(0xB0, 84, 9), .xlFader(col: 7, value: 9))
        XCTAssertNil(ev(0xB0, 21, 9))
        XCTAssertEqual(ev(0x90, 41, 127), .xlButton(row: 0, col: 0, down: true))
        XCTAssertEqual(ev(0x90, 57, 127), .xlButton(row: 0, col: 4, down: true))
        XCTAssertEqual(ev(0x90, 60, 127), .xlButton(row: 0, col: 7, down: true))
        XCTAssertEqual(ev(0x80, 73, 0), .xlButton(row: 1, col: 0, down: false))
        XCTAssertEqual(ev(0x90, 92, 0), .xlButton(row: 1, col: 7, down: false))
        XCTAssertNil(ev(0x90, 45, 127))
        XCTAssertEqual(LaunchControlXL.selectTemplate, [0xF0, 0x00, 0x20, 0x29, 0x02, 0x11, 0x77, 0x08, 0xF7])
        XCTAssertTrue(LaunchControlXL.templateChanged(.sysex([0xF0, 0x00, 0x20, 0x29, 0x02, 0x11, 0x77, 0x00, 0xF7])))
        XCTAssertFalse(LaunchControlXL.templateChanged(.sysex(LaunchControlXL.selectTemplate)))
        XCTAssertEqual(LaunchControlXL.colour(red: 0, green: 0), 0x0C)
        XCTAssertEqual(LaunchControlXL.colour(red: 3, green: 0), 0x0F)
        XCTAssertEqual(LaunchControlXL.colour(red: 0, green: 3), 0x3C)
    }

    func testMIDIStream() {
        var p = MIDIStreamParser()
        var got: [MIDIMessage] = []
        // Running status, a clock byte in the middle, a program change.
        p.feed([0x90, 60, 100, 62, 0xF8, 0, 0xC2, 5, 0xB1, 7]) { got.append($0) }
        p.feed([90]) { got.append($0) }
        // SysEx split across packets.
        p.feed([0xF0, 0x00, 0x20]) { got.append($0) }
        p.feed([0x29, 0xF7, 0x80, 60, 0]) { got.append($0) }
        XCTAssertEqual(got, [
            .channel(status: 0x90, d1: 60, d2: 100), .channel(status: 0x90, d1: 62, d2: 0),
            .channel(status: 0xC2, d1: 5, d2: 0), .channel(status: 0xB1, d1: 7, d2: 90),
            .sysex([0xF0, 0x00, 0x20, 0x29, 0xF7]), .channel(status: 0x80, d1: 60, d2: 0),
        ])
        var latch = PortLatch()
        XCTAssertTrue(latch.accept(7))
        XCTAssertFalse(latch.accept(8))
        XCTAssertTrue(latch.accept(7))
    }

    func testXInputMask() {
        let want: [(XInputButton, UInt32)] = [
            (.up, 0x0001), (.down, 0x0002), (.left, 0x0004), (.right, 0x0008), (.start, 0x0010), (.back, 0x0020),
            (.l3, 0x0040), (.r3, 0x0080), (.lb, 0x0100), (.rb, 0x0200), (.guide, 0x0400),
            (.a, 0x1000), (.b, 0x2000), (.x, 0x4000), (.y, 0x8000), (.lt, 1 << 16), (.rt, 1 << 17),
        ]
        for (b, bit) in want { XCTAssertEqual(b.bit, bit, b.name) }
        XCTAssertEqual(XInputButton.allCases.count, want.count)
        XCTAssertEqual(XInput.mask([.a, .lt, .up]), 0x1_1001)
        XCTAssertEqual(XInput.buttons(0x1_1001), [.a, .lt, .up])
        XCTAssertEqual(XInput.mask([]), 0)
    }

    func testWhichController() {
        XCTAssertEqual(ControllerKind.classify(midiName: "LPMiniMK3 MIDI"), .launchpad)
        XCTAssertEqual(ControllerKind.classify(midiName: "Launchpad Mini MK3 LPMiniMK3 DAW"), .launchpad)
        XCTAssertEqual(ControllerKind.classify(midiName: "Launch Control XL"), .launchControlXL)
        XCTAssertEqual(ControllerKind.classify(midiName: "Launch Control XL 3"), .midi)
        XCTAssertEqual(ControllerKind.classify(midiName: "Launchpad X"), .midi)
        XCTAssertEqual(ControllerKind.classify(midiName: "KeyStep 37"), .midi)
        XCTAssertNil(ControllerKind.classify(midiName: "Alchemy Lab"))

        let ready: [UInt32] = [10, 20]
        XCTAssertEqual(ControllerRoute.left.units(ready), [10])
        XCTAssertEqual(ControllerRoute.right.units(ready), [20])
        XCTAssertEqual(ControllerRoute.both.units(ready), [10, 20])
        XCTAssertEqual(ControllerRoute.right.units([10]), [])
        XCTAssertEqual(ControllerRoute.appOnly.units(ready), [])
        let cs: [(kind: ControllerKind, route: ControllerRoute)] = [
            (.launchpad, .both), (.launchControlXL, .right), (.gamepad, .appOnly), (.midi, .both),
        ]
        XCTAssertEqual(ctrlPresent(for: 10, controllers: cs, ready: ready), [.launchpad])
        XCTAssertEqual(ctrlPresent(for: 20, controllers: cs, ready: ready), [.launchpad, .launchControlXL])
        XCTAssertEqual(ControllerRoute.choices(for: .midi), [.appOnly, .off])

        XCTAssertEqual(PresetStepper.step(from: nil, valid: [2, 5, 9], by: 1), 2)
        XCTAssertEqual(PresetStepper.step(from: nil, valid: [2, 5, 9], by: -1), 9)
        XCTAssertEqual(PresetStepper.step(from: 5, valid: [2, 5, 9], by: 1), 9)
        XCTAssertEqual(PresetStepper.step(from: 9, valid: [2, 5, 9], by: 1), 2)
        XCTAssertEqual(PresetStepper.step(from: 2, valid: [2, 5, 9], by: -1), 9)
        XCTAssertEqual(PresetStepper.step(from: 3, valid: [2, 5, 9], by: -1), 2)
        XCTAssertNil(PresetStepper.step(from: 3, valid: [], by: 1))
    }

    func testLearnAndMap() throws {
        let xl = "Launch Control XL", lp = "Launchpad Mini MK3"
        let knob = ControllerControl(device: xl, element: .xlKnob(row: 0, col: 2))
        let pad = ControllerControl(device: lp, element: .lpPad(.grid, x: 1, y: 1))
        let t = ControlTarget(unit: 7, field: "p0.2")
        var map = ControllerMap()

        XCTAssertNil(map.handle(knob, value: 0.5))                  // not mapped: forwarded
        map.beginLearn()
        XCTAssertTrue(map.consumes(pad))
        XCTAssertEqual(map.handle(pad, value: 0), [])               // a release doesn't count
        XCTAssertTrue(map.learning)
        XCTAssertEqual(map.handle(knob, value: 0.3), [])
        XCTAssertFalse(map.learning)
        XCTAssertEqual(map.learned, knob)
        XCTAssertNil(map.handle(pad, value: 1))                     // others go on
        XCTAssertEqual(map.handle(knob, value: 0.4), [])            // held back until assigned
        map.assign(ControllerMapping(control: knob, action: .knob(t), lo: 0.2, hi: 0.6))
        XCTAssertEqual(map.learned, knob)                         // stays, for more targets
        XCTAssertEqual(map.handle(knob, value: 0.5), [.knob(t, 0.4)])
        map.finishLearn()
        XCTAssertNil(map.learned)
        XCTAssertEqual(map.handle(knob, value: 1), [.knob(t, 0.6)])

        // A button on a knob: held, then latched.
        map.assign(ControllerMapping(control: pad, action: .padX, lo: 0.1, hi: 0.9))
        XCTAssertEqual(map.handle(pad, value: 1), [.padX(0.9)])
        XCTAssertEqual(map.handle(pad, value: 0), [.padX(0.1)])
        var m = map.mappings(for: pad)[0]
        m.mode = .toggle
        map.assign(m)
        XCTAssertEqual(map.mappings.count, 2)                      // changed (same id), not added
        XCTAssertEqual(map.handle(pad, value: 1), [.padX(0.9)])
        XCTAssertEqual(map.handle(pad, value: 0), [])
        XCTAssertEqual(map.handle(pad, value: 1), [.padX(0.1)])

        // HARD and mute: hold, or tap to latch.
        let a = ControllerControl(device: "Haute42", element: .gamepad(.a))
        map.assign(ControllerMapping(control: a, action: .hard(unit: nil)))
        XCTAssertEqual(map.handle(a, value: 1), [.press(unit: nil, button: 0, gesture: .down)])
        XCTAssertEqual(map.handle(a, value: 0), [.press(unit: nil, button: 0, gesture: .up)])
        map.remove(control: a)
        map.assign(ControllerMapping(control: a, action: .mute(unit: 7), mode: .toggle))
        XCTAssertEqual(map.handle(a, value: 1), [.press(unit: 7, button: 1, gesture: .tap)])
        XCTAssertEqual(map.handle(a, value: 0), [])

        // Presets, linking.
        let up = ControllerControl(device: xl, element: .xlButton(row: 0, col: 0))
        map.assign(ControllerMapping(control: up, action: .presetPrevious(unit: 7)))
        XCTAssertEqual(map.handle(up, value: 1), [.preset(unit: 7, step: -1)])
        XCTAssertEqual(map.handle(up, value: 0), [])
        let note = ControllerControl(device: "KeyStep", element: .midiNote(channel: 0, number: 60))
        map.assign(ControllerMapping(control: note, action: .splitHarmonies, mode: .toggle))
        XCTAssertEqual(map.handle(note, value: 1), [.splitHarmonies(nil)])
        map.remove(control: note)
        map.assign(ControllerMapping(control: note, action: .shareKey))
        XCTAssertEqual(map.handle(note, value: 1), [.shareKey(true)])
        XCTAssertEqual(map.handle(note, value: 0), [.shareKey(false)])

        // A fader on a switch: on past half way, with some slack.
        let fader = ControllerControl(device: xl, element: .xlFader(col: 3))
        map.assign(ControllerMapping(control: fader, action: .hard(unit: 7)))
        XCTAssertEqual(map.handle(fader, value: 0.3), [])
        XCTAssertEqual(map.handle(fader, value: 0.6), [.press(unit: 7, button: 0, gesture: .down)])
        XCTAssertEqual(map.handle(fader, value: 0.5), [])
        XCTAssertEqual(map.handle(fader, value: 0.4), [.press(unit: 7, button: 0, gesture: .up)])

        // Saved and loaded: the mappings, not the latches or learning.
        map.beginLearn()
        let data = try JSONEncoder().encode(map)
        let back = try JSONDecoder().decode(ControllerMap.self, from: data)
        XCTAssertEqual(back.mappings, map.mappings)
        XCTAssertFalse(back.learning)
        XCTAssertEqual(back.mappings(device: xl).count, 3)

        map.cancelLearn()
        map.remove(map.mappings(for: knob)[0].id)
        XCTAssertNil(map.handle(knob, value: 0.5))
        map.prune { $0.unit != 7 }
        XCTAssertEqual(map.mappings.map(\.action), [.padX, .shareKey])

        XCTAssertEqual(ControlElement.from(midi: .channel(status: 0xB2, d1: 74, d2: 127))?.0, .midiCC(channel: 2, number: 74))
        XCTAssertEqual(ControlElement.from(midi: .channel(status: 0x90, d1: 60, d2: 0))?.1, 0)
        XCTAssertEqual(ControlElement.midiNote(channel: 0, number: 60).label, "Note C4, channel 1")
    }

    /// One control, several targets: an XL fader on a knob on Left and a CC
    /// on the Daisy Patch, each over its own range; a pad as a gate.
    func testOneControlManyTargetsAndMIDIOut() throws {
        let fader = ControllerControl(device: "Launch Control XL", element: .xlFader(col: 0))
        let left = ControlTarget(unit: 1, field: "p0.3")
        let daisyCC = MIDIOutTarget(device: "Daisy Patch", channel: 2, number: 74)
        let daisyGate = MIDIOutTarget(device: "Daisy Patch", channel: 0, number: 36)
        var map = ControllerMap()
        map.beginLearn()
        _ = map.handle(fader, value: 0.1)
        map.assign(ControllerMapping(control: fader, action: .knob(left)))
        map.assign(ControllerMapping(control: fader, action: .midiCC(daisyCC), lo: 1, hi: 0.5))
        map.finishLearn()
        XCTAssertEqual(map.mappings(for: fader).count, 2)
        XCTAssertEqual(map.controls, [fader])
        XCTAssertEqual(map.handle(fader, value: 0.5), [.knob(left, 0.5), .midiCC(daisyCC, 0.75)])

        let pad = ControllerControl(device: "Launchpad Mini MK3", element: .lpPad(.grid, x: 0, y: 0))
        map.assign(ControllerMapping(control: pad, action: .midiNote(daisyGate)))
        map.assign(ControllerMapping(control: pad, action: .hard(unit: 1), mode: .toggle))
        XCTAssertEqual(map.handle(pad, value: 1), [.midiNote(daisyGate, on: true), .press(unit: 1, button: 0, gesture: .tap)])
        XCTAssertEqual(map.handle(pad, value: 0), [.midiNote(daisyGate, on: false)])
        var gate = map.mappings(for: pad)[0]
        gate.mode = .toggle
        map.assign(gate)
        XCTAssertEqual(map.handle(pad, value: 1).map { Array($0.prefix(1)) }, [.midiNote(daisyGate, on: true)])
        XCTAssertEqual(map.handle(pad, value: 0), [])
        XCTAssertEqual(map.handle(pad, value: 1).map { Array($0.prefix(1)) }, [.midiNote(daisyGate, on: false)])

        // The app's own sources drive MIDI out too, and are never learned.
        let tilt = ControllerControl(device: ControllerMap.appDevice, element: .app(.tilt))
        map.assign(ControllerMapping(control: tilt, action: .midiCC(daisyCC)))
        map.assign(ControllerMapping(control: tilt, action: .midiNote(daisyGate)))
        XCTAssertEqual(map.drive(.tilt, value: 0.2), [.midiCC(daisyCC, 0.2)])
        XCTAssertEqual(map.drive(.tilt, value: 0.8), [.midiCC(daisyCC, 0.8), .midiNote(daisyGate, on: true)])
        XCTAssertEqual(map.drive(.micLevel, value: 0.8), [])
        map.beginLearn()
        XCTAssertEqual(map.drive(.tilt, value: 0.3).count, 2)
        XCTAssertTrue(map.learning)
        map.cancelLearn()

        let back = try JSONDecoder().decode(ControllerMap.self, from: JSONEncoder().encode(map))
        XCTAssertEqual(back.mappings, map.mappings)
        map.remove(control: pad)
        XCTAssertNil(map.handle(pad, value: 1))
        XCTAssertFalse(map.consumes(pad))
        XCTAssertTrue(map.consumes(fader))
    }

    func testMIDIOutOnlyOnChangeAtTheControlRate() {
        let q = MIDIOutQueue(period: 1.0 / 60)
        let a = MIDIOutTarget(device: "Daisy Patch", channel: 2, number: 74)
        let b = MIDIOutTarget(device: "Daisy Patch", channel: 2, number: 75)
        let k = MIDIOutTarget(device: "KeyStep", channel: 0, number: 1)
        XCTAssertFalse(q.hasPending)
        q.cc(a, 0.1); q.cc(k, 1); q.cc(a, 0.5); q.cc(b, 0)
        var out = q.flush(now: 0)
        XCTAssertEqual(out.map(\.device), ["Daisy Patch", "KeyStep"])
        XCTAssertEqual(out[0].messages, [[0xB2, 74, 64], [0xB2, 75, 0]])      // newest value only
        XCTAssertEqual(out[1].messages, [[0xB0, 1, 127]])
        q.cc(a, 0.5)                                                          // no change: nothing
        XCTAssertFalse(q.hasPending)
        q.cc(a, 0.6)
        XCTAssertTrue(q.flush(now: 0.01).isEmpty)                             // not yet
        out = q.flush(now: 0.02)
        XCTAssertEqual(out.first?.messages, [[0xB2, 74, 76]])
        q.cc(a, 0.9); q.cc(a, 0.6)                                            // back where it was
        XCTAssertFalse(q.hasPending)
        q.forget(device: "Daisy Patch")
        q.cc(a, 0.6)
        XCTAssertEqual(q.flush(now: 1).first?.messages, [[0xB2, 74, 76]])

        let g = MIDIOutTarget(device: "Daisy Patch", channel: 0, number: 36)
        XCTAssertEqual(q.note(g, on: true), [0x90, 36, 127])
        XCTAssertNil(q.note(g, on: true))
        XCTAssertEqual(q.note(g, on: false), [0x80, 36, 0])
        XCTAssertNil(q.note(g, on: false))
    }

    func testSessionQueueAndTiming() {
        let s = CtrlSession()
        s.enqueue(.xlKnob(row: 0, col: 0, value: 1))
        s.enqueue(.launchpad(.grid, x: 0, y: 0, down: true))
        s.enqueue(.xlKnob(row: 0, col: 0, value: 9))           // replaces the waiting value
        s.enqueue(.xlFader(col: 2, value: 3))
        s.enqueue(.xlFader(col: 2, value: 4))
        s.enqueue(.launchpad(.grid, x: 0, y: 0, down: false))
        s.enqueue(.gamepad(1))
        s.enqueue(.gamepad(1))                                  // same mask: dropped
        s.enqueue(.gamepad(0))
        XCTAssertEqual(s.queue, [.xlKnob(row: 0, col: 0, value: 9), .launchpad(.grid, x: 0, y: 0, down: true),
                                 .xlFader(col: 2, value: 4), .launchpad(.grid, x: 0, y: 0, down: false),
                                 .gamepad(1), .gamepad(0)])

        XCTAssertTrue(s.due(now: 0, controlPeriod: 1.0 / 60, statePeriod: 1.0 / 15))
        let batch = s.begin(now: 0, maxEvents: 4)
        XCTAssertEqual(batch.count, 4)
        XCTAssertFalse(s.due(now: 5, controlPeriod: 1.0 / 60, statePeriod: 1.0 / 15))   // one in flight
        s.end()
        XCTAssertFalse(s.due(now: 0.01, controlPeriod: 1.0 / 60, statePeriod: 1.0 / 15))
        XCTAssertTrue(s.due(now: 0.02, controlPeriod: 1.0 / 60, statePeriod: 1.0 / 15))
        let b2 = s.begin(now: 0.02, maxEvents: 64)
        XCTAssertEqual(b2, [.gamepad(1), .gamepad(0)])
        s.end(failed: b2)                                       // lost: the newest mask goes again
        XCTAssertEqual(s.queue, [.gamepad(0)])
        _ = s.begin(now: 0.04, maxEvents: 64)
        s.end()
        // Nothing waiting: a poll at the state rate.
        XCTAssertFalse(s.due(now: 0.06, controlPeriod: 1.0 / 60, statePeriod: 1.0 / 15))
        XCTAssertTrue(s.due(now: 0.11, controlPeriod: 1.0 / 60, statePeriod: 1.0 / 15))
        XCTAssertEqual(s.begin(now: 0.11, maxEvents: 64), [])
    }

    func testOlderFirmwareHasNoCtrl() async throws {
        let fake = FakeModule(viaSysEx: true)
        fake.supportsCtrl = false
        let link = HostLinkClient(transport: fake)
        let info = try await link.phoneInfo()
        XCTAssertEqual(info?.hasCtrl, false)
        XCTAssertEqual(info?.hasState, true)
        let r = try await link.ctrl(present: [.launchpad], events: [])
        XCTAssertNil(r)
        let ok = HostLinkClient(transport: FakeModule(viaSysEx: true))
        let info2 = try await ok.phoneInfo()
        XCTAssertEqual(info2?.hasCtrl, true)
    }

    /// Launchpad and XL MIDI in, through CTRL to the pretend module, its
    /// LEDs back out as the MIDI that lights them; the gamepad's mask; and
    /// the controllers going away.
    func testForwardAndLEDRoundTrip() async throws {
        let fake = FakeModule(viaSysEx: true)
        let link = HostLinkClient(transport: fake)
        _ = try await link.sayHello()
        let session = CtrlSession()
        var lpDiff = LEDDiffer(count: LaunchpadMK3.cells), xlDiff = LEDDiffer(count: LaunchControlXL.leds)
        var parser = MIDIStreamParser()
        var now = 0.0
        let present: CtrlPresent = [.launchpad, .launchControlXL, .gamepad]

        func midiIn(_ bytes: [UInt8], xl: Bool = false) {
            parser.feed(bytes) { m in
                if let e = xl ? LaunchControlXL.event(m) : LaunchpadMK3.event(m) { session.enqueue(e) }
            }
        }
        func exchange(_ p: CtrlPresent = present) async throws -> (lp: [[UInt8]], xl: [[UInt8]]) {
            now += 1
            XCTAssertTrue(session.due(now: now, controlPeriod: 1.0 / 60, statePeriod: 1.0 / 15))
            let batch = session.begin(now: now, maxEvents: CtrlRequest.maxEvents(maxBody: link.maxBody))
            let reply = try await link.ctrl(present: p, events: batch)
            session.end()
            let r = try XCTUnwrap(reply)
            XCTAssertEqual(r.presentSeen, p)
            return (LaunchpadMK3.messages(r.lpLeds, diff: &lpDiff), LaunchControlXL.messages(r.xlLeds, diff: &xlDiff))
        }

        // First poll: every LED goes out once.
        var out = try await exchange()
        XCTAssertEqual(out.lp.count, 81)
        XCTAssertEqual(out.xl.count, 40)
        XCTAssertTrue(out.lp.contains([0xB0, 91, 7]))           // HARD off: dim red
        XCTAssertTrue(out.lp.contains([0xB0, 92, 15]))          // mute off: dim amber

        // Press the pad at row 6, column 3 (note 63, running status for the release).
        midiIn([0x90, 63, 127])
        out = try await exchange()
        XCTAssertEqual(out.lp, [[0x90, 63, 21]])
        XCTAssertEqual(fake.ctrlEvents.last, .launchpad(.grid, x: 2, y: 2, down: true))
        midiIn([63, 0])
        out = try await exchange()
        XCTAssertEqual(out.lp, [[0x90, 63, 0]])

        // The top row's first button latches HARD; its LED says so.
        midiIn([0xB0, 91, 127, 91, 0])
        out = try await exchange()
        XCTAssertEqual(out.lp, [[0xB0, 91, 5]])
        let st = try await link.state(since: nil)
        XCTAssertEqual(st.belt?.hard, true)

        // An XL knob: the module lights its LED.
        midiIn([0xB0, 30, 100], xl: true)
        out = try await exchange()
        XCTAssertEqual(out.xl, [LaunchControlXL.message(index: 9, colour: LaunchControlXL.colour(red: 0, green: 3))])
        XCTAssertEqual(out.lp, [])

        // The gamepad's mask, then the gamepad unplugged: the module lets go.
        session.enqueue(.gamepad(XInput.mask([.a, .rt])))
        _ = try await exchange()
        XCTAssertEqual(fake.gamepadMask, 0x2_1000)
        _ = try await exchange([.launchpad, .launchControlXL])
        XCTAssertEqual(fake.gamepadMask, 0)
        XCTAssertEqual(fake.ctrlPresent, [.launchpad, .launchControlXL])
    }
}
