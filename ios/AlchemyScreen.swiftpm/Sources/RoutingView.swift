// MIDI routing: everything on the hub talks over USB through this iPad or
// iPhone. A list of routes (source, destination, what passes, on or off),
// an Add route sheet, and on iPad a matrix (rows sources, columns
// destinations, a dot where a route is). Its own tab on iPad; on iPhone a
// page under Controls. The routes themselves live in ControllerHub, kept by
// device name (modules by unit number).

import SwiftUI
import AlchemyLink

/// A route to edit: an existing one, or a new one perhaps with its ends picked.
struct RouteDraft: Identifiable {
    let id = UUID()
    var route: MIDIRoute?
    var from: RouteEndpoint?
    var to: RouteEndpoint?
    init(route: MIDIRoute) { self.route = route }
    init(from: RouteEndpoint? = nil, to: RouteEndpoint? = nil) { self.from = from; self.to = to }
}

struct RoutingScreen: View {
    @EnvironmentObject var hub: ControllerHub
    @EnvironmentObject var rig: Rig
    @Environment(\.horizontalSizeClass) private var hsc
    @AppStorage("routingMatrix") private var showMatrix = false
    @State private var editing: RouteDraft?

    var body: some View {
        VStack(spacing: 0) {
            if hsc == .regular {
                Picker("View", selection: $showMatrix) {
                    Text("List").tag(false)
                    Text("Matrix").tag(true)
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 320)
                .padding(.horizontal).padding(.vertical, 6)
            }
            if hsc == .regular && showMatrix {
                RoutingMatrix { editing = $0 }
            } else {
                list
            }
        }
        .navigationTitle("Routing")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button { editing = RouteDraft() } label: { Label("Add route", systemImage: "plus") }
                    .disabled(hub.routeSources.isEmpty)
            }
        }
        .sheet(item: $editing) { d in
            NavigationStack { MIDIRouteEditor(draft: d) }
                .environmentObject(hub)
                .environmentObject(rig)
        }
    }

    private var list: some View {
        List {
            Section {
                if hub.midiRoutes.isEmpty {
                    Text("No routes yet. Tap + to send one thing to another: a keyboard to the Daisy Patch, Left's sung note to the Daisy, the Launchpad to Right as a keyboard.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                ForEach(hub.midiRoutes) { r in
                    MIDIRouteRow(route: r) { editing = RouteDraft(route: r) }
                }
                .onDelete { idx in
                    let ids = idx.map { hub.midiRoutes[$0].id }
                    for id in ids { hub.removeMIDIRoute(id) }
                }
                Button { editing = RouteDraft() } label: { Label("Add route", systemImage: "plus") }
                    .disabled(hub.routeSources.isEmpty)
            } header: {
                Text("Routes")
            } footer: {
                Text("Everything goes through this device over USB. Notes still held when a route is removed, switched off or a device is unplugged are let go. Swipe to remove.")
            }
            Section {
                ForEach(endpoints, id: \.self) { e in
                    HStack(spacing: 12) {
                        Image(systemName: hub.endpointIcon(e)).frame(width: 28).foregroundColor(Theme.key)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(hub.endpointName(e)).lineLimit(1)
                            Text(role(e)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                        }
                        Spacer()
                        if !hub.isHere(e) { Text("Not connected").font(.caption).foregroundStyle(.secondary) }
                    }
                }
            } header: {
                Text("On the hub")
            } footer: {
                Text("Modules play routed notes as a keyboard (with Phone firmware that supports it), and send their own MIDI: Belt's sung note, its harmonies and its knobs. A gamepad's buttons send notes.")
            }
        }
    }

    private var endpoints: [RouteEndpoint] {
        var out = hub.routeSources
        for d in hub.routeDestinations where !out.contains(d) { out.append(d) }
        return out
    }

    private func role(_ e: RouteEndpoint) -> String {
        let sends = hub.midiRoutes.filter { $0.from == e }.count
        let gets = hub.midiRoutes.filter { $0.to == e }.count
        var parts: [String] = []
        switch e {
        case .module: parts.append("Module")
        case .gamepad: parts.append("Gamepad, sends only")
        case .device: parts.append("MIDI device")
        }
        if sends > 0 { parts.append("sends to \(sends)") }
        if gets > 0 { parts.append("gets from \(gets)") }
        return parts.joined(separator: " · ")
    }
}

private struct MIDIRouteRow: View {
    @EnvironmentObject var hub: ControllerHub
    let route: MIDIRoute
    let edit: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Button(action: edit) {
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Image(systemName: hub.endpointIcon(route.from)).foregroundColor(Theme.key)
                        Text(hub.endpointName(route.from)).lineLimit(1)
                        Image(systemName: "arrow.right").font(.caption).foregroundStyle(.secondary)
                        Image(systemName: hub.endpointIcon(route.to)).foregroundColor(Theme.key)
                        Text(hub.endpointName(route.to)).lineLimit(1)
                    }
                    Text(route.summary).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                    if let note = hub.routeNote(route) {
                        Label(note.text, systemImage: note.warn ? "exclamationmark.triangle" : "info.circle")
                            .font(.caption)
                            .foregroundColor(note.warn ? Theme.key : .secondary)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            Toggle("On", isOn: Binding(get: { route.enabled }, set: { hub.setMIDIRoute(route.id, enabled: $0) }))
                .labelsHidden()
        }
        .opacity(route.enabled ? 1 : 0.6)
    }
}

/// Rows are sources, columns destinations; a filled dot is a route that's
/// on, a ring one that's off. Tap an empty square to add, a dot to change.
private struct RoutingMatrix: View {
    @EnvironmentObject var hub: ControllerHub
    let edit: (RouteDraft) -> Void
    private let cell: CGFloat = 76, rowHeight: CGFloat = 48, labelWidth: CGFloat = 170

    var body: some View {
        let sources = hub.routeSources, dests = hub.routeDestinations
        GeometryReader { geo in
        ScrollView([.horizontal, .vertical]) {
            VStack(alignment: .leading, spacing: 12) {
                if sources.isEmpty || dests.isEmpty {
                    Text("Plug in a module or a MIDI device to route.").foregroundStyle(.secondary)
                } else {
                    Grid(alignment: .center, horizontalSpacing: 4, verticalSpacing: 4) {
                        GridRow {
                            Text("From ↓   To →")
                                .font(.caption).foregroundStyle(.secondary)
                                .frame(width: labelWidth, alignment: .leading)
                            ForEach(dests, id: \.self) { d in
                                VStack(spacing: 2) {
                                    Image(systemName: hub.endpointIcon(d)).foregroundColor(Theme.key)
                                    Text(hub.endpointName(d)).font(.caption2).lineLimit(2).multilineTextAlignment(.center)
                                }
                                .frame(width: cell, height: rowHeight + 8)
                                .opacity(hub.isHere(d) ? 1 : 0.5)
                            }
                        }
                        ForEach(sources, id: \.self) { s in
                            GridRow {
                                HStack(spacing: 8) {
                                    Image(systemName: hub.endpointIcon(s)).foregroundColor(Theme.key).frame(width: 24)
                                    Text(hub.endpointName(s)).font(.subheadline).lineLimit(1)
                                }
                                .frame(width: labelWidth, height: rowHeight, alignment: .leading)
                                .opacity(hub.isHere(s) ? 1 : 0.5)
                                ForEach(dests, id: \.self) { d in square(s, d) }
                            }
                        }
                    }
                    Text("A filled dot is a route that's on, a ring one that's off; a number means several. Tap an empty square to add a route, a dot to change it.")
                        .font(.footnote).foregroundStyle(.secondary)
                        .frame(maxWidth: labelWidth + CGFloat(dests.count) * (cell + 4), alignment: .leading)
                }
            }
            .padding()
            // Pinned to the top left; a two-way scroll view centres what's smaller than it.
            .frame(minWidth: geo.size.width, minHeight: geo.size.height, alignment: .topLeading)
        }
        }
    }

    private func square(_ s: RouteEndpoint, _ d: RouteEndpoint) -> some View {
        let rs = hub.midiRoutes.filter { $0.from == s && $0.to == d }
        let usable = s != d
        return Button {
            if let r = rs.first { edit(RouteDraft(route: r)) } else { edit(RouteDraft(from: s, to: d)) }
        } label: {
            ZStack {
                RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(usable ? 0.06 : 0.02))
                if let r = rs.first {
                    Circle()
                        .fill(rs.contains { $0.enabled } ? Theme.key : Color.clear)
                        .overlay(Circle().stroke(Theme.key, lineWidth: 2))
                        .frame(width: 18, height: 18)
                        .accessibilityLabel(r.enabled ? "Route on" : "Route off")
                    if rs.count > 1 {
                        Text("\(rs.count)").font(.caption2.weight(.bold)).offset(x: 16, y: -12)
                    }
                }
            }
            .frame(width: cell, height: rowHeight)
        }
        .buttonStyle(.plain)
        .disabled(!usable)
    }
}

/// Add or change one route.
struct MIDIRouteEditor: View {
    @EnvironmentObject var hub: ControllerHub
    @Environment(\.dismiss) private var dismiss
    let draft: RouteDraft
    @State private var r = MIDIRoute(from: .device(""), to: .device(""))
    @State private var loaded = false
    @State private var refusal: String?

    private var isGamepad: Bool { if case .gamepad = r.from { return true }; return false }
    /// What this pair of ends can carry at all.
    private var possible: RouteFilter { var p = r; p.filter = .all; return p.effectiveFilter }

    var body: some View {
        Form {
            Section {
                Picker("From", selection: $r.from) {
                    ForEach(hub.routeSources, id: \.self) { e in
                        Label(hub.endpointName(e), systemImage: hub.endpointIcon(e)).tag(e)
                    }
                }
                Picker("To", selection: $r.to) {
                    ForEach(hub.routeDestinations.filter { $0 != r.from }, id: \.self) { e in
                        Label(hub.endpointName(e), systemImage: hub.endpointIcon(e)).tag(e)
                    }
                }
                Toggle("On", isOn: $r.enabled)
            } footer: {
                if let note = hub.routeNote(r) { Text(note.text) }
            }

            if r.from.isModule {
                moduleSource
            } else if isGamepad {
                Section {
                    Stepper("First button: note \(BeltTheory.name(Int(r.baseNote))) (\(r.baseNote))",
                            value: Binding(get: { Int(r.baseNote) }, set: { r.baseNote = UInt8(max(0, min(127, $0))) }),
                            in: 0...110)
                } header: {
                    Text("Buttons as notes")
                } footer: {
                    Text("Each button plays the next note up, in the order up, down, left, right, start, back, L3, R3, LB, RB, home, A, B, X, Y, LT, RT.")
                }
            } else {
                Section {
                    ForEach(RouteFilter.kinds.indices.filter { possible.contains(RouteFilter.kinds[$0].filter) }, id: \.self) { i in
                        Toggle(RouteFilter.kinds[i].name, isOn: filterBinding(RouteFilter.kinds[i].filter))
                    }
                } header: {
                    Text("What passes")
                } footer: {
                    if r.to.isModule {
                        Text("A module plays notes as a keyboard and takes CC 64 as its sustain pedal; other CCs reach it only through controls learned on the Controls page.")
                    }
                }
            }

            channelSection

            if possible.contains(.notes) && r.filter.contains(.notes) {
                Section {
                    Stepper(r.transpose == 0 ? "Transpose: none" : "Transpose: \(r.transpose > 0 ? "+" : "")\(r.transpose) semitones",
                            value: $r.transpose, in: MIDIRoute.transposeRange)
                } footer: {
                    Text("A note pushed below 0 or above 127 isn't sent.")
                }
            }

            if !r.from.isModule {
                Section {
                    Toggle("Include mapped controls", isOn: $r.includeMapped)
                } footer: {
                    Text("Controls already sent to a module (Left, Right or Both on the Controls page) or mapped in the app aren't routed as well, so nothing plays twice. Switch this on to route them anyway.")
                }
            }

            if draft.route != nil {
                Section {
                    Button("Remove this route", role: .destructive) {
                        hub.removeMIDIRoute(r.id)
                        dismiss()
                    }
                }
            }
        }
        .navigationTitle(draft.route == nil ? "Add route" : "Route")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    if let why = hub.saveMIDIRoute(r) { refusal = why } else { dismiss() }
                }
                .disabled(!canSave)
            }
        }
        .alert("Can't add that route", isPresented: Binding(get: { refusal != nil }, set: { if !$0 { refusal = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(refusal ?? "")
        }
        .onAppear(perform: load)
        .onChange(of: r.from) { _ in
            // A destination that's now the source itself: pick another.
            if r.to == r.from, let d = hub.routeDestinations.first(where: { $0 != r.from }) { r.to = d }
        }
    }

    private var canSave: Bool {
        guard r.from != r.to, r.to.canReceive else { return false }
        if case let .device(n) = r.from, n.isEmpty { return false }
        if case let .device(n) = r.to, n.isEmpty { return false }
        return true
    }

    private func load() {
        guard !loaded else { return }
        loaded = true
        if let route = draft.route { r = route; return }
        let from = draft.from ?? hub.routeSources.first ?? .device("")
        let to = draft.to ?? hub.routeDestinations.first { $0 != from } ?? .device("")
        var n = MIDIRoute(from: from, to: to)
        if from.isModule { n.filter = [.notes, .cc] }
        r = n
    }

    @ViewBuilder private var moduleSource: some View {
        Section {
            Toggle("Sung note", isOn: filterBinding(.notes))
            Toggle("Harmony voices too", isOn: $r.harmonies)
                .disabled(!r.filter.contains(.notes))
        } header: {
            Text("Notes")
        } footer: {
            Text("The note Belt pulls the voice to: on when a note starts, off when it ends or changes, velocity from how loud the input is. Read from the module's screen updates, so it follows a few tens of milliseconds behind.")
        }
        if let unit = r.from.unit, !r.to.isModule {
            Section {
                Toggle("Send knobs as CCs", isOn: filterBinding(.cc))
                if r.filter.contains(.cc) {
                    ForEach(hub.knobFields(unit)) { f in
                        HStack {
                            Toggle(f.name, isOn: knobBinding(f))
                            if r.knobCCs.contains(where: { $0.field == f.id }) {
                                Stepper("CC \(ccBinding(f).wrappedValue)", value: ccBinding(f), in: 0...127)
                                    .fixedSize()
                            }
                        }
                    }
                }
            } header: {
                Text("Knobs")
            } footer: {
                Text("A knob's CC goes out when it moves (by hand or from the app), at most 60 times a second.")
            }
        }
    }

    @ViewBuilder private var channelSection: some View {
        let fromDevice: Bool = { if case .device = r.from { return true }; return false }()
        if fromDevice || !r.to.isModule {
            Section {
                if fromDevice {
                    Picker("In channel", selection: channelBinding(\.inChannel)) {
                        Text("Any").tag(-1)
                        ForEach(0..<16, id: \.self) { Text("\($0 + 1)").tag($0) }
                    }
                }
                if !r.to.isModule {
                    Picker("Out channel", selection: channelBinding(\.outChannel)) {
                        Text(fromDevice ? "Same as in" : "1").tag(-1)
                        ForEach(0..<16, id: \.self) { Text("\($0 + 1)").tag($0) }
                    }
                }
            } header: {
                Text("Channels")
            }
        }
    }

    private func filterBinding(_ k: RouteFilter) -> Binding<Bool> {
        Binding(get: { r.filter.contains(k) },
                set: { on in if on { r.filter.insert(k) } else { r.filter.remove(k) } })
    }

    private func channelBinding(_ path: WritableKeyPath<MIDIRoute, UInt8?>) -> Binding<Int> {
        Binding(get: { r[keyPath: path].map(Int.init) ?? -1 },
                set: { v in r[keyPath: path] = v < 0 ? nil : UInt8(min(15, v)) })
    }

    private func knobBinding(_ f: Field) -> Binding<Bool> {
        Binding(get: { r.knobCCs.contains { $0.field == f.id } },
                set: { on in
                    if on {
                        guard !r.knobCCs.contains(where: { $0.field == f.id }) else { return }
                        let used = Set(r.knobCCs.map(\.cc))
                        let cc = (20...119).map(UInt8.init).first { !used.contains($0) } ?? 20
                        r.knobCCs.append(KnobCC(field: f.id, cc: cc))
                    } else {
                        r.knobCCs.removeAll { $0.field == f.id }
                    }
                })
    }

    private func ccBinding(_ f: Field) -> Binding<Int> {
        Binding(get: { Int(r.knobCCs.first { $0.field == f.id }?.cc ?? 0) },
                set: { v in
                    if let i = r.knobCCs.firstIndex(where: { $0.field == f.id }) { r.knobCCs[i].cc = UInt8(max(0, min(127, v))) }
                })
    }
}
