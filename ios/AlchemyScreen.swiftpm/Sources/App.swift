import SwiftUI
import AlchemyLink

@main
struct AlchemyScreenApp: App {
    @StateObject private var store = ModuleStore()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(store)
                .preferredColorScheme(.dark)
                .tint(.orange)
        }
    }
}

struct RootView: View {
    @EnvironmentObject var store: ModuleStore

    var body: some View {
        Group {
            if store.phase == .ready {
                TabView {
                    ScreenTab().tabItem { Label("Screen", systemImage: "circle.grid.2x2") }
                    KnobsTab().tabItem { Label("Knobs", systemImage: "dial.medium") }
                    PresetsTab().tabItem { Label("Presets", systemImage: "square.grid.4x3.fill") }
                    if store.fsAvailable {
                        SamplesTab().tabItem { Label("Samples", systemImage: "waveform") }
                    }
                    if store.phone?.hasFirmware == true {
                        FirmwareTab().tabItem { Label("Firmware", systemImage: "cpu") }
                    }
                }
            } else {
                ConnectView()
            }
        }
        .alert("Alchemy Lab", isPresented: Binding(get: { store.notice != nil }, set: { if !$0 { store.notice = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(store.notice ?? "")
        }
    }
}

struct ConnectView: View {
    @EnvironmentObject var store: ModuleStore

    var body: some View {
        NavigationStack {
            List {
                Section {
                    switch store.phase {
                    case .searching:
                        Label("Plug the Alchemy Lab into this iPhone's USB-C port.", systemImage: "cable.connector")
                        Text("On the module, set **USB port** to **Phone** on the Firmware settings page and power it up again. Hold B1 while powering up to get Mac mode back for the web programmer.")
                            .font(.footnote).foregroundStyle(.secondary)
                    case .connecting:
                        HStack { ProgressView(); Text("Talking to the module…") }
                    case .failed(let why):
                        Label(why, systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
                    case .ready:
                        EmptyView()
                    }
                }
                if !store.ports.isEmpty {
                    Section("MIDI devices") {
                        ForEach(store.ports) { p in
                            Button(p.name) { store.connect(p) }
                        }
                    }
                }
                Section {
                    Button("Look again") { store.rescan() }
                    Button("Try the demo module") { store.startDemo() }
                } footer: {
                    Text("The demo is a pretend module inside the app, for trying the screens without hardware.")
                }
            }
            .navigationTitle("Alchemy Screen")
        }
    }
}

/// The module's name and a way out, on every tab.
struct ModuleToolbar: ToolbarContent {
    @EnvironmentObject var store: ModuleStore

    var body: some ToolbarContent {
        ToolbarItem(placement: .navigationBarTrailing) {
            Menu {
                if let h = store.hello {
                    Text("\(h.moduleName) \(h.fw)")
                }
                if case .demo = store.link { Text("Demo module") }
                Button("Disconnect", role: .destructive) { store.disconnect() }
            } label: {
                Image(systemName: store.link == .demo ? "theatermasks" : "cable.connector")
            }
        }
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
