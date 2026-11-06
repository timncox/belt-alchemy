// Where each of the app's controls goes: pick a source (the pad's X, Y and
// pressure, tilt, roll and turn, the mic's level and pitch), then a knob on
// either module, and the part of that knob's range it sweeps. Motion and
// the mic are switched on here.

import SwiftUI
import AlchemyLink

struct ControlsView: View {
    @EnvironmentObject var rig: Rig
    @EnvironmentObject var inputs: Inputs
    @EnvironmentObject var hub: ControllerHub
    @Environment(\.horizontalSizeClass) private var hsc

    var body: some View {
        NavigationStack {
            List {
                ControllersSection()
                if hsc != .regular {
                    // On iPad, Routing is its own tab.
                    Section {
                        NavigationLink { RoutingScreen() } label: {
                            HStack {
                                Label("MIDI routing", systemImage: "arrow.triangle.branch")
                                Spacer()
                                Text("\(hub.midiRoutes.count)").foregroundStyle(.secondary).monospacedDigit()
                            }
                        }
                    } footer: {
                        Text("Any device on the hub to any other over USB: a keyboard or the Launchpad to the Daisy Patch, a module's sung note out, notes into a module.")
                    }
                }
                Section("Pad") {
                    ForEach([ControlSource.padX, .padY, .pressure]) { RouteRow(source: $0) }
                }
                Section {
                    Toggle("Use tilt and turn", isOn: $inputs.motionOn)
                        .disabled(!inputs.motionAvailable)
                    if inputs.motionOn {
                        Button("Zero: the way I'm holding it now is the middle") { inputs.zeroMotion() }
                        VStack(alignment: .leading) {
                            Text("Full sweep at \(Int(inputs.motionRange))° either side").font(.footnote)
                            Slider(value: $inputs.motionRange, in: 10...90, step: 5)
                        }
                    }
                    ForEach([ControlSource.tilt, .roll, .turn]) { RouteRow(source: $0) }
                } header: {
                    Text("Motion")
                } footer: {
                    Text("Tilt is leaning the top away or towards you, roll is leaning sideways, turn is spinning it flat on a table.")
                }
                Section {
                    Toggle("Use the mic", isOn: $inputs.micOn)
                    if inputs.micOn {
                        HStack {
                            Text("Hearing")
                            Spacer()
                            Text(inputs.micNote.map { BeltTheory.name(Int($0.rounded())) } ?? "–").monospacedDigit()
                        }
                    }
                    ForEach([ControlSource.micLevel, .micPitch]) { RouteRow(source: $0) }
                } header: {
                    Text("Mic")
                } footer: {
                    Text("Level follows how loud it is; pitch follows the note sung or played, from E2 to C6. This only moves knobs: to sing into Belt, cable the phone's audio out (a USB-C audio adapter) to the module's input.")
                }
            }
            .navigationTitle("Controls")
            .controllerLearnSheet()
            .alert("Mic", isPresented: Binding(get: { inputs.micProblem != nil }, set: { if !$0 { inputs.micProblem = nil } })) {
                Button("OK", role: .cancel) {}
            } message: { Text(inputs.micProblem ?? "") }
        }
    }
}

private struct RouteRow: View {
    @EnvironmentObject var rig: Rig
    @EnvironmentObject var inputs: Inputs
    let source: ControlSource

    var body: some View {
        NavigationLink {
            RouteEditor(source: source)
        } label: {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(source.name)
                    Text(rig.target(of: source).map { rig.label($0.target) } ?? "Not set")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                ProgressView(value: inputs.value(source)).frame(width: 60)
            }
        }
    }
}

private struct RouteEditor: View {
    @EnvironmentObject var rig: Rig
    let source: ControlSource
    @State private var lo = 0.0
    @State private var hi = 1.0

    var body: some View {
        let current = rig.target(of: source)
        List {
            Section {
                Button {
                    rig.route(source, to: nil)
                } label: {
                    HStack { Text("Nothing"); Spacer(); if current == nil { Image(systemName: "checkmark") } }
                }
            }
            ForEach(rig.ready) { m in
                Section(rig.name(m)) {
                    ForEach(m.gridPages) { pg in
                        ForEach(pg.fields) { f in
                            let t = ControlTarget(unit: m.unit, field: f.id)
                            Button {
                                rig.route(source, to: t)
                                rig.setRange(source, lo: lo, hi: hi)
                            } label: {
                                HStack {
                                    Text(f.name)
                                    Text(pg.name).font(.caption).foregroundStyle(.secondary)
                                    Spacer()
                                    if current?.target == t { Image(systemName: "checkmark") }
                                }
                            }
                            .disabled(f.severs)
                        }
                    }
                }
            }
            if current != nil {
                Section {
                    VStack(alignment: .leading) {
                        Text("From \(Int(lo * 100))%").font(.footnote)
                        Slider(value: $lo, in: 0...1) { _ in rig.setRange(source, lo: lo, hi: hi) }
                        Text("To \(Int(hi * 100))%").font(.footnote)
                        Slider(value: $hi, in: 0...1) { _ in rig.setRange(source, lo: lo, hi: hi) }
                    }
                    Button("Flip") { let t = lo; lo = hi; hi = t; rig.setRange(source, lo: lo, hi: hi) }
                } header: {
                    Text("Range")
                } footer: {
                    Text("The part of the knob's travel this sweeps. From higher than To turns it round.")
                }
            }
            SourceMIDIOutSection(source: source)
        }
        .navigationTitle(source.name)
        .onAppear {
            if let r = rig.target(of: source) { lo = r.lo; hi = r.hi }
        }
    }
}
