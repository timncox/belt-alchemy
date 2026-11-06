// The on-screen devices (VirtualDevices): a Launchpad Mini MK3 lit with the
// colours the module sends back, a Launch Control XL, a gamepad and a Daisy
// Patch with a monitor of the MIDI it gets. On iPad all four at once (the
// Devices tab); on iPhone one at a time, picked at the top (Controls > On-
// screen devices > Play them). Learn works from here without a sheet over
// the devices until a control moves.

import SwiftUI
import AlchemyLink

struct DevicesScreen: View {
    @EnvironmentObject var rig: Rig
    @EnvironmentObject var hub: ControllerHub
    @Environment(\.horizontalSizeClass) private var hsc
    @AppStorage("devicesPick") private var pick = VirtualDevices.Device.launchpad.rawValue

    var body: some View {
        let content = VStack(spacing: 0) {
            if hub.learning && hub.learned == nil {
                HStack {
                    Label("Move a control on one of the devices", systemImage: "wand.and.stars")
                    Spacer()
                    Button("Cancel") { hub.cancelLearn() }
                }
                .padding(.horizontal).padding(.vertical, 8)
                .background(Color.orange.opacity(0.2))
            }
            if !rig.virtualOn {
                Spacer()
                Text("The on-screen devices are there in the demo and with the module emulator, when On-screen devices is on in Controls.")
                    .multilineTextAlignment(.center).foregroundStyle(.secondary).padding()
                Spacer()
            } else if hsc == .regular {
                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 420), spacing: 16, alignment: .top)], spacing: 16) {
                        ForEach(VirtualDevices.Device.allCases) { DevicePanel(device: $0) }
                    }
                    .padding()
                }
            } else {
                Picker("Device", selection: $pick) {
                    ForEach(VirtualDevices.Device.allCases) { d in
                        Image(systemName: d.icon).accessibilityLabel(d.title).tag(d.rawValue)
                    }
                }
                .pickerStyle(.segmented)
                .padding(.horizontal).padding(.vertical, 8)
                ScrollView {
                    DevicePanel(device: VirtualDevices.Device(rawValue: pick) ?? .launchpad)
                        .padding(.horizontal)
                }
            }
        }
        .navigationTitle("Devices")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button { hub.beginLearn(inPlace: true) } label: { Label("Learn", systemImage: "wand.and.stars") }
                    .disabled(!rig.virtualOn || hub.learning)
            }
        }
        // On iPhone this page sits on the Controls tab's stack, whose Learn sheet serves it.
        if hsc == .regular { content.controllerLearnSheet() } else { content }
    }
}

/// One device with its name, where it's routed and the control last moved.
private struct DevicePanel: View {
    let device: VirtualDevices.Device
    @EnvironmentObject var virtual: VirtualDevices
    @EnvironmentObject var hub: ControllerHub

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Label(name, systemImage: device.icon).font(.headline).lineLimit(1)
                Spacer(minLength: 8)
                Text(status).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            switch device {
            case .launchpad: LaunchpadPanel()
            case .launchControlXL: LaunchControlXLPanel()
            case .gamepad: GamepadPanel()
            case .daisyPatch: DaisyPatchPanel()
            }
        }
        .padding()
        .background(RoundedRectangle(cornerRadius: 14).fill(Color.white.opacity(0.05)))
    }

    private var name: String {
        switch device {
        case .launchpad: return VirtualDeviceNames.launchpad
        case .launchControlXL: return VirtualDeviceNames.launchControlXL
        case .gamepad: return virtual.names.gamepad
        case .daisyPatch: return virtual.names.daisyPatch
        }
    }

    private var status: String {
        let pad = device == .gamepad
        guard let c = hub.controllers.first(where: { $0.name == name && ($0.kind == .gamepad) == pad }) else {
            return "Not listed yet"
        }
        let route = hub.route(of: c).title
        return hub.lastMoved[c.id].map { "\(route) · \($0)" } ?? route
    }
}

// MARK: - Pieces

/// A pad or button: down while touched (several at once with several fingers).
struct PressPad: View {
    var colour: Color
    var round = false
    var label: String?
    var action: (Bool) -> Void
    @State private var down = false

    var body: some View {
        let shape = round ? AnyShape(Circle()) : AnyShape(RoundedRectangle(cornerRadius: 5))
        ZStack {
            shape.fill(colour)
            shape.stroke(Color.white.opacity(down ? 0.9 : 0.15), lineWidth: down ? 2 : 1)
            if let label {
                Text(label).font(.caption.weight(.bold)).foregroundStyle(.white.opacity(0.85)).minimumScaleFactor(0.5)
            }
        }
        .contentShape(shape)
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { _ in if !down { down = true; action(true) } }
            .onEnded { _ in if down { down = false; action(false) } })
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel(label ?? "Pad")
    }
}

/// A knob, 0...127: drag up or right to turn it up. An LED in the middle if it has one.
struct KnobView: View {
    var value: Int
    var led: Color?
    var label: String?
    var onChange: (Int) -> Void
    @State private var start: Int?

    var body: some View {
        VStack(spacing: 2) {
            ZStack {
                Circle().fill(Color(white: 0.18))
                Circle().trim(from: 0, to: 0.75)
                    .stroke(Color.white.opacity(0.12), style: StrokeStyle(lineWidth: 3, lineCap: .round))
                    .rotationEffect(.degrees(135))
                Circle().trim(from: 0, to: 0.75 * Double(value) / 127)
                    .stroke(Color.orange, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                    .rotationEffect(.degrees(135))
                if let led { Circle().fill(led).frame(width: 9, height: 9) }
            }
            .padding(3)
            .aspectRatio(1, contentMode: .fit)
            .contentShape(Circle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { g in
                    if start == nil { start = value }
                    let v = (start ?? value) + Int((g.translation.width - g.translation.height) / 1.5)
                    onChange(min(127, max(0, v)))
                }
                .onEnded { _ in start = nil })
            .accessibilityElement()
            .accessibilityLabel(label ?? "Knob")
            .accessibilityValue("\(value)")
            .accessibilityAdjustableAction { dir in
                onChange(min(127, max(0, value + (dir == .increment ? 8 : -8))))
            }
            if let label { Text(label).font(.caption2).foregroundStyle(.secondary).lineLimit(1) }
        }
    }
}

/// A fader, 0...127, bottom to top.
struct FaderView: View {
    var value: Int
    var onChange: (Int) -> Void

    var body: some View {
        GeometryReader { geo in
            let h = geo.size.height, thumb: CGFloat = 16
            ZStack(alignment: .bottom) {
                Capsule().fill(Color(white: 0.2)).frame(width: 6).frame(maxHeight: .infinity)
                RoundedRectangle(cornerRadius: 3).fill(Color(white: 0.75))
                    .frame(width: min(geo.size.width, 34), height: thumb)
                    .offset(y: -(h - thumb) * CGFloat(value) / 127)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                let f = 1 - (g.location.y - thumb / 2) / max(1, h - thumb)
                onChange(min(127, max(0, Int((f * 127).rounded()))))
            })
        }
        .accessibilityElement()
        .accessibilityLabel("Fader")
        .accessibilityValue("\(value)")
        .accessibilityAdjustableAction { dir in onChange(min(127, max(0, value + (dir == .increment ? 8 : -8)))) }
    }
}

extension Color {
    /// A Launchpad LED: its palette colour, or an unlit pad.
    static func launchpad(_ index: UInt8) -> Color {
        guard index != 0 else { return Color(white: 0.16) }
        let c = LaunchpadPalette.components(index)
        return Color(red: c.r, green: c.g, blue: c.b)
    }

    /// A Launch Control XL LED (red and green, amber when both); nil when off.
    static func xlLED(_ v: UInt8) -> Color? {
        let c = VirtualLaunchControlXL.components(v)
        guard c.r > 0 || c.g > 0 else { return nil }
        return Color(red: c.r, green: c.g, blue: 0)
    }
}

// MARK: - The devices

private struct LaunchpadPanel: View {
    @EnvironmentObject var virtual: VirtualDevices

    var body: some View {
        let lp = virtual.launchpad
        VStack(alignment: .leading, spacing: 6) {
            Grid(horizontalSpacing: 5, verticalSpacing: 5) {
                ForEach(0..<VirtualLaunchpad.size, id: \.self) { row in
                    GridRow {
                        ForEach(0..<VirtualLaunchpad.size, id: \.self) { col in
                            cell(row, col, lp.colour(row: row, col: col))
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                                .aspectRatio(1, contentMode: .fit)
                        }
                    }
                }
            }
            .padding(8)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color(white: 0.07)))
            .aspectRatio(1, contentMode: .fit)
            Text(lp.programmerMode ? "Programmer mode: the app has it." : "Live mode: not taken by the app (routed Off, or not opened yet).")
                .font(.caption2).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder private func cell(_ row: Int, _ col: Int, _ colour: UInt8) -> some View {
        if row == 0 && col == 8 {
            // The logo: lit by the app, not a button.
            Circle().fill(Color.launchpad(colour)).padding(8)
        } else {
            PressPad(colour: Color.launchpad(colour), round: row == 0 || col == 8) { down in
                virtual.launchpad(row: row, col: col, down: down)
            }
        }
    }
}

private struct LaunchControlXLPanel: View {
    @EnvironmentObject var virtual: VirtualDevices

    var body: some View {
        let xl = virtual.xl
        VStack(spacing: 8) {
            ForEach(0..<3, id: \.self) { row in
                HStack(spacing: 6) {
                    ForEach(0..<8, id: \.self) { col in
                        KnobView(value: Int(xl.knobs[row * 8 + col]),
                                 led: Color.xlLED(xl.knobLED(row: row, col: col)) ?? Color(white: 0.3),
                                 label: row == 0 ? "\(col + 1)" : nil) { v in
                            virtual.xlKnob(row: row, col: col, to: v)
                        }
                    }
                }
            }
            HStack(spacing: 6) {
                ForEach(0..<8, id: \.self) { col in
                    FaderView(value: Int(xl.faders[col])) { v in virtual.xlFader(col: col, to: v) }
                }
            }
            .frame(height: 130)
            ForEach(0..<2, id: \.self) { row in
                HStack(spacing: 6) {
                    ForEach(0..<8, id: \.self) { col in
                        PressPad(colour: Color.xlLED(xl.buttonLED(row: row, col: col)) ?? Color(white: 0.2)) { down in
                            virtual.xlButton(row: row, col: col, down: down)
                        }
                        .frame(height: 30)
                    }
                }
            }
        }
    }
}

private struct GamepadPanel: View {
    @EnvironmentObject var virtual: VirtualDevices

    var body: some View {
        VStack(spacing: 14) {
            HStack(spacing: 8) {
                button(.lt, "LT").frame(width: 56, height: 34)
                button(.lb, "LB").frame(width: 56, height: 34)
                Spacer()
                button(.rb, "RB").frame(width: 56, height: 34)
                button(.rt, "RT").frame(width: 56, height: 34)
            }
            HStack(alignment: .center) {
                cross(up: (.up, "↑"), left: (.left, "←"), right: (.right, "→"), down: (.down, "↓"), round: false)
                Spacer(minLength: 8)
                VStack(spacing: 8) {
                    button(.back, "Back").frame(width: 56, height: 28)
                    button(.start, "Start").frame(width: 56, height: 28)
                }
                Spacer(minLength: 8)
                // By position, as GameController reads a real one: Y top, X left, B right, A bottom.
                cross(up: (.y, "Y"), left: (.x, "X"), right: (.b, "B"), down: (.a, "A"), round: true)
            }
        }
        .padding(.vertical, 6)
    }

    private func button(_ b: XInputButton, _ label: String, round: Bool = false) -> some View {
        PressPad(colour: virtual.pad.held.contains(b) ? .orange : Color(white: 0.22), round: round, label: label) { down in
            virtual.gamepad(b, down: down)
        }
    }

    private func cross(up: (XInputButton, String), left: (XInputButton, String), right: (XInputButton, String),
                       down: (XInputButton, String), round: Bool) -> some View {
        let s: CGFloat = 46
        return Grid(horizontalSpacing: 4, verticalSpacing: 4) {
            GridRow {
                Color.clear.frame(width: s, height: s)
                button(up.0, up.1, round: round).frame(width: s, height: s)
                Color.clear.frame(width: s, height: s)
            }
            GridRow {
                button(left.0, left.1, round: round).frame(width: s, height: s)
                Color.clear.frame(width: s, height: s)
                button(right.0, right.1, round: round).frame(width: s, height: s)
            }
            GridRow {
                Color.clear.frame(width: s, height: s)
                button(down.0, down.1, round: round).frame(width: s, height: s)
                Color.clear.frame(width: s, height: s)
            }
        }
    }
}

private struct DaisyPatchPanel: View {
    @EnvironmentObject var virtual: VirtualDevices

    var body: some View {
        let d = virtual.daisy
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                ForEach(0..<4, id: \.self) { i in
                    KnobView(value: Int(d.knobs[i]), label: "Ctrl \(i + 1) · CC \(VirtualDaisyPatch.knobCCs[i])") { v in
                        virtual.daisyKnob(i, to: v)
                    }
                }
            }
            PressPad(colour: d.gate ? .orange : Color(white: 0.22),
                     label: "Gate · note \(BeltTheory.name(Int(VirtualDaisyPatch.gateNote)))") { down in
                virtual.daisyGate(down)
            }
            .frame(height: 44)
            HStack {
                Text("MIDI in (last \(VirtualDaisyPatch.monitorLimit))").font(.subheadline.weight(.semibold))
                Spacer()
                Button("Clear") { virtual.clearMonitor() }.font(.caption).disabled(d.monitor.isEmpty)
            }
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        if d.monitor.isEmpty {
                            Text("Nothing yet. Route something to \(virtual.names.daisyPatch), or map a control to a CC or note on it.")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        ForEach(d.monitor) { line in
                            Text(line.text).font(.caption.monospaced()).id(line.id)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
                }
                .frame(height: 200)
                .background(RoundedRectangle(cornerRadius: 8).fill(Color.black.opacity(0.35)))
                .onChange(of: d.monitor.last?.id) { id in
                    if let id { proxy.scrollTo(id, anchor: .bottom) }
                }
            }
        }
    }
}
