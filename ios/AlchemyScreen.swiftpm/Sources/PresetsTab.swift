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
                LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 12), count: 4), spacing: 12) {
                    ForEach(0..<16, id: \.self) { i in
                        let s = store.slots.first { $0.slot == i }
                        let used = s?.valid == true
                        let foreign = used && s?.ours == false
                        Button {
                            if used && !foreign { store.load(i) }
                        } label: {
                            let shape = RoundedRectangle(cornerRadius: 18, style: .continuous)
                            VStack(alignment: .leading, spacing: 0) {
                                HStack {
                                    Circle().fill(used && !foreign ? Theme.key : .clear).frame(width: 7, height: 7)
                                        .shadow(color: used && !foreign ? Theme.key : .clear, radius: 4)
                                    Spacer()
                                    if foreign { Text("other fw").font(.caption2).foregroundStyle(Theme.fog) }
                                }
                                Spacer(minLength: 6)
                                Text("\(i + 1)").font(Theme.display(30, weight: .bold))
                                    .foregroundStyle(used && !foreign ? Theme.mist : Theme.fog.opacity(used ? 0.6 : 0.4))
                            }
                            .padding(12)
                            .frame(maxWidth: .infinity, minHeight: 82, alignment: .leading)
                            .background(used ? Theme.surface : Color.clear, in: shape)
                            .overlay(shape.stroke(used ? Color.clear : Theme.line, lineWidth: 1))
                            .accessibilityElement()
                            .accessibilityLabel("Preset \(i + 1), \(foreign ? "other firmware" : used ? "saved" : "empty")")
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
                    .font(.footnote).foregroundStyle(Theme.fog).padding(.horizontal)
            }
            .refreshable { await store.refreshSlots() }
            .background(Theme.ground)
            .toolbar(.hidden, for: .navigationBar)
            .confirmationDialog("Replace preset \((confirmSave ?? 0) + 1)?",
                                isPresented: Binding(get: { confirmSave != nil }, set: { if !$0 { confirmSave = nil } }),
                                titleVisibility: .visible) {
                Button("Replace", role: .destructive) { if let i = confirmSave { store.save(i) }; confirmSave = nil }
            }
        }
    }
}
