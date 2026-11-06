// The controllers on the hub, at the top of the Controls tab: each one's
// route (Left, Right, Both, App only, Off), the Learn flow (tap Learn, move
// a control, pick what it does) and the list of what's mapped.

import SwiftUI
import AlchemyLink

struct ControllersSection: View {
    @EnvironmentObject var hub: ControllerHub
    @EnvironmentObject var rig: Rig

    var body: some View {
        Section {
            if hub.controllers.isEmpty {
                Text("Plug a Launchpad Mini MK3, a Launch Control XL, a gamepad or a MIDI keyboard into the hub with the modules.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            ForEach(hub.controllers) { ControllerRow(controller: $0) }
            Button { hub.beginLearn() } label: {
                Label("Learn: move a control, then pick what it does", systemImage: "wand.and.stars")
            }
            .disabled(hub.controllers.isEmpty)
            NavigationLink { MappingsList() } label: {
                HStack {
                    Text("Mapped in the app")
                    Spacer()
                    Text("\(hub.mappings.count)").foregroundStyle(.secondary).monospacedDigit()
                }
            }
            if let line = unsupportedLine {
                Label(line, systemImage: "exclamationmark.triangle")
                    .font(.footnote).foregroundStyle(.orange)
            }
        } header: {
            Text("Controllers")
        } footer: {
            Text("Left, Right or Both: the controller plays the module as in its Launchpad USB mode, with its LEDs. App only: just what you map here. A control mapped here isn't sent to the module.")
        }
    }

    private var unsupportedLine: String? {
        let names = rig.ready.filter { hub.unsupported.contains($0.unit) }.map { rig.name($0) }
        guard !names.isEmpty else { return nil }
        return "\(names.joined(separator: " and ")): firmware without controller support (UNSUPPORTED); update it to forward controllers."
    }
}

private struct ControllerRow: View {
    @EnvironmentObject var hub: ControllerHub
    @EnvironmentObject var rig: Rig
    let controller: ControllerInfo

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .frame(width: 28)
                .foregroundColor(.orange)
            VStack(alignment: .leading, spacing: 2) {
                Text(controller.name).lineLimit(1)
                Text(hub.lastMoved[controller.id] ?? controller.kind.label)
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 8)
            Picker("Sends to", selection: Binding(get: { hub.route(of: controller) },
                                                  set: { hub.setRoute($0, for: controller) })) {
                ForEach(ControllerRoute.choices(for: controller.kind)) { r in Text(title(r)).tag(r) }
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .fixedSize()
        }
    }

    private func title(_ r: ControllerRoute) -> String {
        switch r {
        case .left: return rig.ready.isEmpty ? "Left (not connected)" : "Left"
        case .right: return rig.ready.count < 2 ? "Right (not connected)" : "Right"
        default: return r.title
        }
    }

    private var icon: String {
        switch controller.kind {
        case .launchpad: return "square.grid.3x3.fill"
        case .launchControlXL: return "slider.vertical.3"
        case .gamepad: return "gamecontroller"
        case .midi: return "pianokeys"
        }
    }
}

/// Everything mapped in the app, by controller and control. Tap a control
/// for its targets.
struct MappingsList: View {
    @EnvironmentObject var hub: ControllerHub

    private var devices: [String] { Array(Set(hub.mappings.map(\.control.device))).sorted() }

    var body: some View {
        List {
            if hub.mappings.isEmpty {
                Text("Nothing mapped yet. Tap Learn on the Controls page, move a control, and pick what it does.")
                    .foregroundStyle(.secondary)
            }
            ForEach(devices, id: \.self) { d in
                let controls = controlsOf(d)
                Section(d) {
                    ForEach(controls, id: \.self) { c in
                        NavigationLink { ControlTargets(control: c) } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(c.element.label)
                                Text(hub.targets(for: c).map { hub.label($0) }.joined(separator: "; "))
                                    .font(.caption).foregroundStyle(.secondary).lineLimit(2)
                            }
                        }
                    }
                    .onDelete { idx in for i in idx { for m in hub.targets(for: controls[i]) { hub.remove(m.id) } } }
                }
            }
        }
        .navigationTitle("Mapped in the app")
    }

    private func controlsOf(_ d: String) -> [ControllerControl] {
        var seen = Set<ControllerControl>(), out: [ControllerControl] = []
        for m in hub.mappings where m.control.device == d && seen.insert(m.control).inserted { out.append(m.control) }
        return out
    }
}

/// One control's targets (it can drive several, each with its own range):
/// tap one to change it, swipe to remove, or add another.
struct ControlTargets: View {
    @EnvironmentObject var hub: ControllerHub
    @EnvironmentObject var rig: Rig
    let control: ControllerControl
    var midiOnly = false
    @State private var editing: ControllerMapping?

    var body: some View {
        let targets = hub.targets(for: control)
        List {
            Section {
                ForEach(targets) { m in
                    Button { editing = m } label: {
                        Text(hub.label(m)).foregroundStyle(.primary)
                    }
                }
                .onDelete { idx in for i in idx { hub.remove(targets[i].id) } }
                NavigationLink {
                    MappingEditor(control: control, initial: nil, midiOnly: midiOnly)
                } label: {
                    Label(targets.isEmpty ? "Pick what it does" : "Add another target", systemImage: "plus")
                }
            } header: {
                Text("\(control.device) · \(control.element.label)")
            } footer: {
                Text("One control can drive several things at once, each over its own range: a knob on Left and a CC on another MIDI device, say.")
            }
        }
        .navigationTitle(control.element.label)
        .navigationBarTitleDisplayMode(.inline)
        .sheet(item: $editing) { m in
            NavigationStack { MappingEditor(control: m.control, initial: m, midiOnly: midiOnly) }
                .environmentObject(hub)
                .environmentObject(rig)
        }
    }
}

/// MIDI out for one of the app's own sources (on its page in Controls).
struct SourceMIDIOutSection: View {
    @EnvironmentObject var hub: ControllerHub
    let source: ControlSource

    var body: some View {
        let c = ControllerControl(device: ControllerMap.appDevice, element: .app(source))
        Section {
            ForEach(hub.targets(for: c)) { m in Text(hub.label(m)) }
            NavigationLink {
                ControlTargets(control: c, midiOnly: true)
            } label: {
                Label(hub.targets(for: c).isEmpty ? "Send to a MIDI device" : "Change MIDI out", systemImage: "pianokeys")
            }
        } header: {
            Text("MIDI out")
        } footer: {
            Text("As well as the knob above: a CC (over a range) or a note to another MIDI device on the hub, such as a Daisy Patch.")
        }
    }
}

extension View {
    /// The Learn sheet: waits for a control to move, then asks what it does.
    func controllerLearnSheet() -> some View { modifier(LearnSheetModifier()) }
}

private struct LearnSheetModifier: ViewModifier {
    @EnvironmentObject var hub: ControllerHub
    @EnvironmentObject var rig: Rig

    func body(content: Content) -> some View {
        content.sheet(isPresented: Binding(get: { (hub.learning && !hub.learnInPlace) || hub.learned != nil },
                                           set: { if !$0 { hub.cancelLearn() } })) {
            NavigationStack { LearnSheet() }
                .environmentObject(hub)
                .environmentObject(rig)
        }
    }
}

private struct LearnSheet: View {
    @EnvironmentObject var hub: ControllerHub
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        if let c = hub.learned {
            ControlTargets(control: c)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) { Button("Done") { hub.finishLearn() } }
                }
        } else {
            VStack(spacing: 18) {
                ProgressView()
                Text("Move a knob, fader, pad or button on any controller.")
                    .font(.headline).multilineTextAlignment(.center)
                Text(hub.controllers.map(\.name).joined(separator: ", "))
                    .font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
            }
            .padding()
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .navigationTitle("Learn")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            }
        }
    }
}

/// One target of a control: an action, its range, and for a button whether
/// it holds or latches. `midiOnly` for the app's own sources.
struct MappingEditor: View {
    @EnvironmentObject var hub: ControllerHub
    @EnvironmentObject var rig: Rig
    @Environment(\.dismiss) private var dismiss
    let control: ControllerControl
    let initial: ControllerMapping?
    var midiOnly = false
    @State private var action: ControllerAction?
    @State private var lo = 0.0
    @State private var hi = 1.0
    @State private var mode = ButtonMode.momentary
    @State private var loaded = false
    @State private var outDevice = ""
    @State private var outChannel = 0
    @State private var outNumber = 1

    var body: some View {
        Form {
            Section("Control") {
                Text("\(control.device) · \(control.element.label)")
            }
            if control.element.isButton && (action?.hasToggle ?? true) {
                Section {
                    Picker("Button", selection: $mode) {
                        Text("Hold").tag(ButtonMode.momentary)
                        Text("Latch").tag(ButtonMode.toggle)
                    }
                    .pickerStyle(.segmented)
                } footer: {
                    Text(mode == .momentary ? "On while held." : "Each press switches it on or off.")
                }
            }
            if action?.takesRange == true {
                Section {
                    VStack(alignment: .leading) {
                        Text("From \(Int(lo * 100))%").font(.footnote)
                        Slider(value: $lo, in: 0...1)
                        Text("To \(Int(hi * 100))%").font(.footnote)
                        Slider(value: $hi, in: 0...1)
                    }
                    Button("Flip") { let t = lo; lo = hi; hi = t }
                } header: {
                    Text("Range")
                } footer: {
                    Text(control.element.isButton ? "The button jumps between these." : "The part of the travel this sweeps. From higher than To turns it round.")
                }
            }
            midiOutSection
            if !midiOnly { moduleSections }
        }
        .navigationTitle(initial == nil ? "What it does" : "Change target")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") { save() }.disabled(action == nil)
            }
        }
        .onAppear {
            guard !loaded else { return }
            loaded = true
            outDevice = hub.outputDevices.first { $0 != control.device } ?? hub.outputDevices.first ?? ""
            if let m = initial {
                action = m.action; lo = m.lo; hi = m.hi; mode = m.mode
                if case let .midiCC(t) = m.action { setOut(t) }
                if case let .midiNote(t) = m.action { setOut(t) }
            }
        }
        .onChange(of: outDevice) { _ in retarget() }
        .onChange(of: outChannel) { _ in retarget() }
        .onChange(of: outNumber) { _ in retarget() }
    }

    private var outTarget: MIDIOutTarget {
        MIDIOutTarget(device: outDevice, channel: UInt8(outChannel), number: UInt8(outNumber))
    }

    private func setOut(_ t: MIDIOutTarget) {
        outDevice = t.device; outChannel = Int(t.channel); outNumber = Int(t.number)
    }

    /// The device, channel or number changed: a MIDI target picked follows it.
    private func retarget() {
        guard loaded else { return }
        if case .midiCC = action { action = .midiCC(outTarget) }
        if case .midiNote = action { action = .midiNote(outTarget) }
    }

    @ViewBuilder private var midiOutSection: some View {
        Section {
            if hub.outputDevices.isEmpty {
                Text("No other MIDI device on the hub.").foregroundStyle(.secondary)
            } else {
                Picker("Device", selection: $outDevice) {
                    ForEach(hub.outputDevices, id: \.self) { d in
                        Text(hub.isPlugged(d) ? d : "\(d) (not connected)").tag(d)
                    }
                }
                Stepper("Channel \(outChannel + 1)", value: $outChannel, in: 0...15)
                Stepper("Number \(outNumber)  (note \(BeltTheory.name(outNumber)))", value: $outNumber, in: 0...127)
                choice(.midiCC(outTarget), "CC \(outNumber)", detail: "a value over the range")
                choice(.midiNote(outTarget), "Note \(BeltTheory.name(outNumber))", detail: "a gate: on while held")
            }
        } header: {
            Text("MIDI out to another device")
        } footer: {
            Text("CCs go out when the value changes, at most 60 times a second.")
        }
    }

    @ViewBuilder private var moduleSections: some View {
        Section("XY pad") {
            choice(.padX, "Pad X")
            choice(.padY, "Pad Y")
        }
        Section {
            choice(.shareKey, "Share key and scale")
            choice(.splitHarmonies, "Split harmonies")
        } header: {
            Text("Link")
        } footer: {
            if !rig.canLink { Text("Works once two modules are connected.") }
        }
        Section("HARD and mute") {
            ForEach(unitChoices, id: \.self) { u in choice(.hard(unit: u), "HARD · \(hub.moduleName(u))") }
            ForEach(unitChoices, id: \.self) { u in choice(.mute(unit: u), "Mute · \(hub.moduleName(u))") }
        }
        Section("Presets") {
            ForEach(unitChoices, id: \.self) { u in choice(.presetNext(unit: u), "Next · \(hub.moduleName(u))") }
            ForEach(unitChoices, id: \.self) { u in choice(.presetPrevious(unit: u), "Previous · \(hub.moduleName(u))") }
        }
        ForEach(rig.ready) { m in
            Section("\(rig.name(m)) knobs") {
                ForEach(m.gridPages) { pg in
                    ForEach(pg.fields.filter { !$0.severs }) { f in
                        choice(.knob(ControlTarget(unit: m.unit, field: f.id)), f.name, detail: pg.name)
                    }
                }
            }
        }
    }

    /// Every module, and both together when there are two.
    private var unitChoices: [UInt32?] {
        (rig.ready.count > 1 ? [nil] : []) + rig.ready.map { Optional($0.unit) }
    }

    private func choice(_ a: ControllerAction, _ title: String, detail: String? = nil) -> some View {
        Button {
            action = a
        } label: {
            HStack {
                Text(title).foregroundStyle(.primary)
                if let detail { Text(detail).font(.caption).foregroundStyle(.secondary) }
                Spacer()
                if action == a { Image(systemName: "checkmark").foregroundColor(.accentColor) }
            }
        }
    }

    private func save() {
        guard let action else { return }
        var m = ControllerMapping(control: control, action: action, lo: lo, hi: hi,
                                  mode: action.hasToggle ? mode : .momentary)
        if let initial { m.id = initial.id }
        hub.save(m)
        dismiss()
    }
}
