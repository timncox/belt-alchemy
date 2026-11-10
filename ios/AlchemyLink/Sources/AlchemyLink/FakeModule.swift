// A pretend Alchemy Lab running Belt, so the iPhone app runs in the
// Simulator and the tests run on a Mac with no module plugged in. It answers
// the same commands Belt answers in Phone mode, from memory: Belt's Play and
// Setup pages and a settings page, 16 preset slots, an SD card with a few
// files, the LED screen drawn from the knob values, STATE with a pretend
// singer (a phrase that wanders around the key, with vibrato and breaths),
// the HARD and HARMONIES buttons, and a firmware switch that only pretends
// to flash. With `viaSysEx`, every byte goes through the SysEx pipe both
// ways, exactly as it does over the USB cable.

import Foundation

public final class FakeModule: ByteTransport, @unchecked Sendable {
    private let q = DispatchQueue(label: "fake-module")
    private var handler: (([UInt8]) -> Void)?
    private var splitter = FrameSplitter()
    private var hostDecoder = SysExPipe.Decoder()     // phone → module
    private var phoneDecoder = SysExPipe.Decoder()    // module → phone
    public let viaSysEx: Bool

    public private(set) var live: [UInt8]
    private var slots: [Int: [UInt8]] = [:]
    private var files: [String: [UInt8]] = [:]
    private var dirs: Set<String> = ["/", "/alchemy", "/samples"]
    private var upload: (path: String, total: Int, staged: [UInt8], overwrite: Bool)?
    private var fwState: FwState = .idle
    private var fwProgress = 0.0
    private var frame: UInt32 = 0
    public private(set) var presses: [(Int, UInt8)] = []
    let descJSON: [UInt8]
    let schemaHash: UInt32 = 0xB31_7A1C
    public let unit: UInt32
    let name: String
    let born = Date()
    let seed: Double
    private var hardLatch = false, muteLatch = false, hardDown = false, muteDown = false
    private var touch: (page: Int, pot: Int, at: Date)?
    /// Pretend audio load and levels, for the screen's meters.
    public var pretendCpu = 0.42
    /// Answers 0x77 CTRL (PHONE_INFO flags bit 4); false acts as an older firmware.
    public var supportsCtrl = true
    /// The controllers the phone forwards (CTRL), as the module holds them.
    public private(set) var ctrlPresent: CtrlPresent = []
    public private(set) var ctrlEvents: [CtrlEvent] = []
    public private(set) var gamepadMask: UInt32 = 0
    /// Plays routed notes (CTRL present bit 3, events 6 and 7); false acts as
    /// a firmware that can't, which leaves bit 3 out of present_seen.
    public var playsNotes = true
    /// Keys held by routed notes, and the notes sounding (held, or let go
    /// while the sustain pedal is down).
    public private(set) var keysDown: Set<UInt8> = []
    public private(set) var sounding: Set<UInt8> = []
    public private(set) var sustainDown = false
    private var ctrlAt = Date.distantPast
    private var lpLeds = [UInt8](repeating: 0, count: CtrlReply.lpCount)
    private var xlLeds = [UInt8](repeating: 0, count: CtrlReply.xlCount)

    public init(viaSysEx: Bool = true, unit: UInt32 = 0xBE17_0001, name: String = "Belt (demo)") {
        self.viaSysEx = viaSysEx
        self.unit = unit
        self.name = name
        self.seed = Double(unit % 997) / 997
        let pct: [String: Any] = ["kind": "linear", "lo": 0, "hi": 100, "unit": "%"]
        func sel(_ labels: [String]) -> [String: Any] { ["kind": "enum", "labels": labels] }
        let perf: [(String, [String: Any])] = [
            ("Key", sel(BeltTheory.keys)), ("Scale", sel(BeltTheory.scales)), ("Retune", pct),
            ("Amount", pct), ("Harmony", pct), ("Formant", ["kind": "linear", "lo": -100, "hi": 100]),
            ("Voice 1", sel(BeltTheory.intervals)), ("Voice 2", sel(BeltTheory.intervals)),
            ("Voice 3", sel(BeltTheory.intervals)), ("Voice 4", sel(BeltTheory.intervals)),
            ("Doubler", pct), ("Spread", pct),
        ]
        let perfNames = perf.map { $0.0 }
        var fields: [[String: Any]] = []
        for (i, (n, disp)) in perf.enumerated() {
            let f: [String: Any] = ["id": BeltFields.perf[i], "name": n, "off": i * 4, "type": "f32",
                                    "page": i / 6, "pot": i % 6, "def": 0.5, "disp": disp]
            fields.append(f)
        }
        let settingsOff = perfNames.count * 4
        let settings: [[String: Any]] = [
            ["id": "s0.0", "name": "Brightness", "off": 0, "type": "f32", "page": 0, "pot": 0,
             "disp": ["kind": "brightness", "lo": 0.1, "hi": 1.0] as [String: Any]],
            ["id": "s0.1", "name": "Tracking", "off": 4, "type": "enum", "zones": 3, "page": 0, "pot": 1,
             "disp": ["kind": "enum", "labels": ["Fast", "Normal", "Smooth"]] as [String: Any]],
            ["id": "usb", "name": "USB port", "off": 5, "type": "enum", "zones": 5, "page": 0, "pot": 5,
             "disp": ["kind": "enum", "labels": ["Mac", "Launchpad", "Audio", "Phone", "Phone+Audio"]] as [String: Any]],
        ]
        let size = settingsOff + 6
        let module: [String: Any] = ["id": "belt_alchemy", "name": name, "fw": "demo",
                                     "git": "0000000", "sdk": "demo", "board": "v2"]
        let perfC: [String: Any] = ["id": "perf", "kind": "pager", "hash": 1, "size": settingsOff, "off": 0,
                                   "pages": 2, "pots": 6, "pageNames": ["Play", "Setup"],
                                   "pageColors": ["#ff9020", "#c060ff"], "fields": fields]
        let gestures: [[String: Any]] = [["page": 0, "pot": 3, "name": "Preset Slot"],
                                         ["page": 0, "pot": 4, "name": "Save / Load"]]
        let setup: [String: Any] = ["id": "settings", "kind": "settings", "hash": 2, "size": 6, "off": settingsOff,
                                    "pages": 1, "pageNames": ["Settings"], "fields": settings, "gestures": gestures]
        let storage: [String: Any] = ["sd": true, "fsv": 1]
        let root: [String: Any] = [
            "dv": 1, "module": module, "schemaHash": 0xB31_7A1C, "size": size,
            "storage": storage, "phone": ["v": 2], "components": [perfC, setup],
        ]
        descJSON = Array((try? JSONSerialization.data(withJSONObject: root, options: [.sortedKeys])) ?? Data())
        var l = [UInt8](repeating: 0, count: size)
        // Key A, Major, quick retune; voices +3rd and +5th; a little doubler.
        let start: [Float] = [9.5 / 12, 1.5 / 9, 0.2, 0.8, 0.6, 0.5,
                              7.5 / 12, 9.5 / 12, 0.5 / 12, 0.5 / 12, 0.15, 0.7]
        for i in 0..<perfNames.count { Self.putF32(&l, i * 4, start[i]) }
        Self.putF32(&l, settingsOff, 0.8)
        l[settingsOff + 5] = 3
        live = l
        files["/alchemy/belt_alchemy.bin"] = [UInt8](repeating: 0xA5, count: 371_912)
        files["/alchemy/smack_alchemy.bin"] = [UInt8](repeating: 0x5A, count: 298_004)
        files["/samples/kick.wav"] = [UInt8](repeating: 0, count: 88_244)
        files["/samples/hat.wav"] = [UInt8](repeating: 0, count: 21_044)
        slots[0] = live
    }

    static func putF32(_ b: inout [UInt8], _ at: Int, _ v: Float) {
        let bits = v.bitPattern
        b[at] = UInt8(bits & 0xFF); b[at + 1] = UInt8((bits >> 8) & 0xFF)
        b[at + 2] = UInt8((bits >> 16) & 0xFF); b[at + 3] = UInt8(bits >> 24)
    }

    static func f32(_ b: [UInt8], _ at: Int) -> Float {
        Float(bitPattern: UInt32(b[at]) | UInt32(b[at + 1]) << 8 | UInt32(b[at + 2]) << 16 | UInt32(b[at + 3]) << 24)
    }

    // MARK: ByteTransport

    public func setReceiveHandler(_ h: @escaping ([UInt8]) -> Void) { q.sync { handler = h } }

    public func send(_ bytes: [UInt8]) {
        q.async { [self] in
            var raw: [UInt8] = []
            if viaSysEx {
                for m in SysExPipe.encodeStream(bytes) { hostDecoder.feed(m) { raw.append($0) } }
            } else { raw = bytes }
            var frames: [Frame] = []
            for b in raw { splitter.feed(b) { frames.append($0) } }
            for f in frames { handle(f) }
        }
    }

    private func emit(_ f: Frame) {
        var out: [UInt8] = []
        if viaSysEx {
            for m in SysExPipe.encodeStream(f.wire()) { phoneDecoder.feed(m) { out.append($0) } }
        } else { out = f.wire() }
        let h = handler
        DispatchQueue.global().async { h?(out) }
    }

    private func ok(_ f: Frame, _ w: Writer = Writer()) { emit(Frame(type: f.type | 0x80, seq: f.seq, body: [0] + w.bytes)) }
    private func fail(_ f: Frame, _ s: Status) { emit(Frame(type: f.type | 0x80, seq: f.seq, body: [s.rawValue])) }

    // MARK: commands

    private var blob: (target: UInt8, len: Int, data: [UInt8])?

    private func handle(_ f: Frame) {
        var r = Reader(f.body)
        guard let cmd = Cmd(rawValue: f.type) else { return fail(f, .unsupported) }
        do {
            switch cmd {
            case .hello:
                ok(f, Writer().u8(1).u8(2).u8(16).u8(0).raw([UInt8](repeating: 0, count: 12))
                    .u32(schemaHash).u32(4096).u32(UInt32(descJSON.count)).u32(CRC32.of(descJSON))
                    .u16(512).u16(UInt16(live.count))
                    .str("belt_alchemy").str(name).str("demo").str("0000000").str("demo"))
            case .descriptor:
                let off = Int(try r.u32()), n = min(Int(try r.u16()), max(0, descJSON.count - off))
                ok(f, Writer().u32(UInt32(off)).u16(UInt16(n)).raw(n > 0 ? Array(descJSON[off..<(off + n)]) : []))
            case .getLive: try chunk(f, &r, prefix: nil, live)
            case .readSlot:
                let s = Int(try r.u8())
                guard let b = slots[s] else { return fail(f, .badSlot) }
                try chunk(f, &r, prefix: UInt8(s), b)
            case .listSlots:
                var w = Writer().u8(16)
                for s in 0..<16 {
                    let b = slots[s]
                    w = w.u8(b == nil ? 0 : 3).u32(UInt32(s)).u16(UInt16(b?.count ?? 0)).u32(b == nil ? 0 : schemaHash)
                }
                ok(f, w)
            case .blobBegin:
                let t = try r.u8(), len = Int(try r.u32()), hash = try r.u32()
                guard hash == schemaHash, len == live.count else { return fail(f, .schemaMismatch) }
                blob = (t, len, []); ok(f)
            case .blobData:
                _ = try r.u32()
                blob?.data += r.rest(); ok(f)
            case .blobCommit:
                let crc = try r.u32()
                guard let b = blob, b.data.count == b.len else { return fail(f, .badState) }
                guard CRC32.of(b.data) == crc else { return fail(f, .badCrc) }
                if b.target == liveTarget { noteTouch(old: live, new: b.data); live = b.data }
                else { slots[Int(b.target)] = b.data }
                blob = nil; ok(f)
            case .saveToSlot: slots[Int(try r.u8())] = live; ok(f)
            case .loadFromSlot:
                guard let b = slots[Int(try r.u8())] else { return fail(f, .badSlot) }
                live = b; ok(f)
            case .eraseSlot: slots[Int(try r.u8())] = nil; ok(f)
            case .fsInfo: ok(f, Writer().u8(1).u8(3).u8(1).u32(15_558_144).u32(15_100_000).u16(64))
            case .fsList: try fsList(f, &r)
            case .fsOpenWrite:
                let flags = try r.u8(), total = Int(try r.u32()), path = try r.str()
                if files[path] != nil && flags & 1 == 0 { return fail(f, .exists) }
                upload = (path, total, [], flags & 1 != 0); ok(f, Writer().u32(0))
            case .fsWrite:
                let off = Int(try r.u32())
                guard var u = upload, off == u.staged.count else { return fail(f, .badArgs) }
                u.staged += r.rest(); upload = u
                ok(f, Writer().u32(UInt32(u.staged.count)))
            case .fsCommit:
                let crc = try r.u32()
                guard let u = upload, u.staged.count == u.total else { return fail(f, .badState) }
                guard CRC32.of(u.staged) == crc else { return fail(f, .badCrc) }
                files[u.path] = u.staged
                dirs.insert((u.path as NSString).deletingLastPathComponent)
                upload = nil; ok(f)
            case .fsDelete:
                let p = try r.str()
                if files.removeValue(forKey: p) == nil {
                    guard dirs.contains(p) else { return fail(f, .noFile) }
                    guard !files.keys.contains(where: { $0.hasPrefix(p + "/") }) else { return fail(f, .badState) }
                    dirs.remove(p)
                }
                ok(f)
            case .fsMkdir:
                let p = try r.str()
                guard !dirs.contains(p) else { return fail(f, .exists) }
                dirs.insert(p); ok(f)
            case .fsRename:
                let a = try r.str(), b = try r.str()
                guard let d = files[a] else { return fail(f, .noFile) }
                guard files[b] == nil else { return fail(f, .exists) }
                files[a] = nil; files[b] = d; ok(f)
            case .phoneInfo:
                ok(f, Writer().u8(2).u8(supportsCtrl ? 31 : 15).u16(UInt16(PanelLayout.leds.count)).str("alchemy-lab-v2").u32(unit))
            case .state:
                let since: UInt16? = r.remaining >= 2 ? try r.u16() : nil
                ok(f, stateReply(since: since))
            case .screen:
                frame &+= 1
                ok(f, Writer().u32(frame).u16(UInt16(PanelLayout.leds.count)).raw(screenFrame()))
            case .fwList:
                let bins = files.keys.filter { $0.hasPrefix("/alchemy/") && $0.hasSuffix(".bin") }.sorted()
                var w = Writer().u8(UInt8(bins.count))
                for b in bins { w = w.str(String(b.dropFirst("/alchemy/".count))).u32(UInt32(files[b]!.count)) }
                ok(f, w)
            case .fwFlash:
                let name = try r.str()
                guard files["/alchemy/" + name] != nil else { return fail(f, .noFile) }
                guard fwState == .idle || fwState == .same || fwState == .error else { return fail(f, .busy) }
                fwState = .comparing; fwProgress = 0; ok(f)
            case .fwStatus:
                advanceFlash()
                ok(f, Writer().u8(fwState.rawValue).u8(UInt8(fwProgress * 255)).str(""))
            case .press:
                let b = try r.u8(), g = try r.u8()
                guard b <= 1, g <= 2 else { return fail(f, .badArgs) }
                presses.append((Int(b), g))
                switch (b, g) {
                case (0, 0): hardLatch.toggle()
                case (1, 0): muteLatch.toggle()
                case (0, _): hardDown = g == 1
                default: muteDown = g == 1
                }
                ok(f)
            case .ctrl:
                guard supportsCtrl else { return fail(f, .unsupported) }
                let present = CtrlPresent(rawValue: try r.u8()), n = Int(try r.u8())
                var events: [CtrlEvent] = []
                for _ in 0..<n {
                    guard let e = CtrlEvent(bytes: try r.bytes(4)) else { return fail(f, .badArgs) }
                    events.append(e)
                }
                ok(f, ctrl(present: present, events: events))
            default: fail(f, .unsupported)
            }
        } catch { fail(f, .badArgs) }
    }

    // MARK: controllers (CTRL)

    /// A pressed grid pad lights green while held, a side button white; top
    /// buttons 1 and 2 latch HARD and mute, and the top row shows both (red
    /// and amber, dim when off); an XL knob's LED shows its value, an XL
    /// button flips red. A controller whose bit clears, or a phone that
    /// stops asking for a second, lets go of everything held.
    private func ctrl(present asked: CtrlPresent, events: [CtrlEvent]) -> Writer {
        let present = playsNotes ? asked : asked.subtracting(.keyboard)
        if Date().timeIntervalSince(ctrlAt) > 1 { release(ctrlPresent) }
        ctrlAt = Date()
        release(ctrlPresent.subtracting(present))
        ctrlPresent = present
        for e in events where present.contains(e.source) {
            ctrlEvents.append(e)
            switch e {
            case let .launchpad(k, x, y, down):
                let i = LaunchpadMK3.index(k, x: Int(x), y: Int(y))
                switch k {
                case .grid: lpLeds[i] = down ? 21 : 0
                case .side: lpLeds[i] = down ? 3 : 0
                case .top:
                    if down && x == 0 { hardLatch.toggle() }
                    if down && x == 1 { muteLatch.toggle() }
                }
            case let .xlKnob(row, col, v):
                xlLeds[Int(row) * 8 + Int(col)] = v == 0 ? LaunchControlXL.colour(red: 0, green: 0)
                    : LaunchControlXL.colour(red: 0, green: min(3, v / 32 + 1))
            case let .xlButton(row, col, down):
                let i = 24 + Int(row) * 8 + Int(col)
                if down { xlLeds[i] = xlLeds[i] == LaunchControlXL.colour(red: 3, green: 0)
                    ? LaunchControlXL.colour(red: 0, green: 0) : LaunchControlXL.colour(red: 3, green: 0) }
            case .xlFader: break
            case let .gamepad(m): gamepadMask = m
            case let .keyNote(n, v):
                if v > 0 { keysDown.insert(n); sounding.insert(n) }
                else { keysDown.remove(n); if !sustainDown { sounding.remove(n) } }
            case let .keySustain(down):
                sustainDown = down
                if !down { sounding = keysDown }
            }
        }
        let hard = hardLatch != hardDown, muted = muteLatch != muteDown
        lpLeds[LaunchpadMK3.index(.top, x: 0, y: 0)] = hard ? 5 : 7
        lpLeds[LaunchpadMK3.index(.top, x: 1, y: 0)] = muted ? 13 : 15
        return Writer().u8(1).u8(present.rawValue).raw(lpLeds).raw(xlLeds)
    }

    private func release(_ gone: CtrlPresent) {
        if gone.contains(.launchpad) { for i in 0..<72 { lpLeds[i] = 0 } }
        if gone.contains(.gamepad) { gamepadMask = 0 }
        if gone.contains(.keyboard) { keysDown = []; sounding = []; sustainDown = false }
    }

    // MARK: the pretend singer (STATE)

    private func norm(_ i: Int) -> Double { Double(Self.f32(live, i * 4)) }
    private func zone(_ i: Int, _ n: Int) -> Int { max(0, min(n - 1, Int(norm(i) * Double(n)))) }

    var key: Int { zone(0, 12) }
    var scale: Int { zone(1, 9) }

    /// A phrase in the key: scale degrees, two beats each, with breaths,
    /// sung a little flat or sharp, with vibrato. Deterministic in the frame.
    func sung(atFrame n: Int) -> Double? {
        let t = Double(n) / 60 + seed * 7
        let beat = Int(t / 0.45)
        if beat % 9 == 8 { return nil }                       // a breath
        let phrase = [0, 2, 4, 5, 4, 2, 1, 0, 4, 7, 5, 4, 2, 0, -1, 0]
        let deg = BeltTheory.degrees(scale == 0 ? 1 : scale)
        let step = phrase[(beat / 2) % phrase.count]
        let o = Int((Double(step) / Double(deg.count)).rounded(.down))
        let j = ((step % deg.count) + deg.count) % deg.count
        let note = Double(57 + key % 12 + 12 * o + deg[j])
        let scoop = max(0, 0.6 - (t.truncatingRemainder(dividingBy: 0.9)) * 4) * -0.5
        let wander = 0.22 * sin(t * 0.7 + seed * 5)
        let vibrato = 0.18 * sin(t * 2 * .pi * 5.5) * min(1, t.truncatingRemainder(dividingBy: 0.9) * 2)
        return note + scoop + wander + vibrato
    }

    private func frameNow() -> Int { Int(Date().timeIntervalSince(born) * 60) }

    private func stateReply(since: UInt16?) -> Writer {
        let n = frameNow()
        var w = Writer().u16(UInt16(truncatingIfNeeded: n)).u8(0).u8(0)
        if let t = touch {
            let age = min(65_534, Int(Date().timeIntervalSince(t.at) * 1000))
            w = w.u8(UInt8(t.page)).u8(UInt8(t.pot)).u16(UInt16(age))
        } else {
            w = w.u8(0xFF).u8(0xFF).u16(0xFFFF)
        }
        let s = sung(atFrame: n)
        let inDb = s == nil ? -54.0 : -14 + 4 * sin(Double(n) / 23)
        let lvl = { (db: Double) in UInt8(max(0, min(255, ((db + 60) * 255 / 60).rounded()))) }
        w = w.u8(UInt8(max(0, min(255, pretendCpu * 255)))).u8(lvl(inDb)).u8(lvl(inDb + 2))
        // Belt's tail
        w = w.u8(UInt8(ascii: "B")).u8(1).u8(UInt8(key)).u8(UInt8(scale))
        for v in 6..<10 { w = w.u8(UInt8(zone(v, 12))) }
        let hard = hardLatch != hardDown, muted = muteLatch != muteDown
        let flags = (s != nil ? 1 : 0) | (hard ? 2 : 0) | (muted ? 4 : 0) | (hardLatch ? 8 : 0) | (muteLatch ? 16 : 0)
        w = w.u8(UInt8(flags))
        let cents = { (x: Double?) -> Int16 in x.map { Int16(($0 * 100).rounded()) } ?? BeltState.unvoiced }
        let target = s.map { (x: Double) -> Double in
            var best = x.rounded(), d = 99.0
            for c in Int(x) - 6...Int(x) + 6 where BeltTheory.inScale(c, key: key, scale: scale)
                && Swift.abs(Double(c) - x) < d { d = Swift.abs(Double(c) - x); best = Double(c) }
            return best
        }
        w = w.u16(UInt16(bitPattern: cents(s)))
        w = w.u16(UInt16(bitPattern: Int16(((s ?? 0) - (target ?? 0)) * 100)))
        let last = n - 1
        var count = since.map { Int(UInt16(truncatingIfNeeded: last) &- $0) } ?? 32
        count = max(0, min(32, count, last + 1))
        w = w.u16(UInt16(truncatingIfNeeded: last)).u8(UInt8(count))
        for k in (last - count + 1)..<(last + 1) {
            w = w.u16(UInt16(bitPattern: cents(sung(atFrame: k))))
        }
        return w
    }

    /// The firmware notices a pot moved by hand; here, a knob moved from the
    /// phone counts the same way, so the demo's pop-up has something to show.
    private func noteTouch(old: [UInt8], new: [UInt8]) {
        for i in 0..<12 where i * 4 + 4 <= old.count && i * 4 + 4 <= new.count {
            if Swift.abs(Self.f32(old, i * 4) - Self.f32(new, i * 4)) > 0.004 {
                touch = (i / 6, i % 6, Date())
            }
        }
    }

    private func chunk(_ f: Frame, _ r: inout Reader, prefix: UInt8?, _ src: [UInt8]) throws {
        let off = Int(try r.u32()), max = Int(try r.u16())
        let n = Swift.max(0, Swift.min(max, src.count - off))
        ok(f, Writer().u8(prefix ?? liveTarget).u32(UInt32(off)).u16(UInt16(n)).u16(UInt16(src.count))
            .u32(schemaHash).raw(n > 0 ? Array(src[off..<(off + n)]) : []))
    }

    private func fsList(_ f: Frame, _ r: inout Reader) throws {
        _ = try r.u32()
        let path = try r.str()
        guard dirs.contains(path) else { return fail(f, .noFile) }
        let prefix = path == "/" ? "/" : path + "/"
        var names: [(String, Bool, Int)] = []
        for d in dirs where d != path && d.hasPrefix(prefix) && !d.dropFirst(prefix.count).contains("/") {
            names.append((String(d.dropFirst(prefix.count)), true, 0))
        }
        for (p, b) in files where p.hasPrefix(prefix) && !p.dropFirst(prefix.count).contains("/") {
            names.append((String(p.dropFirst(prefix.count)), false, b.count))
        }
        var w = Writer().u32(0xFFFF_FFFF).u8(UInt8(names.count))
        for (n, dir, size) in names.sorted(by: { $0.0 < $1.0 }) {
            w = w.u8(dir ? 1 : 0).u32(UInt32(size)).u32(0).str(n)
        }
        ok(f, w)
    }

    private func advanceFlash() {
        switch fwState {
        case .comparing, .erasing, .writing, .verifying:
            fwProgress += 0.12
            if fwProgress >= 1 {
                fwProgress = 0
                fwState = FwState(rawValue: fwState.rawValue + 1) ?? .done
            }
        case .done: fwState = .idle   // a real module reboots here
        default: break
        }
    }

    /// Each pot's ring shows its page-1 value as an arc; buttons glow dim.
    private func screenFrame() -> [UInt8] {
        var rgb = [UInt8](repeating: 0, count: 3 * PanelLayout.leds.count)
        let flashing = fwState != .idle && fwState != .same && fwState != .error
        for led in PanelLayout.leds {
            let i = led.index * 3
            if let p = led.pot {
                let ring = PanelLayout.leds.filter { $0.pot == p }.map { $0.index }.sorted()
                let k = ring.firstIndex(of: led.index) ?? 0
                let v = flashing ? fwProgress : Double(Self.f32(live, p * 4))
                if Double(k) < v * 13 + 0.5 && k < 13 {
                    rgb[i] = flashing ? 0x20 : 0xFF; rgb[i + 1] = flashing ? 0x60 : 0xA0; rgb[i + 2] = flashing ? 0xFF : 0x30
                } else if k < 13 {
                    rgb[i] = 0x10; rgb[i + 1] = 0x0C; rgb[i + 2] = 0x08
                }
            } else {
                rgb[i] = 0x30; rgb[i + 1] = 0x30; rgb[i + 2] = 0x30
            }
        }
        return rgb
    }
}
