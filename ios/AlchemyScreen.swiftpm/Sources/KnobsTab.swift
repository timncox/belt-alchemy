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
                VStack(alignment: .leading, spacing: 14) {
                    PagePicker()
                    if let pg = store.page {
                        LazyVGrid(columns: [GridItem(.flexible(), spacing: 12), GridItem(.flexible(), spacing: 12)], spacing: 12) {
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
            .navigationTitle(store.page?.name ?? "Knobs")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ModuleToolbar() }
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
            Text("P\(position + 1)").font(.caption.monospaced()).foregroundStyle(.secondary)
            Text(.init(text)).font(.footnote).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, minHeight: 130, alignment: .topLeading)
        .padding(12)
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(style: StrokeStyle(lineWidth: 1, dash: [4, 4])).foregroundStyle(.secondary.opacity(0.4)))
    }
}

struct KnobCell: View {
    @EnvironmentObject var store: ModuleStore
    var field: Field
    var position: Int?

    var body: some View {
        let v = store.value(field)
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                if let p = position { Text("P\(p + 1)").font(.caption.monospaced()).foregroundStyle(.secondary) }
                Spacer()
                Text(field.show(v)).font(.callout.monospacedDigit()).foregroundStyle(.orange)
            }
            Text(field.name).font(.headline).lineLimit(2)
            if field.isEnum {
                Choices(labels: field.disp.labels ?? (0..<max(1, field.zones)).map(String.init), selected: Int(v)) {
                    store.set(field, Double($0))
                }
            } else if field.disp.kind == "snap" || field.disp.kind == "enum", let labels = field.disp.labels, !labels.isEmpty {
                // A float that snaps to named positions: show the names.
                let n = labels.count
                let sel = field.disp.kind == "snap" ? Int((v * Double(max(1, n - 1))).rounded()) : min(n - 1, Int(v * Double(n)))
                Choices(labels: labels, selected: sel) { i in
                    store.set(field, field.disp.kind == "snap" ? Double(i) / Double(max(1, n - 1)) : (Double(i) + 0.5) / Double(n))
                }
            } else {
                Dial(value: v) { store.set(field, $0) }
                    .frame(height: 84)
                    .frame(maxWidth: .infinity)
            }
            if let help = field.help, !help.isEmpty {
                DisclosureGroup("What it does") { Text(.init(help)).font(.footnote) }.font(.footnote)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, minHeight: 130, alignment: .topLeading)
        .background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14))
    }
}

struct Choices: View {
    var labels: [String]
    var selected: Int
    var pick: (Int) -> Void

    var body: some View {
        FlowRow(spacing: 6) {
            ForEach(Array(labels.enumerated()), id: \.offset) { item in
                Button(item.element) { pick(item.offset) }
                    .font(.caption)
                    .padding(.horizontal, 10).padding(.vertical, 6)
                    .background(item.offset == selected ? Color.orange.opacity(0.35) : Color.white.opacity(0.07), in: Capsule())
                    .buttonStyle(.plain)
            }
        }
    }
}

/// A knob: a 270° arc you drag up and down, like a pot.
struct Dial: View {
    var value: Double
    var set: (Double) -> Void
    @State private var start: Double?

    var body: some View {
        GeometryReader { geo in
            let d = min(geo.size.width, geo.size.height)
            ZStack {
                Arc(from: 0, to: 1).stroke(Color.white.opacity(0.12), style: StrokeStyle(lineWidth: 7, lineCap: .round))
                Arc(from: 0, to: value).stroke(Color.orange, style: StrokeStyle(lineWidth: 7, lineCap: .round))
                Circle().fill(Color.white.opacity(0.08)).padding(14)
                Rectangle().fill(Color.white).frame(width: 2, height: d / 2 - 18)
                    .offset(y: -(d / 4 - 9))
                    .rotationEffect(.degrees(-135 + 270 * value))
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
