// A pretend Alchemy Lab in the app, so the iPhone app runs in the Simulator
// and the tests run on a Mac with no module plugged in. It answers the
// same commands Belt answers in Phone mode, from memory: two knob pages and
// a settings page, 16 preset slots, an SD card with a few files, the LED
// screen drawn from the knob values, and a firmware switch that only
// pretends to flash. With `viaSysEx`, every byte goes through the SysEx
// pipe both ways, exactly as it does over the USB cable.

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

    public init(viaSysEx: Bool = true) {
        self.viaSysEx = viaSysEx
        let perfNames = ["Tension", "Mass", "Damping", "Bright", "Drive", "Mix",
                         "Rate", "Depth", "Spread", "Tone", "Space", "Level"]
        var fields: [[String: Any]] = []
        for (i, n) in perfNames.enumerated() {
            let disp: [String: Any] = i == 5 || i == 11 ? ["kind": "percent", "scale": 100] : ["kind": "norm"]
            let f: [String: Any] = ["id": "p\(i / 6).\(i % 6)", "name": n, "off": i * 4, "type": "f32",
                                    "page": i / 6, "pot": i % 6, "def": 0.5, "disp": disp]
            fields.append(f)
        }
        let settingsOff = perfNames.count * 4
        let settings: [[String: Any]] = [
            ["id": "s0.0", "name": "Brightness", "off": 0, "type": "f32", "page": 0, "pot": 0,
             "disp": ["kind": "brightness", "lo": 0.1, "hi": 1.0] as [String: Any]],
            ["id": "s0.1", "name": "Scale", "off": 4, "type": "enum", "zones": 3, "page": 0, "pot": 1,
             "disp": ["kind": "enum", "labels": ["Major", "Minor", "Free"]] as [String: Any]],
            ["id": "usb", "name": "USB port", "off": 5, "type": "enum", "zones": 4, "page": 0, "pot": 5,
             "disp": ["kind": "enum", "labels": ["Mac", "Launchpad", "Audio", "Phone"]] as [String: Any]],
        ]
        let size = settingsOff + 6
        let module: [String: Any] = ["id": "belt_alchemy", "name": "Belt (demo)", "fw": "demo",
                                     "git": "0000000", "sdk": "demo", "board": "v2"]
        let perf: [String: Any] = ["id": "perf", "kind": "pager", "hash": 1, "size": settingsOff, "off": 0,
                                   "pages": 2, "pots": 6, "pageNames": ["Play", "Space"],
                                   "pageColors": ["#ffb454", "#67e8f9"], "fields": fields]
        let gestures: [[String: Any]] = [["page": 0, "pot": 3, "name": "Preset Slot"],
                                         ["page": 0, "pot": 4, "name": "Save / Load"]]
        let setup: [String: Any] = ["id": "settings", "kind": "settings", "hash": 2, "size": 6, "off": settingsOff,
                                    "pages": 1, "pageNames": ["Setup"], "fields": settings, "gestures": gestures]
        let storage: [String: Any] = ["sd": true, "fsv": 1]
        let root: [String: Any] = [
            "dv": 1, "module": module, "schemaHash": 0xB31_7A1C, "size": size,
            "storage": storage, "phone": ["v": 1], "components": [perf, setup],
        ]
        descJSON = Array((try? JSONSerialization.data(withJSONObject: root, options: [.sortedKeys])) ?? Data())
        var l = [UInt8](repeating: 0, count: size)
        for i in 0..<perfNames.count { Self.putF32(&l, i * 4, 0.25 + 0.05 * Float(i)) }
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
                    .str("belt_alchemy").str("Belt (demo)").str("demo").str("0000000").str("demo"))
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
                if b.target == liveTarget { live = b.data } else { slots[Int(b.target)] = b.data }
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
            case .phoneInfo: ok(f, Writer().u8(1).u8(7).u16(UInt16(PanelLayout.leds.count)).str("alchemy-lab-v2"))
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
                presses.append((Int(b), g)); ok(f)
            default: fail(f, .unsupported)
            }
        } catch { fail(f, .badArgs) }
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
