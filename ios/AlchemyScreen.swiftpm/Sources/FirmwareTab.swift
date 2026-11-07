// Switch firmware from the phone, through the module's own picker: the
// .bin files in /alchemy on the card, written into the app slot, verified,
// and booted. Exactly what the Firmware settings page does with its pots;
// no DFU, which an iPhone can't do.

import SwiftUI
import UniformTypeIdentifiers
import AlchemyLink

struct FirmwareTab: View {
    @EnvironmentObject var store: ModuleStore
    @State private var importing = false
    @State private var confirm: String?

    var body: some View {
        NavigationStack {
            List {
                if let s = store.fw, s.state != .idle {
                    Section("Switching") { status(s) }
                }
                Section {
                    ForEach(store.firmwares) { fw in
                        Button {
                            confirm = fw.name
                        } label: {
                            HStack {
                                Label(fw.name, systemImage: "cpu")
                                Spacer()
                                Text(ByteCountFormatter.string(fromByteCount: Int64(fw.size), countStyle: .file))
                                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                            }
                        }
                        .disabled(busy)
                    }
                    if store.firmwares.isEmpty {
                        Text("No .bin files in /alchemy on the card yet.").foregroundStyle(.secondary)
                    }
                } header: {
                    Text("On the card")
                } footer: {
                    Text("Tap one to switch to it. Picking the firmware that is already running changes nothing. The phone keeps working afterwards only if the new firmware has Phone mode too; otherwise use the module, a Mac, or the card.")
                }
            }
            .navigationTitle("")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                // Not .bottomBar: inside a TabView it sits behind the tab bar.
                ToolbarItem(placement: .navigationBarLeading) {
                    Button { importing = true } label: { Label("Add a .bin", systemImage: "plus") }
                        .disabled(busy || store.transfer != nil)
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [UTType(filenameExtension: "bin") ?? .data],
                          allowsMultipleSelection: true) { result in
                if case let .success(urls) = result { store.uploadFirmware(urls) }
            }
            .confirmationDialog("Switch to \(confirm ?? "")?",
                                isPresented: Binding(get: { confirm != nil }, set: { if !$0 { confirm = nil } }),
                                titleVisibility: .visible) {
                Button("Write it and reboot") { if let n = confirm { store.switchFirmware(to: n) }; confirm = nil }
            } message: {
                Text("The module checks the file, writes it, verifies it and reboots into it. Sound stops for the reboot. Don't unplug its power while it writes.")
            }
            .refreshable { store.refreshFirmwares() }
            .onAppear { store.refreshFirmwares() }
        }
    }

    private var busy: Bool {
        guard let s = store.fw else { return false }
        return [.comparing, .erasing, .writing, .verifying].contains(s.state)
    }

    @ViewBuilder private func status(_ s: FwStatus) -> some View {
        switch s.state {
        case .comparing: ProgressRow(title: "Checking against the running firmware", value: s.progress)
        case .erasing: ProgressRow(title: "Erasing", value: s.progress)
        case .writing: ProgressRow(title: "Writing", value: s.progress)
        case .verifying: ProgressRow(title: "Verifying", value: s.progress)
        case .done: Label("Done. The module is rebooting into it.", systemImage: "checkmark.circle")
        case .same: Label("That firmware is already running. Nothing was written.", systemImage: "equal.circle")
        case .error: Label("It stopped (\(s.error.isEmpty ? "unknown" : s.error)). Nothing was booted. If it stopped while writing, switch again before you power the module off.", systemImage: "xmark.octagon")
                .foregroundStyle(.red)
        case .idle: EmptyView()
        }
    }
}

struct ProgressRow: View {
    var title: String
    var value: Double

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
            ProgressView(value: value)
        }
    }
}
