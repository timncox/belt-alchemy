// The module's SD card, through HostLink's filesystem block: browse
// folders, add files from the Files app, delete, make folders. Uploads are
// atomic on the module (staged, CRC-checked, then renamed), so a pulled
// cable never leaves half a sample under its real name.

import SwiftUI
import UniformTypeIdentifiers
import AlchemyLink

struct SamplesTab: View {
    @EnvironmentObject var store: ModuleStore
    @State private var importing = false
    @State private var newFolder = false
    @State private var folderName = ""

    var body: some View {
        NavigationStack {
            List {
                if let t = store.transfer {
                    Section {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("Sending \(t.name)").font(.subheadline)
                            ProgressView(value: t.progress)
                            Text(rate(t.rate)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                        }
                    }
                }
                Section {
                    if store.cwd != "/" {
                        Button { store.up() } label: { Label("Up", systemImage: "arrow.up.left") }
                    }
                    ForEach(store.entries) { e in
                        if e.isDir {
                            Button { store.open(e.path) } label: { Label(e.name, systemImage: "folder") }
                        } else {
                            HStack {
                                Label(e.name, systemImage: icon(e.name))
                                Spacer()
                                if e.locked { Image(systemName: "lock").foregroundStyle(.secondary) }
                                Text(size(e.size)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                            }
                        }
                    }
                    .onDelete { idx in
                        for e in idx.map({ store.entries[$0] }) where !e.locked { store.delete(e) }
                    }
                } header: {
                    Text(store.cwd)
                } footer: {
                    footer
                }
            }
            .navigationTitle("Samples")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItemGroup(placement: .bottomBar) {
                    Button { newFolder = true } label: { Label("New folder", systemImage: "folder.badge.plus") }
                    Spacer()
                    Button { importing = true } label: { Label("Add files", systemImage: "plus") }
                        .disabled(store.transfer != nil)
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.audio, .data], allowsMultipleSelection: true) { result in
                if case let .success(urls) = result { store.upload(urls) }
            }
            .alert("New folder", isPresented: $newFolder) {
                TextField("Name", text: $folderName)
                Button("Make") { store.makeFolder(folderName); folderName = "" }
                Button("Cancel", role: .cancel) { folderName = "" }
            }
            .refreshable { store.open(store.cwd) }
            .onAppear { if store.entries.isEmpty { store.open(store.cwd) } }
        }
    }

    @ViewBuilder private var footer: some View {
        if let fs = store.fs {
            let free = fs.freeKiB.map { " · \(size(UInt32(min(UInt64($0) * 1024, UInt64(UInt32.max))))) free" } ?? ""
            Text("SD card\(free). Sending over USB is much slower than a card reader; the speed shows while a file goes. For big batches, pull the card and use a USB-C SD card reader in the Files app.")
        }
    }

    private func icon(_ name: String) -> String {
        switch (name as NSString).pathExtension.lowercased() {
        case "wav", "aif", "aiff", "raw": return "waveform"
        case "bin": return "cpu"
        case "cfg", "txt", "json": return "doc.text"
        default: return "doc"
        }
    }

    private func size(_ b: UInt32) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(b), countStyle: .file)
    }

    private func rate(_ bps: Double) -> String {
        bps > 0 ? ByteCountFormatter.string(fromByteCount: Int64(bps), countStyle: .file) + "/s" : "starting…"
    }
}
