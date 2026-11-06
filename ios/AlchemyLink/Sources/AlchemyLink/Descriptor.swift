// The module's self-description (protocol §5): which bytes of the live
// state are which knob, how to show them, and how the pages are grouped.
// Mirrors fieldsOf / decodeState / encodeState / show() in the web
// Control page, so the iPhone and the browser agree on every value.

import Foundation

public struct Disp {
    public var kind: String
    public var labels: [String]?
    public var lo: Double?, hi: Double?, scale: Double?
    public var unit: String?

    init(_ d: [String: Any]?) {
        kind = d?["kind"] as? String ?? "norm"
        labels = d?["labels"] as? [String]
        lo = (d?["lo"] as? NSNumber)?.doubleValue
        hi = (d?["hi"] as? NSNumber)?.doubleValue
        scale = (d?["scale"] as? NSNumber)?.doubleValue
        unit = d?["unit"] as? String
    }
}

public struct Field: Identifiable {
    public var id: String
    public var name: String
    public var abs: Int            // byte offset in the live state
    public var isEnum: Bool        // "enum": 1 byte; otherwise "f32"
    public var zones: Int
    public var page: Int?, pot: Int?
    public var disp: Disp
    public var help: String?

    /// The USB port setting: changing it from the phone would cut the phone
    /// off at the next boot, so it stays on the module (as on the web page).
    public var severs: Bool {
        id == "usb" || ["usb", "usb port"].contains(name.lowercased())
    }

    /// The text the module's value means, like the web page shows it.
    public func show(_ v: Double) -> String {
        if isEnum {
            let i = Int(v)
            if let l = disp.labels, i >= 0, i < l.count { return l[i] }
            return String(i)
        }
        let n = disp.labels?.count ?? 0
        switch disp.kind {
        case "snap" where n > 1:
            return disp.labels![max(0, min(n - 1, Int((v * Double(n - 1)).rounded())))]
        case "enum" where n > 0:
            return disp.labels![max(0, min(n - 1, Int(v * Double(n))))]
        case "percent": return "\(Int((v * (disp.scale ?? 100)).rounded()))%"
        case "bipolar": return "\(Int(((v * 2 - 1) * 100).rounded()))%"
        case "brightness":
            let lo = disp.lo ?? 0, hi = disp.hi ?? 1
            return "\(Int(((lo + (hi - lo) * v) * 100).rounded()))%"
        case "gain": return v > 0 ? String(format: "%.1f dB", 20 * log10(v)) : "−∞ dB"
        case "linear":
            let lo = disp.lo ?? 0, hi = disp.hi ?? 1, x = lo + (hi - lo) * v
            let num = Swift.abs(hi - lo) >= 20 ? String(Int(x.rounded())) : String(format: "%.2f", x)
            return withUnit(num)
        case "exp":
            let lo = disp.lo ?? 1, hi = disp.hi ?? 10, x = lo * pow(hi / lo, v)
            let num = x >= 100 ? String(Int(x.rounded())) : String(format: "%.3g", x)
            return withUnit(num)
        default: return "\(Int((v * 100).rounded()))%"
        }
    }

    private func withUnit(_ num: String) -> String {
        guard let u = disp.unit, !u.isEmpty else { return num }
        return u == "%" ? num + "%" : num + " " + u
    }

    /// Positions a stepped knob has (0 = continuous).
    public var steps: Int {
        if isEnum { return zones }
        switch disp.kind {
        case "snap": return max(0, (disp.labels?.count ?? 1) - 1)
        case "enum": return disp.labels?.count ?? 0
        default: return 0
        }
    }

    /// Snaps a 0…1 value the way the web page does before it writes.
    public func quantize(_ v: Double) -> Double {
        let c = min(1, max(0, v))
        switch disp.kind {
        case "snap" where steps > 0: return (c * Double(steps)).rounded() / Double(steps)
        case "enum" where steps > 0: return (Double(min(steps - 1, Int(c * Double(steps)))) + 0.5) / Double(steps)
        default: return c
        }
    }
}

public struct PanelGesture: Identifiable {
    public var id: String { "\(page).\(pot)" }
    public var page: Int, pot: Int, name: String
}

/// One tab of knobs: a pager page, a settings page, or a custom group.
public struct ControlPage: Identifiable {
    public var id: String
    public var name: String
    public var color: String?
    public var isSettings: Bool
    public var grid: Bool               // six pot positions, like the panel
    public var fields: [Field]
    public var gestures: [PanelGesture]

    /// For a grid page: what sits at each pot, 0…5.
    public func at(pot: Int) -> Field? { fields.first { $0.pot == pot } }
}

public struct Descriptor {
    public var moduleId: String, moduleName: String, fw: String
    public var size: Int
    public var fields: [Field]
    public var pages: [ControlPage]
    public var hasStorage: Bool
    public var hasPhoneBlock: Bool

    public init(json: Data) throws {
        guard let root = try JSONSerialization.jsonObject(with: json) as? [String: Any] else {
            throw HostLinkError.badReply("descriptor is not a JSON object")
        }
        let m = root["module"] as? [String: Any] ?? [:]
        moduleId = m["id"] as? String ?? ""
        moduleName = m["name"] as? String ?? moduleId
        fw = m["fw"] as? String ?? ""
        size = (root["size"] as? NSNumber)?.intValue ?? 0
        hasStorage = (root["storage"] as? [String: Any])?["sd"] as? Bool ?? false
        hasPhoneBlock = root["phone"] != nil

        var all: [Field] = []
        var pages: [ControlPage] = []
        for c in root["components"] as? [[String: Any]] ?? [] {
            let cid = c["id"] as? String ?? "?"
            let kind = c["kind"] as? String ?? ""
            let off = (c["off"] as? NSNumber)?.intValue ?? 0
            let fs: [Field] = (c["fields"] as? [[String: Any]] ?? []).compactMap { f in
                guard let id = f["id"] as? String, let foff = (f["off"] as? NSNumber)?.intValue else { return nil }
                let type = f["type"] as? String ?? "f32"
                guard type == "f32" || type == "enum" else { return nil }
                return Field(id: id, name: f["name"] as? String ?? id, abs: off + foff, isEnum: type == "enum",
                             zones: (f["zones"] as? NSNumber)?.intValue ?? 0,
                             page: (f["page"] as? NSNumber)?.intValue, pot: (f["pot"] as? NSNumber)?.intValue,
                             disp: Disp(f["disp"] as? [String: Any]), help: f["help"] as? String)
            }
            all += fs
            if kind == "pager" || kind == "settings" {
                let n = (c["pages"] as? NSNumber)?.intValue ?? ((fs.compactMap { $0.page }.max() ?? 0) + 1)
                let names = c["pageNames"] as? [String] ?? []
                let colors = c["pageColors"] as? [String] ?? []
                let gs: [PanelGesture] = (c["gestures"] as? [[String: Any]] ?? []).compactMap { g in
                    guard let p = (g["page"] as? NSNumber)?.intValue, let pot = (g["pot"] as? NSNumber)?.intValue else { return nil }
                    return PanelGesture(page: p, pot: pot, name: g["name"] as? String ?? "")
                }
                for p in 0..<max(0, n) {
                    let pf = fs.filter { ($0.page ?? 0) == p }
                    let pg = gs.filter { $0.page == p }
                    if pf.filter({ !$0.severs }).isEmpty && pg.isEmpty { continue }
                    let named = p < names.count && !names[p].isEmpty ? names[p] : nil
                    let fallback = kind == "settings" ? "Settings \(p + 1)" : "Page \(p + 1)"
                    pages.append(ControlPage(id: "\(cid).\(p)", name: named ?? fallback,
                                             color: p < colors.count && !colors[p].isEmpty ? colors[p] : nil,
                                             isSettings: kind == "settings", grid: true, fields: pf, gestures: pg))
                }
            } else if !fs.isEmpty {
                pages.append(ControlPage(id: cid, name: c["name"] as? String ?? cid, color: nil,
                                         isSettings: false, grid: false, fields: fs, gestures: []))
            }
        }
        fields = all
        self.pages = pages
    }

    public func field(_ id: String) -> Field? { fields.first { $0.id == id } }

    /// Every field's value in the live state bytes.
    public func decode(_ bytes: [UInt8]) -> [String: Double] {
        var v: [String: Double] = [:]
        for f in fields where f.abs < bytes.count {
            if f.isEnum { v[f.id] = Double(bytes[f.abs]) }
            else if f.abs + 4 <= bytes.count {
                let bits = UInt32(bytes[f.abs]) | UInt32(bytes[f.abs + 1]) << 8
                    | UInt32(bytes[f.abs + 2]) << 16 | UInt32(bytes[f.abs + 3]) << 24
                v[f.id] = Double(Float(bitPattern: bits))
            }
        }
        return v
    }

    /// A copy of the state with some fields changed; every other byte
    /// (opaque components, names) round-trips untouched.
    public func encode(_ bytes: [UInt8], changes: [String: Double]) -> [UInt8] {
        var out = bytes
        for (id, val) in changes {
            guard let f = field(id) else { continue }
            if f.isEnum {
                guard f.abs < out.count else { continue }
                let top = Double(max(1, f.zones == 0 ? 256 : f.zones) - 1)
                out[f.abs] = UInt8(min(top, max(0, val.rounded())))
            } else {
                guard f.abs + 4 <= out.count else { continue }
                let bits = Float(min(1, max(0, val))).bitPattern
                out[f.abs] = UInt8(bits & 0xFF)
                out[f.abs + 1] = UInt8((bits >> 8) & 0xFF)
                out[f.abs + 2] = UInt8((bits >> 16) & 0xFF)
                out[f.abs + 3] = UInt8(bits >> 24)
            }
        }
        return out
    }
}
