// The look: the module's own panel, lit. Colours are Belt's LED colours as
// the firmware paints them (belt_alchemy.cpp kCol*), each knob in its ring
// colour and ring kind, so a knob looks the same on the phone as on the
// module. Notes and values are set in Alfa Slab One, the face of Belt's
// manual (SIL OFL, Fonts/OFL.txt); everything else stays system type.

import SwiftUI
import UIKit
import CoreText
import AlchemyLink

enum Theme {
    static let plate = Color(red: 0.047, green: 0.039, blue: 0.035)      // #0C0A09, the panel
    static let raised = Color(red: 0.090, green: 0.075, blue: 0.059)     // #17130F, a raised field
    static let silk = Color(red: 0.933, green: 0.898, blue: 0.820)       // #EEE5D1, silkscreen
    static let silkDim = Color(red: 0.549, green: 0.510, blue: 0.451)    // #8C8273
    static let ledOff = Color(red: 0.150, green: 0.125, blue: 0.098)     // #26201A, an unlit LED
    static let key = led(0xFF9020)
    static let inTune = led(0x4CFF7A)
    static let alarm = led(0xFF4A4A)

    static func led(_ hex: UInt32) -> Color {
        Color(red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255, blue: Double(hex & 0xFF) / 255)
    }

    /// Belt's four harmony voices, as their Setup rings (kColV1-V4).
    static let voices: [Color] = [led(0xC060FF), led(0xFF60C0), led(0x60C0FF), led(0x60FFC0)]

    /// The display face, falling back to the system's heaviest rounded face
    /// if the font file isn't in the bundle.
    static func slab(_ size: CGFloat) -> Font {
        registerFonts()
        return fontReady ? .custom("AlfaSlabOne-Regular", size: size) : .system(size: size, weight: .black, design: .rounded)
    }

    private static var fontReady = false
    private static var tried = false
    private static func registerFonts() {
        guard !tried else { return }
        tried = true
        let name = "AlfaSlabOne-Regular"
        var urls: [URL?] = [Bundle.main.url(forResource: name, withExtension: "ttf"),
                            Bundle.main.url(forResource: name, withExtension: "ttf", subdirectory: "Fonts")]
        #if SWIFT_PACKAGE
        urls.append(Bundle.module.url(forResource: name, withExtension: "ttf", subdirectory: "Fonts"))
        urls.append(Bundle.module.url(forResource: name, withExtension: "ttf"))
        #endif
        guard let url = urls.compactMap({ $0 }).first else { return }
        var err: Unmanaged<CFError>?
        fontReady = CTFontManagerRegisterFontsForURL(url as CFURL, .process, &err)
            || UIFont(name: name, size: 12) != nil
    }
}

/// How a knob's ring shows its value, as the firmware draws it.
enum RingKind: Equatable {
    case level                 // lit from the start up to the value
    case selector(zones: Int)  // one LED for the zone picked
    case bipolar               // from the middle notch towards the value
}

/// A knob's look on the module: its LED colour, the colour below the notch
/// for a bipolar ring, and the ring kind. Belt's knobs by name; anything else
/// takes its page colour and a level ring.
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

/// Sixteen LEDs on a 270° arc, 7:30 round to 4:30, as a module ring reads.
struct LEDRing: View {
    var value: Double              // 0...1
    var look: KnobLook
    var count = 16
    var dot: CGFloat? = nil        // LED size; a share of the ring if nil

    var body: some View {
        GeometryReader { g in
            let d = min(g.size.width, g.size.height)
            let led = dot ?? max(4, d * 0.085)
            let r = d / 2 - led / 2 - 2
            let c = CGPoint(x: g.size.width / 2, y: g.size.height / 2)
            ZStack {
                ForEach(0..<count, id: \.self) { i in
                    let a = (135 + 270 * Double(i) / Double(count - 1)) * .pi / 180
                    let col = colour(i)
                    Circle()
                        .fill(col ?? Theme.ledOff)
                        .frame(width: led, height: led)
                        .shadow(color: (col ?? .clear).opacity(0.75), radius: col == nil ? 0 : led * 0.7)
                        .position(x: c.x + r * cos(a), y: c.y + r * sin(a))
                }
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .accessibilityHidden(true)
    }

    private func colour(_ i: Int) -> Color? {
        let v = max(0, min(1, value))
        let pos = v * Double(count - 1)
        switch look.kind {
        case .level:
            return v > 0.001 && Double(i) <= pos + 0.5 ? look.color : nil
        case .selector:
            return i == Int(pos.rounded()) ? look.color : nil
        case .bipolar:
            let mid = Double(count - 1) / 2
            if abs(Double(i) - mid) < 0.6 { return Theme.silk.opacity(0.9) }        // the notch
            if pos > mid, Double(i) > mid, Double(i) <= pos + 0.5 { return look.color }
            if pos < mid, Double(i) < mid, Double(i) >= pos - 0.5 { return look.below ?? look.color }
            return nil
        }
    }
}

/// A level as a row of LEDs: green, then yellow above -12 dB, red above -3.
struct LEDLadder: View {
    var db: Double
    var segments = 16

    var body: some View {
        GeometryReader { g in
            let gap: CGFloat = 2
            let w = (g.size.width - gap * CGFloat(segments - 1)) / CGFloat(segments)
            let lit = Int(((db + 60) / 60 * Double(segments)).rounded())
            HStack(spacing: gap) {
                ForEach(0..<segments, id: \.self) { i in
                    let segDb = -60 + 60 * Double(i + 1) / Double(segments)
                    let c = segDb > -3 ? Theme.alarm : segDb > -12 ? Theme.led(0xFFD040) : Theme.inTune
                    RoundedRectangle(cornerRadius: 1.5)
                        .fill(i < lit ? c : Theme.ledOff)
                        .frame(width: max(1, w))
                }
            }
        }
        .accessibilityHidden(true)
    }
}
