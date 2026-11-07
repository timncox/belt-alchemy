// Every knob and setting by name, page by page, as the web Control page
// lays them out: six positions per page like the panel, dials for
// continuous values, segments for choices. The USB port setting stays on
// the module, because changing it here would cut the phone off.

import SwiftUI
import AlchemyLink

struct KnobsTab: View {
    @EnvironmentObject var store: ModuleStore

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    PagePicker()
                    if let pg = store.page {
                        LazyVGrid(columns: [GridItem(.flexible(), spacing: 22, alignment: .top), GridItem(.flexible(), spacing: 22, alignment: .top)], spacing: 6) {
                            if pg.grid {
                                ForEach(0..<6, id: \.self) { pot in cell(pg, pot: pot) }
                            } else {
                                ForEach(pg.fields) { f in KnobCell(field: f, position: nil) }
                            }
                        }
                    }
                }
                .padding()
            }
            .background(Theme.ground)
            .toolbar(.hidden, for: .navigationBar)
        }
    }

    @ViewBuilder
    private func cell(_ pg: ControlPage, pot: Int) -> some View {
        if let f = pg.at(pot: pot), !f.severs {
            KnobCell(field: f, position: pot)
        } else if let f = pg.at(pot: pot) {
            Placeholder(position: pot, text: "**\(f.name)**: set it on the module. It decides what the USB port does, so changing it here would cut the phone off.")
        } else if let g = pg.gestures.first(where: { $0.pot == pot }) {
            Placeholder(position: pot, text: "**\(g.name)**: used on the module itself; the Presets tab does the same.")
        } else {
            Placeholder(position: pot, text: "nothing here")
        }
    }
}

struct Placeholder: View {
    var position: Int
    var text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("P\(position + 1)").font(.caption).foregroundStyle(Theme.fog)
            Text(.init(text)).font(.footnote).foregroundStyle(Theme.fog)
        }
        .frame(maxWidth: .infinity, minHeight: 120, alignment: .topLeading)
        .padding(.vertical, 14)
        .overlay(alignment: .top) { Rectangle().fill(Theme.line).frame(height: 1) }
    }
}

struct KnobCell: View {
    @EnvironmentObject var store: ModuleStore
    var field: Field
    var position: Int?

    var body: some View {
        let v = store.value(field)
        let look = KnobLook.of(field, pageColor: store.page?.color)
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                Text(field.name).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.mist).lineLimit(2)
                if let p = position { Text("P\(p + 1)").font(.caption).foregroundStyle(Theme.fog) }
                Spacer()
                if field.isEnum || field.disp.kind == "snap" || field.disp.kind == "enum" {
                    Text(field.show(v)).font(Theme.display(17)).foregroundStyle(look.color)
                        .lineLimit(1).minimumScaleFactor(0.6)
                }
            }
            if field.isEnum {
                Choices(labels: field.disp.labels ?? (0..<max(1, field.zones)).map(String.init), selected: Int(v),
                        colour: look.color) {
                    store.set(field, Double($0))
                }
            } else if field.disp.kind == "snap" || field.disp.kind == "enum", let labels = field.disp.labels, !labels.isEmpty {
                // A float that snaps to named positions: show the names.
                let n = labels.count
                let sel = field.disp.kind == "snap" ? Int((v * Double(max(1, n - 1))).rounded()) : min(n - 1, Int(v * Double(n)))
                Choices(labels: labels, selected: sel, colour: look.color) { i in
                    store.set(field, field.disp.kind == "snap" ? Double(i) / Double(max(1, n - 1)) : (Double(i) + 0.5) / Double(n))
                }
            } else {
                Dial(value: v, look: look, text: field.show(v)) { store.set(field, $0) }
                    .frame(height: 132)
                    .frame(maxWidth: .infinity)
            }
            if let help = field.help, !help.isEmpty {
                DisclosureGroup("What it does") { Text(.init(help)).font(.footnote).foregroundStyle(Theme.silk) }
                    .font(.footnote).tint(Theme.silkDim)
            }
        }
        .padding(.vertical, 14)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .overlay(alignment: .top) { Rectangle().fill(Theme.line).frame(height: 1) }
    }
}

struct Choices: View {
    var labels: [String]
    var selected: Int
    var colour: Color = Theme.key
    var pick: (Int) -> Void

    var body: some View {
        FlowRow(spacing: 6) {
            ForEach(Array(labels.enumerated()), id: \.offset) { item in
                Button(item.element) { pick(item.offset) }
                    .font(.caption.weight(item.offset == selected ? .bold : .regular))
                    .foregroundStyle(item.offset == selected ? Theme.plate : Theme.silk)
                    .padding(.horizontal, 10).padding(.vertical, 6)
                    .background(item.offset == selected ? colour : Theme.silk.opacity(0.07), in: Capsule())
                    .buttonStyle(.plain)
            }
        }
    }
}

/// A knob: its LED ring around a cap with a pointer, dragged up and down
/// like a pot.
struct Dial: View {
    var value: Double
    var look = KnobLook(color: Theme.key)
    var text: String? = nil
    var set: (Double) -> Void
    @State private var start: Double?

    var body: some View {
        GeometryReader { geo in
            let d = min(geo.size.width, geo.size.height)
            ZStack {
                ValueArc(value: value, look: look, width: max(4, d * 0.045))
                if let text {
                    Text(text).font(Theme.display(d * 0.19)).foregroundStyle(Theme.mist)
                        .lineLimit(1).minimumScaleFactor(0.5)
                        .frame(width: d * 0.6)
                }
            }
            .frame(width: d, height: d)
            .position(x: geo.size.width / 2, y: geo.size.height / 2)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 1)
                .onChanged { g in
                    if start == nil { start = value }
                    set(min(1, max(0, start! - Double(g.translation.height) / 200 + Double(g.translation.width) / 400)))
                }
                .onEnded { _ in start = nil })
            .accessibilityElement()
            .accessibilityValue("\(Int(value * 100)) percent")
            .accessibilityAdjustableAction { dir in
                set(min(1, max(0, value + (dir == .increment ? 0.05 : -0.05))))
            }
        }
    }
}

struct Arc: Shape {
    var from: Double, to: Double

    func path(in rect: CGRect) -> Path {
        var p = Path()
        let c = CGPoint(x: rect.midX, y: rect.midY)
        let r = min(rect.width, rect.height) / 2 - 4
        // 0 sits at 7:30, 1 at 4:30, clockwise, like the panel's rings.
        p.addArc(center: c, radius: r, startAngle: .degrees(135), endAngle: .degrees(135 + 270 * max(from, min(1, to))), clockwise: false)
        return p
    }
}

/// Wraps its children onto as many lines as they need.
struct FlowRow: Layout {
    var spacing: CGFloat = 6

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? 300
        var x: CGFloat = 0, y: CGFloat = 0, line: CGFloat = 0
        for v in subviews {
            let s = v.sizeThatFits(.unspecified)
            if x > 0 && x + s.width > width { x = 0; y += line + spacing; line = 0 }
            x += s.width + spacing
            line = max(line, s.height)
        }
        return CGSize(width: width, height: y + line)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = bounds.minX, y = bounds.minY, line: CGFloat = 0
        for v in subviews {
            let s = v.sizeThatFits(.unspecified)
            if x > bounds.minX && x + s.width > bounds.maxX { x = bounds.minX; y += line + spacing; line = 0 }
            v.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(s))
            x += s.width + spacing
            line = max(line, s.height)
        }
    }
}
