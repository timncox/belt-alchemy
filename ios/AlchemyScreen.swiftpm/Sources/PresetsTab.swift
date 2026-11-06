// The module's 16 preset slots: tap to load, hold for save and clear.
// Saving here is exactly the panel's save gesture (SAVE_TO_SLOT).

import SwiftUI
import AlchemyLink

struct PresetsTab: View {
    @EnvironmentObject var store: ModuleStore
    @State private var confirmSave: Int?

    var body: some View {
        NavigationStack {
            ScrollView {
                LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10), count: 4), spacing: 10) {
                    ForEach(0..<16, id: \.self) { i in
                        let s = store.slots.first { $0.slot == i }
                        let used = s?.valid == true
                        let foreign = used && s?.ours == false
                        Button {
                            if used && !foreign { store.load(i) }
                        } label: {
                            VStack(spacing: 4) {
                                Text("\(i + 1)").font(.title3.monospacedDigit().weight(.semibold))
                                Text(foreign ? "other fw" : used ? "saved" : "empty").font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            .frame(maxWidth: .infinity, minHeight: 64)
                            .background(used ? Color.orange.opacity(foreign ? 0.12 : 0.28) : Color.white.opacity(0.05),
                                        in: RoundedRectangle(cornerRadius: 12))
                        }
                        .buttonStyle(.plain)
                        .contextMenu {
                            Button("Save current sound here") { if used { confirmSave = i } else { store.save(i) } }
                            if used {
                                Button("Load") { store.load(i) }.disabled(foreign)
                                Button("Clear", role: .destructive) { store.erase(i) }
                            }
                        }
                    }
                }
                .padding()
                Text("Tap a saved slot to load it. Touch and hold a slot to save into it or clear it. Slots marked “other fw” belong to another firmware on this module.")
                    .font(.footnote).foregroundStyle(.secondary).padding(.horizontal)
            }
            .refreshable { await store.refreshSlots() }
            .navigationTitle("Presets")
            .navigationBarTitleDisplayMode(.inline)
            .confirmationDialog("Replace preset \((confirmSave ?? 0) + 1)?",
                                isPresented: Binding(get: { confirmSave != nil }, set: { if !$0 { confirmSave = nil } }),
                                titleVisibility: .visible) {
                Button("Replace", role: .destructive) { if let i = confirmSave { store.save(i) }; confirmSave = nil }
            }
        }
    }
}
