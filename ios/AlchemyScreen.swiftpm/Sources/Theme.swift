// The look: the voice is the interface. The pitch field runs edge to edge
// and everything floats over it; nothing is boxed. Type is SF Pro, with the
// expanded width at heavy weight for notes, values and titles. Colour is
// black and greys, Belt's KEY orange for what's active, and the four voice
// colours only for voices. Each knob keeps its firmware ring colour
// (belt_alchemy.cpp kCol*) so it reads the same as on the module.

import SwiftUI
import AlchemyLink

enum Theme {
    static let ground = Color.black                                       // #000, OLED
    static let surface = Color(red: 0.086, green: 0.090, blue: 0.102)    // #16171A
    static let line = Color.white.opacity(0.08)
    static let mist = Color(red: 0.925, green: 0.918, blue: 0.949)       // #ECEAF2
    static let fog = Color(red: 0.541, green: 0.529, blue: 0.580)        // #8A8794
    static let key = led(0xFF9020)
    static let inTune = led(0x5BF58C)
    static let alarm = led(0xFF5A5F)

    // Older names, kept so every screen shares one palette.
    static var plate: Color { ground }
    static var raised: Color { surface }
    static var silk: Color { mist }
    static var silkDim: Color { fog }
    static var ledOff: Color { Color.white.opacity(0.10) }

    static func led(_ hex: UInt32) -> Color {
        Color(red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255, blue: Double(hex & 0xFF) / 255)
    }

    /// Belt's four harmony voices, as their Setup rings (kColV1-V4).
    static let voices: [Color] = [led(0xC060FF), led(0xFF60C0), led(0x60C0FF), led(0x60FFC0)]

    /// Notes, values and titles: SF Pro, expanded, heavy.
    static func display(_ size: CGFloat, weight: Font.Weight = .heavy) -> Font {
        .system(size: size, weight: weight).width(.expanded)
    }
    static func slab(_ size: CGFloat) -> Font { display(size) }
}

/// How a knob's value is drawn, as the firmware's ring draws it.
enum RingKind: Equatable {
    case level                 // filled from the start up to the value
    case selector(zones: Int)  // one point for the zone picked
    case bipolar               // from the middle towards the value
}

/// A knob's colour, its colour below the middle for a bipolar knob, and
/// its kind. Belt's knobs by name; anything else takes its page colour.
struct KnobLook {
    var color: Color
    var below: Color? = nil
    var kind: RingKind = .level

    static func of(_ field: Field, pageColor: String?) -> KnobLook {
        let L = Theme.led
        switch field.name {
        case "Key": return KnobLook(color: L(0xFF9020), kind: .selector(zones: 12))
        case "Scale": return KnobLook(color: L(0xFFC060), kind: .selector(zones: 9))
        case "Retune", "Flex": return KnobLook(color: L(0x40C0FF))
        case "Amount", "Vel sens", "Humanize": return KnobLook(color: L(0x60FFC0))
        case "Harmony": return KnobLook(color: L(0xC060FF))
        case "Formant": return KnobLook(color: L(0xFF60A0), below: L(0x60A0FF), kind: .bipolar)
        case "Voice 1": return KnobLook(color: Theme.voices[0], kind: .selector(zones: 12))
        case "Voice 2": return KnobLook(color: Theme.voices[1], kind: .selector(zones: 12))
        case "Voice 3": return KnobLook(color: Theme.voices[2], kind: .selector(zones: 12))
        case "Voice 4": return KnobLook(color: Theme.voices[3], kind: .selector(zones: 12))
        case "Doubler": return KnobLook(color: L(0xFFFF80))
        case "Spread": return KnobLook(color: L(0x80FFFF))
        case "Lead", "Wet": return KnobLook(color: L(0xFF9020))
        default:
            let c = Color(hex: pageColor) ?? Theme.key
            return KnobLook(color: c, kind: field.isEnum ? .selector(zones: max(2, field.zones)) : .level)
        }
    }
}

/// A knob's value as a thin 270° arc, 7:30 round to 4:30, with a glowing
/// point where it stands.
struct ValueArc: View {
    var value: Double              // 0...1
    var look: KnobLook
    var width: CGFloat = 5

    var body: some View {
        GeometryReader { g in
            let d = min(g.size.width, g.size.height)
            let r = d / 2 - width
            let c = CGPoint(x: g.size.width / 2, y: g.size.height / 2)
            let v = max(0, min(1, value))
            let (from, to, col) = span(v)
            let head = point(c, r, v)
            ZStack {
                ArcShape(from: 0, to: 1, radius: r).stroke(Color.white.opacity(0.09), style: StrokeStyle(lineWidth: width, lineCap: .round))
                if to > from {
                    ArcShape(from: from, to: to, radius: r)
                        .stroke(col, style: StrokeStyle(lineWidth: width, lineCap: .round))
                        .shadow(color: col.opacity(0.55), radius: width * 1.6)
                }
                Circle().fill(Theme.mist).frame(width: width * 1.9, height: width * 1.9)
                    .shadow(color: col.opacity(0.9), radius: width * 1.5)
                    .position(head)
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .accessibilityHidden(true)
    }

    private func span(_ v: Double) -> (Double, Double, Color) {
        switch look.kind {
        case .level: return (0, v, look.color)
        case .selector: return (max(0, v - 0.03), min(1, v + 0.03), look.color)
        case .bipolar: return v >= 0.5 ? (0.5, v, look.color) : (v, 0.5, look.below ?? look.color)
        }
    }

    private func point(_ c: CGPoint, _ r: CGFloat, _ v: Double) -> CGPoint {
        let a = (135 + 270 * v) * .pi / 180
        return CGPoint(x: c.x + r * cos(a), y: c.y + r * sin(a))
    }
}

struct ArcShape: Shape {
    var from: Double, to: Double, radius: CGFloat

    func path(in rect: CGRect) -> Path {
        var p = Path()
        p.addArc(center: CGPoint(x: rect.midX, y: rect.midY), radius: radius,
                 startAngle: .degrees(135 + 270 * from), endAngle: .degrees(135 + 270 * to), clockwise: false)
        return p
    }
}

/// A level as a hairline that fills: mist, warm above -12 dB, red above -3.
struct LevelLine: View {
    var db: Double

    var body: some View {
        GeometryReader { g in
            let f = CGFloat(max(0, min(1, (db + 60) / 60)))
            let c = db > -3 ? Theme.alarm : db > -12 ? Theme.key : Theme.mist
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.10))
                Capsule().fill(c).frame(width: g.size.width * f)
            }
        }
        .frame(height: 3)
        .accessibilityHidden(true)
    }
}

/// The module's views as words, the one you're on large.
struct SectionTabs<T: Hashable & Identifiable>: View {
    var items: [T]
    var title: (T) -> String
    @Binding var selection: T

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(alignment: .lastTextBaseline, spacing: 18) {
                ForEach(items) { it in
                    let on = it == selection
                    Button { withAnimation(.easeOut(duration: 0.2)) { selection = it } } label: {
                        Text(title(it))
                            .font(on ? Theme.display(22) : .system(size: 15, weight: .medium))
                            .foregroundStyle(on ? Theme.mist : Theme.fog)
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(on ? .isSelected : [])
                }
            }
            .padding(.horizontal)
        }
    }
}
