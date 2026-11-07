import SwiftUI
import AlchemyLink

@main
struct AlchemyScreenApp: App {
    @StateObject private var rig = Rig()
    @StateObject private var inputs = Inputs()
    @StateObject private var controllers = ControllerHub()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(rig)
                .environmentObject(inputs)
                .environmentObject(controllers)
                .environmentObject(rig.virtual)
                .onAppear {
                    inputs.rig = rig
                    controllers.attach(rig: rig, inputs: inputs)
                }
                .onChange(of: scenePhase) { phase in
                    // The Launchpad goes back to Live mode while the app is away.
                    if phase == .background { controllers.setActive(false) }
                    if phase == .active { controllers.setActive(true) }
                }
                .preferredColorScheme(.dark)
                .tint(Theme.key)
        }
    }
}

/// The modules, the pad, and how they're connected. On an iPad (or a big
/// iPhone sideways) two modules sit side by side, one per half; on an
/// iPhone the bar at the top switches between them.
struct RootView: View {
    @EnvironmentObject var rig: Rig
    @Environment(\.horizontalSizeClass) private var hsc

    var body: some View {
        Group {
            if rig.modules.isEmpty {
                ConnectView()
            } else {
                TabView {
                    ModulesView().tabItem { Label("Modules", systemImage: "rectangle.split.2x1") }
                    PadView().tabItem { Label("Pad", systemImage: "hand.point.up.left") }
                    // On iPhone the on-screen devices are a page under Controls.
                    if hsc == .regular && rig.virtualOn {
                        NavigationStack { DevicesScreen() }
                            .tabItem { Label("Devices", systemImage: "square.grid.3x3.fill") }
                    }
                    ControlsView().tabItem { Label("Controls", systemImage: "slider.horizontal.3") }
                    // On iPhone (and a narrow iPad window) Routing is a page under Controls.
                    if hsc == .regular {
                        NavigationStack { RoutingScreen() }
                            .tabItem { Label("Routing", systemImage: "arrow.triangle.branch") }
                    }
                    if rig.modules.count >= 2 {
                        LinkView().tabItem { Label("Link", systemImage: "link") }
                    }
                    NavigationStack { ConnectView(embedded: true) }
                        .tabItem { Label("Connect", systemImage: "cable.connector") }
                }
            }
        }
        .alert("Alchemy Screen", isPresented: Binding(get: { rig.notice != nil }, set: { if !$0 { rig.notice = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(rig.notice ?? "")
        }
    }
}

struct ModulesView: View {
    @EnvironmentObject var rig: Rig
    @Environment(\.horizontalSizeClass) private var hsc
    @AppStorage("oneScreen") private var oneScreen = false

    var body: some View {
        VStack(spacing: 0) {
            if rig.canLink {
                Picker("View", selection: $oneScreen) {
                    Text(hsc == .regular ? "Side by side" : "Each module").tag(false)
                    Text("One screen").tag(true)
                }
                .pickerStyle(.segmented)
                .padding(.horizontal).padding(.top, 6)
            }
            if oneScreen && rig.canLink {
                CombinedScreen()
            } else {
                columns
            }
        }
    }

    @ViewBuilder private var columns: some View {
        if hsc == .regular && rig.modules.count >= 2 {
            HStack(spacing: 0) {
                ModuleColumn(module: rig.modules[0])
                Divider()
                ModuleColumn(module: rig.modules[1])
            }
        } else if let m = rig.selected {
            ModuleColumn(module: m).id(m.id)
        }
    }
}

/// Everything for one module: its screen, knobs, presets, files and
/// firmware. Each half of the iPad has its own.
struct ModuleColumn: View {
    @ObservedObject var module: ModuleStore
    @EnvironmentObject var rig: Rig
    @State private var section = Section.screen

    enum Section: String, CaseIterable, Identifiable {
        case screen, knobs, presets, samples, firmware
        var id: String { rawValue }
        var icon: String {
            switch self {
            case .screen: return "waveform.path.ecg"
            case .knobs: return "dial.medium"
            case .presets: return "square.grid.4x3.fill"
            case .samples: return "folder"
            case .firmware: return "cpu"
            }
        }
        var title: String { rawValue.capitalized }
    }

    private var sections: [Section] {
        Section.allCases.filter {
            switch $0 {
            case .samples: return module.fsAvailable
            case .firmware: return module.phone?.hasFirmware == true
            default: return true
            }
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            ModuleBar(module: module)
            switch module.phase {
            case .connecting:
                Spacer()
                ProgressView("Talking to \(rig.name(module))…")
                Spacer()
            case .failed(let why):
                Spacer()
                Label(why, systemImage: "exclamationmark.triangle").foregroundStyle(.orange).padding()
                Spacer()
            case .ready:
                Picker("Section", selection: $section) {
                    ForEach(sections) { s in Image(systemName: s.icon).accessibilityLabel(s.title).tag(s) }
                }
                .pickerStyle(.segmented)
                .padding(.horizontal).padding(.bottom, 6)
                Group {
                    switch section {
                    case .screen: ScreenTab()
                    case .knobs: KnobsTab()
                    case .presets: PresetsTab()
                    case .samples: SamplesTab()
                    case .firmware: FirmwareTab()
                    }
                }
                .frame(maxHeight: .infinity)
            }
        }
        .environmentObject(module)
        .alert(rig.name(module), isPresented: Binding(get: { module.notice != nil }, set: { if !$0 { module.notice = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(module.notice ?? "")
        }
        .confirmationDialog("This will be slow", isPresented: Binding(get: { module.slowTransfer != nil },
                                                                      set: { if !$0 { module.slowTransfer = nil } })) {
            if let t = module.slowTransfer {
                Button("Send it anyway (about \(max(1, t.seconds / 60)) min)") {
                    module.slowTransfer = nil
                    if t.firmware { module.uploadFirmware(t.urls, confirmed: true) }
                    else { module.upload(t.urls, into: t.folder, confirmed: true) }
                }
            }
            Button("Cancel", role: .cancel) { module.slowTransfer = nil }
        } message: {
            Text("Over \(module.kind.label) this takes about \(max(1, (module.slowTransfer?.seconds ?? 0) / 60)) minutes. A USB-C cable is much faster.")
        }
    }
}

/// The module's name, how it's connected, and (on an iPhone with two
/// modules) the switch to the other one.
struct ModuleBar: View {
    @ObservedObject var module: ModuleStore
    @EnvironmentObject var rig: Rig
    @Environment(\.horizontalSizeClass) private var hsc
    @State private var renaming = false
    @State private var newName = ""

    var body: some View {
        HStack(spacing: 10) {
            if hsc != .regular && rig.modules.count > 1 {
                Picker("Module", selection: Binding(get: { rig.selected?.id ?? "" }, set: { rig.selectedID = $0 })) {
                    ForEach(rig.modules) { m in Text(rig.name(m)).tag(m.id) }
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 260)
            } else {
                Text(rig.name(module)).font(.headline)
            }
            Spacer()
            LinkBadge(kind: module.kind)
            Menu {
                if let h = module.hello { Text("\(h.moduleName) \(h.fw)") }
                Button("Rename…") { newName = rig.name(module); renaming = true }
                Button("Disconnect", role: .destructive) { rig.disconnect(module) }
            } label: {
                Image(systemName: "ellipsis.circle").imageScale(.large)
            }
        }
        .padding(.horizontal).padding(.vertical, 8)
        .alert("Name this module", isPresented: $renaming) {
            TextField("Name", text: $newName)
            Button("Save") { rig.rename(module, to: newName) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The name stays with this module on any cable or adapter.")
        }
    }
}

struct LinkBadge: View {
    var kind: LinkKind
    var body: some View {
        Label(kind.label, systemImage: icon)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 8).padding(.vertical, 4)
            .background(Color.white.opacity(0.08), in: Capsule())
            .foregroundStyle(kind == .bluetooth ? Color.cyan : Color.secondary)
    }
    private var icon: String {
        switch kind {
        case .usb: return "cable.connector"
        case .bluetooth: return "dot.radiowaves.left.and.right"
        case .network: return "wifi"
        case .demo: return "theatermasks"
        case .emulator: return "desktopcomputer"
        }
    }
}

/// Plugging in, pairing Bluetooth adapters, and the demo.
struct ConnectView: View {
    @EnvironmentObject var rig: Rig
    var embedded = false
    @State private var pairing = false

    var body: some View {
        let list = List {
            if !embedded {
                Section {
                    Label("Plug an Alchemy Lab into this device's USB-C port, or two through a USB-C hub.", systemImage: "cable.connector")
                    Text("On each module, set **USB port** to **Phone** on the Firmware settings page and power it up again. Hold B1 while powering up to get Mac mode back for the web programmer.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            if !rig.modules.isEmpty {
                Section("Connected") {
                    ForEach(rig.modules) { m in
                        HStack {
                            Text(rig.name(m))
                            Spacer()
                            LinkBadge(kind: m.kind)
                        }
                    }
                }
            }
            // Launchpads and Launch Control XLs are controllers (Controls tab), not modules.
            let others = rig.devices.filter { d in
                let kind = ControllerKind.classify(midiName: d.name + " " + d.deviceName)
                return !d.isVirtual && !rig.isConnected(d) && kind != .launchpad && kind != .launchControlXL
            }
            if !others.isEmpty {
                Section {
                    ForEach(others) { d in
                        Button {
                            rig.connect(d)
                        } label: {
                            HStack {
                                Text(d.name)
                                Spacer()
                                LinkBadge(kind: d.kind)
                            }
                        }
                    }
                } header: {
                    Text("Other MIDI devices")
                } footer: {
                    Text("A Bluetooth adapter shows its own name. Tap it to try it as a module; once it answers, it connects by itself next time.")
                }
            }
            Section {
                Button { pairing = true } label: { Label("Pair a Bluetooth MIDI adapter", systemImage: "dot.radiowaves.left.and.right") }
            } footer: {
                Text("A USB-host Bluetooth adapter (such as a CME WIDI Uhost) on each module's USB-C. Fine for the screen, knobs and presets; files and firmware are much quicker over a cable.")
            }
            EmulatorSection()
            Section {
                Button("Look again") { rig.rescan() }
                if rig.isDemo {
                    Button("Stop the demo", role: .destructive) { rig.stopDemo() }
                } else {
                    Button("Try one demo module") { rig.startDemo(count: 1) }
                    Button("Try two demo modules") { rig.startDemo(count: 2) }
                }
            } footer: {
                Text("The demo is pretend modules inside the app, for trying the screens without hardware.")
            }
        }
        .navigationTitle(embedded ? "Connect" : "Alchemy Screen")
        .sheet(isPresented: $pairing) { BluetoothPairing().ignoresSafeArea() }

        if embedded { list } else { NavigationStack { list } }
    }
}

/// Connect > Emulator: the Alchemy Lab module emulator on a Mac, over TCP,
/// Left and Right on their own ports.
struct EmulatorSection: View {
    @EnvironmentObject var rig: Rig
    @State private var host = ""
    @State private var left = ""
    @State private var right = ""

    var body: some View {
        Section {
            if rig.isEmulator {
                ForEach(Array(rig.emulator.ports.enumerated()), id: \.element) { i, port in
                    VStack(alignment: .leading, spacing: 2) {
                        Text("\(i == 0 ? "Left" : "Right") · \(rig.emulator.host):\(String(port))")
                        Text(rig.emulatorStatus[port] ?? "Connecting…")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                Button("Stop using the emulator", role: .destructive) { rig.stopEmulator() }
            } else {
                Button { save(); rig.startEmulator() } label: {
                    Label("Connect to the module emulator", systemImage: "desktopcomputer")
                }
            }
            LabeledContent("Host") {
                TextField("127.0.0.1", text: $host)
                    .multilineTextAlignment(.trailing)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                    .onSubmit(save)
            }
            LabeledContent("Left port") {
                TextField("5071", text: $left).multilineTextAlignment(.trailing).keyboardType(.numberPad).onSubmit(save)
            }
            LabeledContent("Right port") {
                TextField("5072", text: $right).multilineTextAlignment(.trailing).keyboardType(.numberPad).onSubmit(save)
            }
            if changed {
                Button("Save") { save() }
            }
        } header: {
            Text("Emulator")
        } footer: {
            Text("The Alchemy Lab module emulator on a Mac, started with --hostlink and a port for each module. In the Simulator 127.0.0.1 is the Mac; on an iPhone or iPad, type the Mac's address. While it isn't there, the app tries again every 2 seconds.")
        }
        .onAppear(perform: load)
        .onChange(of: rig.emulator) { _ in load() }
    }

    private func load() {
        host = rig.emulator.host
        left = String(rig.emulator.leftPort)
        right = String(rig.emulator.rightPort)
    }

    private var edited: EmulatorSettings {
        var e = rig.emulator
        let h = host.trimmingCharacters(in: .whitespaces)
        if !h.isEmpty { e.host = h }
        if let p = EmulatorSettings.port(left) { e.leftPort = p }
        if let p = EmulatorSettings.port(right) { e.rightPort = p }
        return e
    }

    private var changed: Bool { edited != rig.emulator }

    private func save() {
        rig.setEmulator(edited)
        load()
    }
}

extension Color {
    /// "#rrggbb" from the descriptor's pageColors; nil if malformed.
    init?(hex: String?) {
        guard var s = hex, s.hasPrefix("#") else { return nil }
        s.removeFirst()
        guard s.count == 6, let v = UInt32(s, radix: 16) else { return nil }
        self.init(red: Double((v >> 16) & 0xFF) / 255, green: Double((v >> 8) & 0xFF) / 255, blue: Double(v & 0xFF) / 255)
    }
}
