// The module's screen. For Belt (and any firmware that answers STATE) the
// app draws its own, in BeltScreen.swift; for Pulse 0.4.0 and later, its
// own in PulseScreen.swift (PANEL there shows this one). Otherwise: its six
// LED rings and button lights, each ring named and valued, drawn from
// the frame the panel last showed (SCREEN, 0x71), so pages, catch pips and
// the picker's progress look on the phone as they do on the module. Drag
// up or down on a ring to turn that knob on the page picked above; tap a
// button to press it, when the firmware lets the phone press buttons.

import SwiftUI
import AlchemyLink

struct ScreenTab: View {
    @EnvironmentObject var store: ModuleStore
    @State private var touched: Field?
    @State private var showPanel = false
    @State fileprivate var holdToken = 0

    var body: some View {
        if let st = store.state, let belt = st.belt {
            BeltScreen(st: st, belt: belt)
        } else if Pulse.drives(store.desc), !showPanel {
            PulseScreen(showPanel: $showPanel)
        } else {
            ledScreen
        }
    }

    /// For firmwares without STATE: the module's own LEDs, mirrored.
    private var ledScreen: some View {
        NavigationStack {
            VStack(spacing: 12) {
                PagePicker(onlyGrid: true)
                ZStack(alignment: .top) {
                    PanelView(touched: $touched, onHold: { button, down in holdChanged(button, down) })
                        .padding(.bottom, 40)   // the bottom rings' names sit below the panel
                    if let f = touched {
                        HStack(spacing: 8) {
                            Text(f.name).font(.headline)
                            Text(f.show(store.value(f))).font(.headline.monospacedDigit()).foregroundStyle(Theme.key)
                        }
                        .padding(.horizontal, 14).padding(.vertical, 8)
                        .background(.ultraThinMaterial, in: Capsule())
                        .padding(.top, 8)
                        .transition(.opacity)
                    }
                }
                Text(store.phone?.hasScreen == true
                     ? "Drag a ring up or down to turn its knob."
                     : "This firmware doesn't send its LEDs; the knobs still work.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .padding()
            .navigationTitle(store.desc?.moduleName ?? "Alchemy Lab")
            .navigationBarTitleDisplayMode(.inline)
            .onAppear { followModule() }
            .onChange(of: store.values[Pulse.selectedId]) { _ in followModule() }
            .toolbar {
                if Pulse.drives(store.desc) {
                    ToolbarItem(placement: .navigationBarTrailing) {
                        Button("Pulse") { showPanel = false }.accessibilityIdentifier("pulse-back")
                    }
                }
            }
        }
    }
}

extension ScreenTab {
    /// Pulse's panel shows the selected output's OUTPUT layer, its PATTERN
    /// layer while B3 is held and the clock while B2 is held. The page
    /// picked here names the rings and takes their drags, so it follows:
    /// otherwise a ring lit with one setting is labelled as another.
    /// Other firmwares keep the page you pick.
    fileprivate func followModule(layer: Int = 0) {
        guard Pulse.drives(store.desc) else { return }
        let n = Int(store.values[Pulse.selectedId] ?? 0) + 1
        let name = layer == 2 ? "Clock" : layer == 1 ? "Pat \(n)" : "Out \(n)"
        if let i = store.pages.firstIndex(where: { $0.name == name }), store.pageIndex != i { store.pageIndex = i }
    }

    /// A button held on the phone's panel: after the module's own
    /// tap-or-hold time (300 ms), the page goes to the layer it brings up.
    fileprivate func holdChanged(_ button: Int, _ down: Bool) {
        guard Pulse.drives(store.desc), button == 1 || button == 2 else { return }
        holdToken += 1
        guard down else { followModule(); return }
        let token = holdToken
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            if holdToken == token { followModule(layer: button == 1 ? 2 : 1) }
        }
    }
}

struct PanelView: View {
    @EnvironmentObject var store: ModuleStore
    @Binding var touched: Field?
    /// A panel button going down or coming up under a finger (button 0…2).
    var onHold: ((Int, Bool) -> Void)? = nil
    @State private var dragStart: Double?

    private let b = PanelLayout.bounds

    var body: some View {
        GeometryReader { geo in
            let w = b.maxX - b.minX, h = b.maxY - b.minY
            let s = min(geo.size.width / w, geo.size.height / h)
            let ox = (geo.size.width - w * s) / 2, oy = (geo.size.height - h * s) / 2
            let at = { (x: Double, y: Double) in CGPoint(x: ox + (x - b.minX) * s, y: oy + (y - b.minY) * s) }
            let rgb = store.screen

            ZStack {
                Canvas { ctx, _ in
                    let panel = CGRect(x: ox, y: oy, width: w * s, height: h * s)
                    ctx.fill(Path(roundedRect: panel, cornerRadius: 3 * s), with: .color(Color(white: 0.09)))
                    for c in PanelLayout.controls {
                        let p = at(c.x, c.y)
                        let r = c.r * s
                        ctx.fill(Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r)),
                                 with: .color(Color(white: c.pot != nil ? 0.18 : 0.24)))
                    }
                    for led in PanelLayout.leds {
                        let i = led.index * 3
                        let on = rgb.count >= i + 3
                        let col = on ? ledColor(rgb[i], rgb[i + 1], rgb[i + 2]) : Color(white: 0.12)
                        let p = at(led.x, led.y)
                        let r = 1.1 * s
                        if on, max(rgb[i], rgb[i + 1], rgb[i + 2]) > 24 {
                            var glow = ctx
                            glow.addFilter(.blur(radius: 1.6 * s))
                            glow.fill(Path(ellipseIn: CGRect(x: p.x - 2 * r, y: p.y - 2 * r, width: 4 * r, height: 4 * r)),
                                      with: .color(col.opacity(0.55)))
                        }
                        ctx.fill(Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r)), with: .color(col))
                    }
                }

                ForEach(PanelLayout.controls.filter { $0.pot != nil }) { c in
                    Circle().fill(Color.white.opacity(0.001))
                        .frame(width: 2 * (PanelLayout.ringRadius + 2) * s, height: 2 * (PanelLayout.ringRadius + 2) * s)
                        .position(at(c.x, c.y))
                        .gesture(potDrag(c.pot ?? 0))
                        .accessibilityLabel(label(c))
                }
                // Each ring's knob on the page shown: its name and value, so
                // the panel reads without knowing the firmware by heart.
                ForEach(PanelLayout.controls.filter { $0.pot != nil }) { c in
                    if let f = store.field(atPot: c.pot ?? 0) {
                        VStack(spacing: 0) {
                            Text(f.name).foregroundStyle(Theme.fog)
                            Text(f.show(store.value(f))).foregroundStyle(Theme.mist).fontWeight(.semibold)
                        }
                        .font(.system(size: max(9, 2.5 * s))).lineLimit(1).minimumScaleFactor(0.6)
                        .frame(width: 30 * s)
                        .position(at(c.x, c.y + PanelLayout.ringRadius + 5.2))
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                    }
                }
                if store.phone?.hasPress == true {
                    ForEach(PanelLayout.controls.filter { $0.button != nil }) { c in
                        Circle().fill(Color.white.opacity(0.001))
                            .frame(width: 2 * (PanelLayout.buttonRadius + 2) * s, height: 2 * (PanelLayout.buttonRadius + 2) * s)
                            .position(at(c.x, c.y))
                            .gesture(buttonPress(c.button ?? 0))
                            .accessibilityLabel(label(c))
                    }
                }
            }
        }
        .aspectRatio((b.maxX - b.minX) / (b.maxY - b.minY), contentMode: .fit)
    }

    /// The LEDs are dim on the module's own scale; lift them so a phone
    /// screen in daylight shows what the panel shows in a dark studio.
    private func ledColor(_ r: UInt8, _ g: UInt8, _ b: UInt8) -> Color {
        let m = Double(max(r, g, b, 1))
        let lift = min(255 / m, max(1, pow(255 / m, 0.6)))
        return Color(red: min(1, Double(r) * lift / 255), green: min(1, Double(g) * lift / 255), blue: min(1, Double(b) * lift / 255))
    }

    private func label(_ c: PanelControl) -> String {
        if let p = c.pot { return store.field(atPot: p)?.name ?? "Pot \(p + 1)" }
        return "Button B\((c.button ?? 0) + 1)"
    }

    private func potDrag(_ pot: Int) -> some Gesture {
        DragGesture(minimumDistance: 2)
            .onChanged { g in
                guard let f = store.field(atPot: pot), !f.severs else { return }
                if dragStart == nil {
                    dragStart = f.isEnum ? store.value(f) / Double(max(1, f.zones - 1)) : store.value(f)
                    withAnimation(.easeOut(duration: 0.15)) { touched = f }
                }
                let v = min(1, max(0, dragStart! - Double(g.translation.height) / 220))
                store.set(f, f.isEnum ? v * Double(max(1, f.zones - 1)) : v)
            }
            .onEnded { _ in
                dragStart = nil
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
                    if dragStart == nil { withAnimation { touched = nil } }
                }
            }
    }

    private func buttonPress(_ button: Int) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { _ in if dragStart == nil { dragStart = -1; store.press(button: button, .down); onHold?(button, true) } }
            .onEnded { _ in dragStart = nil; store.press(button: button, .up); onHold?(button, false) }
    }
}

/// Tabs for the descriptor's pages, tinted with its page colours.
struct PagePicker: View {
    @EnvironmentObject var store: ModuleStore
    var onlyGrid = false

    var body: some View {
        let all = Array(store.pages.enumerated()).filter { !onlyGrid || ($0.element.grid && !$0.element.isSettings) }
        if all.count > 1 {
          ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 20) {
                    ForEach(all, id: \.element.id) { item in
                        let i = item.offset, pg = item.element
                        let on = store.pageIndex == i
                        let c = Color(hex: pg.color) ?? Theme.fog
                        Button {
                            store.pageIndex = i
                        } label: {
                            HStack(spacing: 7) {
                                Circle().fill(on ? c : c.opacity(0.35)).frame(width: 7, height: 7)
                                    .shadow(color: on ? c : .clear, radius: 4)
                                Text(pg.name).font(.subheadline.weight(on ? .semibold : .regular))
                                    .foregroundStyle(on ? Theme.mist : Theme.fog)
                            }
                            .padding(.vertical, 6)
                        }
                        .buttonStyle(.plain)
                        .accessibilityAddTraits(on ? .isSelected : [])
                        .id(i)
                    }
                }
            }
            .onAppear {
                if onlyGrid, !all.contains(where: { $0.offset == store.pageIndex }), let first = all.first {
                    store.pageIndex = first.offset
                }
                proxy.scrollTo(store.pageIndex, anchor: .center)
            }
            // The page can change without a tap (it follows the module): keep its tab in view.
            .onChange(of: store.pageIndex) { i in withAnimation { proxy.scrollTo(i, anchor: .center) } }
          }
        }
    }
}
