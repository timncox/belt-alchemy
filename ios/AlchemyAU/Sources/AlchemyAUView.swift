// The extension's own view, inside the host: each module's name and link,
// its twelve knobs as sliders, and HARD and Mute as buttons. Side by side
// when there's room, one above the other when not.

import SwiftUI
import AlchemyLink

struct AlchemyAUView: View {
    @ObservedObject var model: AUPanelModel

    var body: some View {
        GeometryReader { geo in
            ScrollView {
                if geo.size.width >= 560 {
                    HStack(alignment: .top, spacing: 12) {
                        ModuleColumn(model: model, slot: 0)
                        ModuleColumn(model: model, slot: 1)
                    }
                    .padding(12)
                } else {
                    VStack(spacing: 12) {
                        ModuleColumn(model: model, slot: 0)
                        ModuleColumn(model: model, slot: 1)
                    }
                    .padding(12)
                }
            }
        }
        .background(Color(.systemBackground))
    }
}

private struct ModuleColumn: View {
    @ObservedObject var model: AUPanelModel
    let slot: Int

    private var status: SlotStatus { model.slots.indices.contains(slot) ? model.slots[slot] : SlotStatus() }
    private var ready: Bool { status.state == .ready }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            header
            HStack(spacing: 8) {
                ToggleButton(model: model, address: AUParams.address(slot: slot, param: 12), title: "HARD", tint: .orange)
                ToggleButton(model: model, address: AUParams.address(slot: slot, param: 13), title: "Mute", tint: .red)
            }
            ForEach(0..<12, id: \.self) { p in
                KnobRow(model: model, address: AUParams.address(slot: slot, param: p))
            }
        }
        .opacity(ready ? 1 : 0.55)
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemBackground)))
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Circle().fill(dot).frame(width: 8, height: 8)
                Text(AUParams.slotNames[slot]).font(.caption).foregroundStyle(.secondary)
            }
            Text(title).font(.headline).lineLimit(1)
            if let line = subtitle {
                Text(line).font(.caption).foregroundStyle(.secondary).lineLimit(2)
            }
        }
    }

    private var title: String {
        switch status.state {
        case .empty: return "No module"
        default: return status.name.isEmpty ? "Alchemy Lab" : status.name
        }
    }

    private var subtitle: String? {
        switch status.state {
        case .empty: return slot == 0 ? "Plug in an Alchemy Lab in Phone mode." : nil
        case .connecting: return "Connecting · \(status.detail)"
        case .ready: return status.detail
        case .failed(let why): return why
        }
    }

    private var dot: Color {
        switch status.state {
        case .ready: return .green
        case .connecting: return .yellow
        case .failed: return .red
        case .empty: return .gray
        }
    }
}

private struct KnobRow: View {
    @ObservedObject var model: AUPanelModel
    let address: UInt64

    var body: some View {
        let spec = AUParams.spec(address)!
        let binding = Binding<Float>(get: { model.value(address) }, set: { model.set(address, $0) })
        HStack(spacing: 8) {
            Text(spec.name).font(.subheadline).frame(width: 64, alignment: .leading)
            if spec.steps > 0 {
                Slider(value: binding, in: spec.minValue...spec.maxValue, step: 1,
                       onEditingChanged: { model.gesture(address, began: $0) })
            } else {
                Slider(value: binding, in: spec.minValue...spec.maxValue,
                       onEditingChanged: { model.gesture(address, began: $0) })
            }
            Text(AUParams.valueString(address, model.value(address)))
                .font(.subheadline.monospacedDigit())
                .frame(width: 72, alignment: .trailing)
                .lineLimit(1)
        }
    }
}

private struct ToggleButton: View {
    @ObservedObject var model: AUPanelModel
    let address: UInt64
    let title: String
    let tint: Color

    var body: some View {
        let on = model.value(address) >= 0.5
        Button {
            model.gesture(address, began: true)
            model.set(address, on ? 0 : 1)
            model.gesture(address, began: false)
        } label: {
            Text(title)
                .font(.subheadline.bold())
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
                .foregroundStyle(on ? Color.white : Color.primary)
                .background(RoundedRectangle(cornerRadius: 8).fill(on ? tint : Color.gray.opacity(0.2)))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(on ? .isSelected : [])
    }
}
