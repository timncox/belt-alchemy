// Where each of the Alchemy Lab V2's 102 LEDs sits, so the phone can draw
// the SCREEN frame as the panel looks. From the SDK:
//   chain order and ring winding: hardware/alchemy-lab/v2/src/alchemy_lab_v2_layout.cpp
//   pot and button positions:     panel/front-panel-template.kicad_pcb (mm)
// The ring radius is a drawing choice, not a measurement.

import Foundation

public struct PanelLED {
    public var index: Int          // chain index, = position in the SCREEN frame
    public var x: Double, y: Double   // mm, panel coordinates
    public var pot: Int?           // 0…5 for ring LEDs
    public var button: Int?        // 0…2 for button LEDs
}

public struct PanelControl: Identifiable {
    public var id: String
    public var x: Double, y: Double, r: Double
    public var pot: Int?, button: Int?
}

public enum PanelLayout {
    /// The panel area the controls live in (mm).
    public static let bounds = (minX: 54.0, minY: 40.0, maxX: 119.0, maxY: 123.0)
    public static let ringRadius = 8.6
    public static let knobRadius = 5.0
    public static let buttonRadius = 3.1

    struct Ring { var start: Int, count: Int, startHour: Double, stepHours: Double, x: Double, y: Double }

    /// Pot 1…6 as 0…5.
    static let rings: [Ring] = [
        Ring(start: 69, count: 16, startHour: 3.00, stepHours: 0.75, x: 69.16, y: 53.39),
        Ring(start: 86, count: 16, startHour: 9.75, stepHours: 0.75, x: 104.16, y: 53.39),
        Ring(start: 52, count: 16, startHour: 2.25, stepHours: 0.75, x: 69.16, y: 81.41),
        Ring(start: 35, count: 16, startHour: 9.00, stepHours: -0.75, x: 104.16, y: 81.39),
        Ring(start: 17, count: 16, startHour: 3.00, stepHours: 0.75, x: 69.16, y: 109.42),
        Ring(start: 0, count: 16, startHour: 9.00, stepHours: 0.75, x: 104.16, y: 109.39),
    ]

    /// B1…B3 as 0…2: (bottom LED, top LED, centre).
    static let buttons: [(bottom: Int, top: Int, x: Double, y: Double)] = [
        (68, 85, 86.66, 53.40),
        (34, 51, 86.66, 81.40),
        (16, 33, 86.66, 109.41),
    ]

    public static let leds: [PanelLED] = {
        var out: [PanelLED] = []
        for (p, r) in rings.enumerated() {
            for k in 0..<r.count {
                let hour = r.startHour + r.stepHours * Double(k)
                let a = hour / 12 * 2 * Double.pi          // 0 = 12 o'clock, clockwise
                out.append(PanelLED(index: r.start + k, x: r.x + ringRadius * sin(a),
                                    y: r.y - ringRadius * cos(a), pot: p, button: nil))
            }
        }
        for (b, btn) in buttons.enumerated() {
            out.append(PanelLED(index: btn.bottom, x: btn.x, y: btn.y + 5.2, pot: nil, button: b))
            out.append(PanelLED(index: btn.top, x: btn.x, y: btn.y - 5.2, pot: nil, button: b))
        }
        return out.sorted { $0.index < $1.index }
    }()

    public static let controls: [PanelControl] =
        rings.enumerated().map { PanelControl(id: "pot\($0.offset)", x: $0.element.x, y: $0.element.y, r: knobRadius, pot: $0.offset, button: nil) }
        + buttons.enumerated().map { PanelControl(id: "b\($0.offset)", x: $0.element.x, y: $0.element.y, r: buttonRadius, pot: nil, button: $0.offset) }
}
