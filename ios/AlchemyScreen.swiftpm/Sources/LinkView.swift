// Linking the two modules: shared key and scale, knobs that follow knobs
// on the other module, and harmonies split across both. The app copies the
// moves across as they happen, from the module's own pots or from here.

import SwiftUI
import AlchemyLink

struct LinkView: View {
    @EnvironmentObject var rig: Rig
    @State private var adding = false

    var body: some View {
        NavigationStack {
            List {
                if !rig.canLink {
                    Section {
                        Label("Connect a second module to link them.", systemImage: "link")
                            .foregroundStyle(.secondary)
                    }
                } else {
                    Section {
                        Toggle("Share key and scale", isOn: Binding(get: { rig.linking.keyShared },
                                                                    set: { rig.setShareKey($0) }))
                            .disabled(rig.linking.splitHarmonies)
                    } footer: {
                        Text("Turn Key or Scale on either module and the other follows. On while harmonies are split.")
                    }
                    Section {
                        Toggle("Split harmonies across both", isOn: Binding(get: { rig.linking.splitHarmonies },
                                                                            set: { rig.setSplit($0) }))
                        if rig.linking.splitHarmonies {
                            ForEach(0..<4, id: \.self) { i in
                                Picker(selection: Binding(get: { rig.linking.plan[i] }, set: { rig.setPlan(i, $0) })) {
                                    ForEach(0..<BeltTheory.intervals.count, id: \.self) { Text(BeltTheory.intervals[$0]).tag($0) }
                                } label: {
                                    Text("Voice \(i + 1)")
                                    Text(i < 2 ? rig.name(rig.ready[0]) : rig.name(rig.ready[1]))
                                }
                            }
                        }
                    } footer: {
                        Text("Four harmony voices, two sung by each module. Cable the same voice into both modules' inputs; each output carries its two harmonies. A module's own Voice 1 and 2 pots change its voices here.")
                    }
                    Section {
                        ForEach(rig.linking.follows) { f in
                            VStack(alignment: .leading, spacing: 2) {
                                Text("\(rig.label(f.from)) → \(rig.label(f.to))")
                                if f.lo != 0 || f.hi != 1 {
                                    Text("\(Int(f.lo * 100))% to \(Int(f.hi * 100))%").font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                        .onDelete { idx in for i in idx { rig.removeFollow(rig.linking.follows[i].id) } }
                        Button { adding = true } label: { Label("Add a knob that drives another", systemImage: "plus") }
                    } header: {
                        Text("Follows")
                    } footer: {
                        Text("One way: the first knob drives the second, over the range you set. Swipe to remove.")
                    }
                    Section {
                        Text("Clock, gates and audio between the modules belong on patch cables: through the phone they arrive a few milliseconds late over USB, and 25 ms or more over Bluetooth.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Link")
            .sheet(isPresented: $adding) { AddFollow() }
        }
    }
}

private struct AddFollow: View {
    @EnvironmentObject var rig: Rig
    @Environment(\.dismiss) private var dismiss
    @State private var from: ControlTarget?
    @State private var to: ControlTarget?
    @State private var lo = 0.0, hi = 1.0

    var body: some View {
        NavigationStack {
            Form {
                Section("This knob") { picker($from) }
                Section("Drives") { picker($to, excluding: from?.unit) }
                Section {
                    VStack(alignment: .leading) {
                        Text("From \(Int(lo * 100))%").font(.footnote)
                        Slider(value: $lo, in: 0...1)
                        Text("To \(Int(hi * 100))%").font(.footnote)
                        Slider(value: $hi, in: 0...1)
                    }
                } header: { Text("Range") } footer: {
                    Text("The part of the second knob's travel the first one sweeps. From higher than To turns it round.")
                }
            }
            .navigationTitle("Add a follow")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Add") {
                        if let from, let to, rig.addFollow(Follow(from: from, to: to, lo: lo, hi: hi)) { dismiss() }
                    }
                    .disabled(from == nil || to == nil)
                }
            }
        }
    }

    private func picker(_ sel: Binding<ControlTarget?>, excluding unit: UInt32? = nil) -> some View {
        Picker("Knob", selection: sel) {
            Text("Choose").tag(ControlTarget?.none)
            ForEach(rig.ready.filter { $0.unit != unit }) { m in
                ForEach(m.allFields.filter { !$0.severs }) { f in
                    Text("\(rig.name(m)) · \(f.name)").tag(Optional(ControlTarget(unit: m.unit, field: f.id)))
                }
            }
        }
        .pickerStyle(.navigationLink)
    }
}
