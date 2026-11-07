// Both modules on one screen: one pitch graph with both singers on the same
// note lanes (Left orange, Right cyan), and each one's note, harmonies and
// levels underneath. For when the two are played as one big module.

import SwiftUI
import AlchemyLink

struct CombinedScreen: View {
    @EnvironmentObject var rig: Rig
    var header: AnyView? = nil

    var body: some View {
        let ms = Array(rig.ready.prefix(2))
        let colors: [Color] = [Theme.key, Theme.led(0x5FD0FF)]
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                if let header { header }
                PitchGraph(lines: ms.enumerated().compactMap { i, m in
                    m.state?.belt.map { PitchLine(trace: m.trace, belt: $0, color: colors[i]) }
                }, fieldStyle: true)
                .frame(height: 300)
                ForEach(Array(ms.enumerated()), id: \.element.id) { i, m in
                    ModuleSummary(module: m, color: colors[i])
                }
            }
            .padding()
        }
        .background(Theme.ground)
    }
}

private struct ModuleSummary: View {
    @ObservedObject var module: ModuleStore
    @EnvironmentObject var rig: Rig
    let color: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Circle().fill(color).frame(width: 10, height: 10).shadow(color: color, radius: 5)
                Text(rig.name(module)).font(Theme.display(24, weight: .black)).foregroundStyle(Theme.mist)
                Spacer()
                LinkBadge(kind: module.kind)
            }
            if let st = module.state, let b = st.belt {
                Tuner(belt: b)
                Voices(belt: b)
                Meters(st: st)
            } else {
                Text("This module's firmware doesn't send its state.").font(.footnote).foregroundStyle(Theme.fog)
            }
        }
        .padding(.vertical, 18)
        .overlay(alignment: .top) { Rectangle().fill(Theme.line).frame(height: 1) }
        .environmentObject(module)
    }
}
