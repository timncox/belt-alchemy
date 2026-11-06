// Both modules on one screen: one pitch graph with both singers on the same
// note lanes (Left orange, Right cyan), and each one's note, harmonies and
// levels underneath. For when the two are played as one big module.

import SwiftUI
import AlchemyLink

struct CombinedScreen: View {
    @EnvironmentObject var rig: Rig

    var body: some View {
        let ms = Array(rig.ready.prefix(2))
        let colors: [Color] = [.orange, .cyan]
        ScrollView {
            VStack(spacing: 12) {
                PitchGraph(lines: ms.enumerated().compactMap { i, m in
                    m.state?.belt.map { PitchLine(trace: m.trace, belt: $0, color: colors[i]) }
                })
                .frame(height: 260)
                ForEach(Array(ms.enumerated()), id: \.element.id) { i, m in
                    ModuleSummary(module: m, color: colors[i])
                }
            }
            .padding()
        }
    }
}

private struct ModuleSummary: View {
    @ObservedObject var module: ModuleStore
    @EnvironmentObject var rig: Rig
    let color: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Circle().fill(color).frame(width: 10, height: 10)
                Text(rig.name(module)).font(.headline)
                Spacer()
                LinkBadge(kind: module.kind)
            }
            if let st = module.state, let b = st.belt {
                Tuner(belt: b)
                Voices(belt: b)
                Meters(st: st)
            } else {
                Text("This module's firmware doesn't send its state.").font(.footnote).foregroundStyle(.secondary)
            }
        }
        .padding()
        .background(Color.white.opacity(0.04), in: RoundedRectangle(cornerRadius: 12))
        .environmentObject(module)
    }
}
