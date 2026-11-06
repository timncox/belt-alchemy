import XCTest
@testable import AlchemyLink

/// MIDI routing: the parser, routes' filters, channels and transpose, fan
/// out, letting go of held notes, loops, modules played as keyboards (CTRL
/// events 6 and 7) and Belt's sung note as a module's MIDI out.
final class RoutingTests: XCTestCase {
    private func parse(_ bytes: [UInt8]) -> [[UInt8]] {
        var p = MIDIRouteParser()
        var out: [[UInt8]] = []
        p.feed(bytes) { out.append($0.bytes) }
        return out
    }

    private func msg(_ b: [UInt8]) -> RawMIDIMessage { RawMIDIMessage(b) }

    func testParserRunningStatusRealTimeAndSysEx() {
        let stream: [UInt8] = [0x90, 60, 100, 62, 0xF8, 90,                // running status, clock inside a note
                               0xF0, 0x7E, 1, 0xF8, 2, 0xF7,             // SysEx with a clock inside
                               0xB0, 7, 100, 8, 50,                       // running status CCs
                               0xF2, 0x10, 0x20, 0xFA,                    // song position, start
                               0x33,                                      // no running status after system common
                               0xC5, 3, 4, 0xE1, 0, 64]
        XCTAssertEqual(parse(stream), [[0x90, 60, 100], [0xF8], [0x90, 62, 90], [0xF8],
                                       [0xF0, 0x7E, 1, 2, 0xF7], [0xB0, 7, 100], [0xB0, 8, 50],
                                       [0xF2, 0x10, 0x20], [0xFA], [0xC5, 3], [0xC5, 4], [0xE1, 0, 64]])
        // Across packets, as CoreMIDI may split them.
        var p = MIDIRouteParser()
        var out: [[UInt8]] = []
        for chunk in [[0x90, 60] as [UInt8], [100, 0xF0, 1], [2, 0xF7, 61], [0]] { p.feed(chunk) { out.append($0.bytes) } }
        XCTAssertEqual(out, [[0x90, 60, 100], [0xF0, 1, 2, 0xF7]])
        // A SysEx cut short by a status byte is dropped.
        XCTAssertEqual(parse([0xF0, 1, 2, 0x80, 60, 0]), [[0x80, 60, 0]])

        // Categories, and the same messages as the controller code's parser.
        XCTAssertEqual(msg([0xF0, 1, 0xF7]).category, nil)
        XCTAssertEqual(msg([0xFE]).category, nil)
        XCTAssertEqual(msg([0xF8]).category, .clock)
        XCTAssertEqual(msg([0xD3, 9]).category, .aftertouch)
        XCTAssertEqual(msg([0xA3, 9, 9]).category, .aftertouch)
        XCTAssertEqual(msg([0xE3, 0, 64]).category, .pitchBend)
        XCTAssertTrue(msg([0x90, 60, 0]).isNoteOff)
        var old = MIDIStreamParser(), olds: [MIDIMessage] = []
        old.feed(stream) { olds.append($0) }
        var news: [MIDIMessage] = []
        var np = MIDIRouteParser()
        np.feed(stream) { if let m = $0.message { news.append(m) } }
        XCTAssertEqual(news, olds)
    }

    func testSysExIsNeverRouted() {
        let r = MIDIRouter()
        try! r.save(MIDIRoute(from: .device("Left"), to: .device("Daisy")))
        // A HostLink frame, as a module sends it.
        let frame = SysExPipe.encodeStream([0xA5, 0x01, 0x02, 0x03]).first!
        var p = MIDIRouteParser()
        var outs: [RouteOutput] = []
        p.feed(frame) { outs += r.input(from: .device("Left"), $0) }
        XCTAssertEqual(outs, [])
    }

    func testFilterAndChannelRemap() throws {
        let r = MIDIRouter()
        var route = MIDIRoute(from: .device("Keys"), to: .device("Daisy"), filter: [.notes], inChannel: 1, outChannel: 4)
        try r.save(route)
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x91, 60, 100])), [.midi(device: "Daisy", bytes: [0x94, 60, 100])])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x90, 62, 100])), [])          // channel 1: not this route
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xB1, 7, 100])), [])           // CCs filtered out
        XCTAssertEqual(r.input(from: .device("Other"), msg([0x91, 60, 100])), [])         // another source
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x81, 60, 0])), [.midi(device: "Daisy", bytes: [0x84, 60, 0])])

        route.filter = [.cc, .clock, .program]
        route.inChannel = nil
        route.outChannel = nil
        try r.save(route)
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xB3, 7, 9])), [.midi(device: "Daisy", bytes: [0xB3, 7, 9])])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xC3, 5])), [.midi(device: "Daisy", bytes: [0xC3, 5])])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xF8])), [.midi(device: "Daisy", bytes: [0xF8])])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xE3, 0, 64])), [])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x93, 60, 100])), [])
        XCTAssertEqual(route.summary, "CC, Program change, Clock and transport")

        // A channel picked in: clock still passes (it has no channel).
        route.inChannel = 0
        route.outChannel = 9
        try r.save(route)
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xFC])), [.midi(device: "Daisy", bytes: [0xFC])])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xB0, 1, 2])), [.midi(device: "Daisy", bytes: [0xB9, 1, 2])])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xB1, 1, 2])), [])

        // Switched off: nothing.
        _ = r.setEnabled(route.id, false)
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xB0, 1, 2])), [])
        XCTAssertEqual(RouteFilter.all.summary, "Everything")
    }

    func testTransposeBounds() throws {
        let r = MIDIRouter()
        var route = MIDIRoute(from: .device("Keys"), to: .device("Daisy"), transpose: 12)
        try r.save(route)
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x90, 60, 90])), [.midi(device: "Daisy", bytes: [0x90, 72, 90])])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x90, 120, 90])), [])           // 132: past the top
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x80, 120, 0])), [])            // its off: nothing held
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xA0, 60, 30])), [.midi(device: "Daisy", bytes: [0xA0, 72, 30])])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x80, 60, 0])), [.midi(device: "Daisy", bytes: [0x80, 72, 0])])
        route.transpose = -48
        try r.save(route)
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x90, 30, 90])), [])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x90, 48, 90])), [.midi(device: "Daisy", bytes: [0x90, 0, 90])])
        XCTAssertEqual(MIDIRoute(from: .device("a"), to: .device("b"), transpose: 100).transpose, 48)
        XCTAssertEqual(MIDIRoute(from: .device("a"), to: .device("b"), transpose: -100).transpose, -48)
    }

    func testFanOutToDevicesAndModules() throws {
        let r = MIDIRouter()
        try r.save(MIDIRoute(from: .device("Keys"), to: .device("Daisy")))
        try r.save(MIDIRoute(from: .device("Keys"), to: .module(7), transpose: -12))
        try r.save(MIDIRoute(from: .device("Keys"), to: .device("Synth"), filter: [.cc]))
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x92, 60, 80])),
                       [.midi(device: "Daisy", bytes: [0x92, 60, 80]), .keyboard(unit: 7, .keyNote(note: 48, velocity: 80))])
        // CC 7 to the module: dropped (only learned mappings act on a module).
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xB0, 7, 1])),
                       [.midi(device: "Daisy", bytes: [0xB0, 7, 1]), .midi(device: "Synth", bytes: [0xB0, 7, 1])])
        // The sustain pedal: the module's own pedal.
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0xB0, 64, 127])),
                       [.midi(device: "Daisy", bytes: [0xB0, 64, 127]), .keyboard(unit: 7, .keySustain(down: true)),
                        .midi(device: "Synth", bytes: [0xB0, 64, 127])])
        XCTAssertTrue(r.feedsKeyboard(unit: 7))
        XCTAssertFalse(r.feedsKeyboard(unit: 8))
        XCTAssertTrue(r.uses(.device("Synth")))
    }

    func testNoteOffsWhenARouteGoesOrADeviceIsUnplugged() throws {
        let r = MIDIRouter()
        let a = MIDIRoute(from: .device("Keys"), to: .device("Daisy"))
        let b = MIDIRoute(from: .device("Keys"), to: .module(7))
        try r.save(a)
        try r.save(b)
        _ = r.input(from: .device("Keys"), msg([0x90, 60, 100]))
        _ = r.input(from: .device("Keys"), msg([0x91, 64, 100]))
        _ = r.input(from: .device("Keys"), msg([0xB0, 64, 127]))
        XCTAssertEqual(r.remove(a.id), [.midi(device: "Daisy", bytes: [0x80, 60, 0]), .midi(device: "Daisy", bytes: [0x81, 64, 0]),
                                        .midi(device: "Daisy", bytes: [0xB0, 64, 0])])
        // The keyboard unplugged: the module lets go too.
        XCTAssertEqual(r.gone(.device("Keys")), [.keyboard(unit: 7, .keyNote(note: 60, velocity: 0)),
                                                 .keyboard(unit: 7, .keyNote(note: 64, velocity: 0)),
                                                 .keyboard(unit: 7, .keySustain(down: false))])
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x80, 60, 0])), [])

        // Switching a route off lets go as well; a changed route too.
        _ = r.input(from: .device("Keys"), msg([0x90, 50, 100]))
        XCTAssertEqual(r.setEnabled(b.id, false), [.keyboard(unit: 7, .keyNote(note: 50, velocity: 0))])
        _ = r.setEnabled(b.id, true)
        _ = r.input(from: .device("Keys"), msg([0x90, 50, 100]))
        var b2 = b
        b2.transpose = 5
        XCTAssertEqual(try r.save(b2), [.keyboard(unit: 7, .keyNote(note: 50, velocity: 0))])

        // The destination unplugged: nothing to send, and the later off is dropped.
        let c = MIDIRoute(from: .device("Pads"), to: .device("Daisy"))
        try r.save(c)
        _ = r.input(from: .device("Pads"), msg([0x90, 40, 100]))
        XCTAssertEqual(r.gone(.device("Daisy")), [])
        XCTAssertEqual(r.input(from: .device("Pads"), msg([0x80, 40, 0])), [])

        // Two sources on one note: the off waits for the last.
        _ = r.remove(b2.id)
        try r.save(MIDIRoute(from: .device("Keys"), to: .device("Daisy")))
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x90, 70, 100])).count, 1)
        XCTAssertEqual(r.input(from: .device("Pads"), msg([0x90, 70, 100])).count, 1)
        XCTAssertEqual(r.input(from: .device("Keys"), msg([0x80, 70, 0])), [])
        XCTAssertEqual(r.input(from: .device("Pads"), msg([0x80, 70, 0])), [.midi(device: "Daisy", bytes: [0x80, 70, 0])])
    }

    func testLoopsAreRefused() throws {
        let r = MIDIRouter()
        try r.save(MIDIRoute(from: .device("A"), to: .device("B")))
        try r.save(MIDIRoute(from: .device("B"), to: .device("C")))
        XCTAssertThrowsError(try r.save(MIDIRoute(from: .device("C"), to: .device("A")))) { XCTAssertEqual($0 as? RouteRefusal, .loop) }
        XCTAssertThrowsError(try r.save(MIDIRoute(from: .device("A"), to: .device("A")))) { XCTAssertEqual($0 as? RouteRefusal, .loop) }
        // Disabled routes still count.
        var off = MIDIRoute(from: .module(1), to: .module(2))
        off.enabled = false
        try r.save(off)
        XCTAssertThrowsError(try r.save(MIDIRoute(from: .module(2), to: .module(1))))
        XCTAssertThrowsError(try r.save(MIDIRoute(from: .device("A"), to: .gamepad("Pad")))) { XCTAssertEqual($0 as? RouteRefusal, .invalid) }
        try r.save(MIDIRoute(from: .device("C"), to: .device("D")))
        XCTAssertEqual(r.routes.count, 4)
        // Loading drops a loop that crept into the saved list.
        r.load(r.routes + [MIDIRoute(from: .device("D"), to: .device("A"))])
        XCTAssertEqual(r.routes.count, 4)
    }

    func testConsumedControlsOnlyWhenIncluded() throws {
        let r = MIDIRouter()
        var route = MIDIRoute(from: .device("Launchpad"), to: .device("Daisy"))
        try r.save(route)
        XCTAssertEqual(r.input(from: .device("Launchpad"), msg([0x90, 11, 127]), consumed: true), [])
        XCTAssertEqual(r.input(from: .device("Launchpad"), msg([0x90, 12, 127])).count, 1)
        // A held note's off goes out even when the control is now mapped.
        XCTAssertEqual(r.input(from: .device("Launchpad"), msg([0x80, 12, 0]), consumed: true).count, 1)
        route.includeMapped = true
        try r.save(route)
        XCTAssertEqual(r.input(from: .device("Launchpad"), msg([0x90, 11, 127]), consumed: true),
                       [.midi(device: "Daisy", bytes: [0x90, 11, 127])])
    }

    func testGamepadButtonsAsNotes() throws {
        let r = MIDIRouter()
        var route = MIDIRoute(from: .gamepad("Haute42"), to: .device("Daisy"), outChannel: 2)
        route.baseNote = 36
        try r.save(route)
        XCTAssertEqual(r.gamepad("Haute42", button: .a, down: true), [.midi(device: "Daisy", bytes: [0x92, 47, 100])])
        XCTAssertEqual(r.gamepad("Haute42", button: .a, down: false), [.midi(device: "Daisy", bytes: [0x82, 47, 0])])
        XCTAssertEqual(r.gamepad("Haute42", button: .up, down: true, consumed: true), [])     // forwarded to a module
        XCTAssertEqual(r.gamepad("Other", button: .up, down: true), [])
        route.baseNote = 120
        try r.save(route)
        XCTAssertEqual(r.gamepad("Haute42", button: .rt, down: true), [])                   // 136: past the top
        XCTAssertEqual(route.effectiveFilter, .notes)
    }

    func testCodableAndOlderSaves() throws {
        var route = MIDIRoute(from: .module(9), to: .device("Daisy"), filter: [.notes, .cc], outChannel: 3)
        route.harmonies = true
        route.knobCCs = [KnobCC(field: "p0.2", cc: 20)]
        let back = try JSONDecoder().decode([MIDIRoute].self, from: JSONEncoder().encode([route]))
        XCTAssertEqual(back, [route])
        // A save with only the essentials gets the defaults.
        let json = #"{"from":{"device":{"_0":"Keys"}},"to":{"module":{"_0":5}}}"#
        let min = try JSONDecoder().decode(MIDIRoute.self, from: Data(json.utf8))
        XCTAssertEqual(min.filter, .all)
        XCTAssertTrue(min.enabled)
        XCTAssertNil(min.outChannel)
        XCTAssertEqual(min.to, .module(5))
        XCTAssertEqual(route.summary, "Notes, CC · on ch 4 · with harmonies · 1 knob as a CC")
    }

    // MARK: modules

    func testKeyboardCtrlEvents() {
        XCTAssertEqual(CtrlEvent.keyNote(note: 60, velocity: 100).bytes, [6, 60, 100, 0])
        XCTAssertEqual(CtrlEvent.keySustain(down: true).bytes, [7, 1, 0, 0])
        for e in [CtrlEvent.keyNote(note: 60, velocity: 0), .keySustain(down: false), .keyNote(note: 127, velocity: 127)] {
            XCTAssertEqual(CtrlEvent(bytes: e.bytes), e)
            XCTAssertEqual(e.source, .keyboard)
            XCTAssertNil(e.element)
        }
        XCTAssertEqual(CtrlPresent.keyboard.rawValue, 8)
        // A failed request sends the note offs and pedal releases again, not the note ons.
        let s = CtrlSession()
        s.enqueue(.keyNote(note: 60, velocity: 90))
        s.enqueue(.keyNote(note: 62, velocity: 0))
        s.enqueue(.keySustain(down: false))
        let batch = s.begin(now: 0, maxEvents: 64)
        s.end(failed: batch)
        XCTAssertEqual(s.queue, [.keyNote(note: 62, velocity: 0), .keySustain(down: false)])
    }

    /// A keyboard routed to the pretend module: notes and the pedal arrive
    /// as CTRL events 6 and 7 with present bit 3, and the module plays them.
    func testModuleAsKeyboardThroughCtrl() async throws {
        let fake = FakeModule(viaSysEx: true)
        let link = HostLinkClient(transport: fake)
        _ = try await link.sayHello()
        let router = MIDIRouter()
        try router.save(MIDIRoute(from: .device("Keys"), to: .module(fake.unit)))
        let session = CtrlSession()
        var now = 0.0

        func play(_ bytes: [UInt8]) {
            var p = MIDIRouteParser()
            p.feed(bytes) { m in
                for o in router.input(from: .device("Keys"), m) {
                    if case let .keyboard(u, e) = o, u == fake.unit { session.enqueue(e) }
                }
            }
        }
        func exchange() async throws -> CtrlReply {
            now += 1
            let asked: CtrlPresent = router.feedsKeyboard(unit: fake.unit) ? .keyboard : []
            let batch = session.begin(now: now, maxEvents: CtrlRequest.maxEvents(maxBody: link.maxBody))
            let got = try await link.ctrl(present: asked, events: batch)
            let reply = try XCTUnwrap(got)
            session.end()
            router.ctrlReply(unit: fake.unit, asked: asked, seen: reply.presentSeen)
            return reply
        }

        play([0x90, 60, 100, 64, 90, 0xB0, 64, 127, 7, 50])
        var reply = try await exchange()
        XCTAssertTrue(reply.presentSeen.contains(.keyboard))
        XCTAssertEqual(fake.ctrlEvents, [.keyNote(note: 60, velocity: 100), .keyNote(note: 64, velocity: 90), .keySustain(down: true)])
        XCTAssertEqual(fake.sounding, [60, 64])
        play([0x80, 60, 0])
        _ = try await exchange()
        XCTAssertEqual(fake.keysDown, [64])
        XCTAssertEqual(fake.sounding, [60, 64])            // sustained
        play([0xB0, 64, 0])
        _ = try await exchange()
        XCTAssertEqual(fake.sounding, [64])
        // The route goes: the held note is let go.
        for o in router.remove(router.routes[0].id) { if case let .keyboard(_, e) = o { session.enqueue(e) } }
        reply = try await exchange()
        XCTAssertEqual(fake.sounding, [])
        XCTAssertFalse(reply.presentSeen.contains(.keyboard))
    }

    /// Firmware that can't play notes leaves bit 3 out: the app stops
    /// sending it notes (devices still get theirs).
    func testModuleThatCantPlayNotes() async throws {
        let fake = FakeModule(viaSysEx: true)
        fake.playsNotes = false
        let link = HostLinkClient(transport: fake)
        _ = try await link.sayHello()
        let router = MIDIRouter()
        try router.save(MIDIRoute(from: .device("Keys"), to: .module(fake.unit)))
        try router.save(MIDIRoute(from: .device("Keys"), to: .device("Daisy")))
        let outs = router.input(from: .device("Keys"), msg([0x90, 60, 100]))
        XCTAssertEqual(outs.count, 2)
        let events = outs.compactMap { o -> CtrlEvent? in if case let .keyboard(_, e) = o { return e }; return nil }
        let got = try await link.ctrl(present: .keyboard, events: events)
        let reply = try XCTUnwrap(got)
        XCTAssertFalse(reply.presentSeen.contains(.keyboard))
        XCTAssertEqual(fake.sounding, [])
        router.ctrlReply(unit: fake.unit, asked: .keyboard, seen: reply.presentSeen)
        XCTAssertEqual(router.noNotes, [fake.unit])
        XCTAssertFalse(router.feedsKeyboard(unit: fake.unit))
        XCTAssertEqual(router.input(from: .device("Keys"), msg([0x90, 62, 100])), [.midi(device: "Daisy", bytes: [0x90, 62, 100])])
        XCTAssertEqual(router.input(from: .device("Keys"), msg([0x80, 60, 0])), [.midi(device: "Daisy", bytes: [0x80, 60, 0])])
        XCTAssertEqual(router.input(from: .device("Keys"), msg([0xB0, 64, 127])), [.midi(device: "Daisy", bytes: [0xB0, 64, 127])])
        // Unplugged and back (perhaps with other firmware): asked again.
        _ = router.gone(.module(fake.unit))
        XCTAssertTrue(router.feedsKeyboard(unit: fake.unit))
    }

    /// STATE bytes as Belt sends them (firmware/phone_ext.h).
    private func beltState(sung: Double?, cents: Int = 0, voices: [UInt8] = [0, 0, 0, 0],
                           key: UInt8 = 0, scale: UInt8 = 1, inByte: UInt8 = 204) throws -> ModuleState {
        let s = sung.map { Int16(($0 * 100).rounded()) } ?? BeltState.unvoiced
        var w = Writer().u16(1).u8(0).u8(0).u8(0xFF).u8(0xFF).u16(0xFFFF).u8(100).u8(inByte).u8(inByte)
        w = w.u8(UInt8(ascii: "B")).u8(1).u8(key).u8(scale).raw(voices).u8(sung == nil ? 0 : 1)
        w = w.u16(UInt16(bitPattern: s)).u16(UInt16(bitPattern: Int16(cents))).u16(1).u8(0)
        return try ModuleState(reading: Reader(w.bytes))
    }

    func testBeltSungNoteFromState() throws {
        var f = BeltNoteFollower()
        let vel = BeltNoteFollower.velocity(inLevel: try beltState(sung: 60).inLevel)
        XCTAssertEqual(f.update(try beltState(sung: nil)), [])
        XCTAssertEqual(f.update(try beltState(sung: 60.12, cents: 12)), [ModuleNote(.sung, 60, vel)])
        XCTAssertEqual(f.update(try beltState(sung: 59.8, cents: -20)), [])                  // still pulled to C4
        XCTAssertEqual(f.update(try beltState(sung: 62.1, cents: 10)), [ModuleNote(.sung, 60, 0), ModuleNote(.sung, 62, vel)])
        XCTAssertEqual(f.update(try beltState(sung: nil)), [ModuleNote(.sung, 62, 0)])
        XCTAssertEqual(BeltNoteFollower.velocity(inLevel: -60), 1)
        XCTAssertEqual(BeltNoteFollower.velocity(inLevel: 0), 127)
        XCTAssertLessThan(BeltNoteFollower.velocity(inLevel: -40), BeltNoteFollower.velocity(inLevel: -10))

        // Harmony voices: a fifth above in C major (voice interval 9 = +5th),
        // and a unison voice that isn't sent twice.
        let st = try beltState(sung: 60, voices: [9, 6, 0, 0])
        XCTAssertEqual(st.belt?.harmonyNotes.compactMap { $0 }, [67, 60])
        let notes = f.update(st)
        XCTAssertEqual(notes, [ModuleNote(.sung, 60, vel), ModuleNote(.harmony, 67, vel)])

        // Through the router: one route takes the harmonies, one doesn't.
        let r = MIDIRouter()
        var withH = MIDIRoute(from: .module(3), to: .device("Daisy"))
        withH.harmonies = true
        try r.save(withH)
        try r.save(MIDIRoute(from: .module(3), to: .module(4)))
        XCTAssertEqual(r.moduleNotes(unit: 3, notes), [
            .midi(device: "Daisy", bytes: [0x90, 60, vel]), .keyboard(unit: 4, .keyNote(note: 60, velocity: vel)),
            .midi(device: "Daisy", bytes: [0x90, 67, vel]),
        ])
        // The module went away: everything it played is let go.
        XCTAssertEqual(f.release(), [ModuleNote(.harmony, 67, 0), ModuleNote(.sung, 60, 0)])
        XCTAssertEqual(r.gone(.module(3)).count, 3)
    }

    func testModuleKnobsAsCCs() throws {
        let r = MIDIRouter(knobPeriod: 0.1)
        var route = MIDIRoute(from: .module(3), to: .device("Daisy"), outChannel: 2)
        route.knobCCs = [KnobCC(field: "p0.2", cc: 20), KnobCC(field: "p0.3", cc: 21)]
        try r.save(route)
        var between = MIDIRoute(from: .module(3), to: .module(4))
        between.knobCCs = [KnobCC(field: "p0.2", cc: 20)]
        try r.save(between)
        XCTAssertEqual(between.effectiveFilter, .notes)          // knob to knob stays with linking

        r.moduleKnobs(unit: 3, ["p0.2": 0.5, "p0.9": 1])
        XCTAssertEqual(r.flush(now: 0), [.midi(device: "Daisy", bytes: [0xB2, 20, 64])])
        r.moduleKnobs(unit: 3, ["p0.2": 0.6])
        XCTAssertEqual(r.flush(now: 0.05), [])                    // not yet: at most once a period
        r.moduleKnobs(unit: 3, ["p0.2": 0.5])                     // back where it was: nothing to send
        XCTAssertFalse(r.hasPending)
        r.moduleKnobs(unit: 3, ["p0.3": 1])
        XCTAssertEqual(r.flush(now: 0.2), [.midi(device: "Daisy", bytes: [0xB2, 21, 127])])
        // The Daisy replugged: the next value goes out even if unchanged.
        r.arrived(.device("Daisy"))
        r.moduleKnobs(unit: 3, ["p0.3": 1])
        XCTAssertEqual(r.flush(now: 0.4), [.midi(device: "Daisy", bytes: [0xB2, 21, 127])])
    }
}
