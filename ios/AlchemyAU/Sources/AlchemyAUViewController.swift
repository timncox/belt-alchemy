// The extension's principal class (Info.plist NSExtensionPrincipalClass):
// the view the host shows, and the factory that makes the Audio Unit.

import CoreAudioKit
import SwiftUI
import AlchemyLink

public final class AlchemyAUViewController: AUViewController, AUAudioUnitFactory {
    private var audioUnit: AlchemyAudioUnit?
    private var model: AUPanelModel?
    private var hosting: UIHostingController<AlchemyAUView>?

    public override func viewDidLoad() {
        super.viewDidLoad()
        let m = AUPanelModel()
        model = m
        let h = UIHostingController(rootView: AlchemyAUView(model: m))
        addChild(h)
        h.view.translatesAutoresizingMaskIntoConstraints = false
        h.view.backgroundColor = .clear
        view.addSubview(h.view)
        NSLayoutConstraint.activate([
            h.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            h.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            h.view.topAnchor.constraint(equalTo: view.topAnchor),
            h.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        h.didMove(toParent: self)
        hosting = h
        preferredContentSize = CGSize(width: 640, height: 520)
        if let au = audioUnit { m.attach(au) }
    }

    // MARK: AUAudioUnitFactory

    /// Called by the host, not necessarily on the main thread.
    nonisolated public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        let au = try AlchemyAudioUnit(componentDescription: componentDescription, options: [])
        Task { @MainActor [weak self] in self?.attach(au) }
        return au
    }

    private func attach(_ au: AlchemyAudioUnit) {
        audioUnit = au
        if isViewLoaded { model?.attach(au) }
    }
}

/// What the view shows: the two slots and every parameter's value, kept in
/// step with the parameter tree.
@MainActor
final class AUPanelModel: ObservableObject {
    @Published var slots: [SlotStatus] = [SlotStatus(), SlotStatus()]
    @Published private(set) var values: [UInt64: Float] = [:]
    private weak var au: AlchemyAudioUnit?
    private var token: AUParameterObserverToken?

    func attach(_ au: AlchemyAudioUnit) {
        detach()
        self.au = au
        guard let tree = au.parameterTree else { return }
        for p in tree.allParameters { values[p.address] = p.value }
        // Changes from the host, the modules (readback) and CCs.
        token = tree.token(byAddingParameterObserver: { @Sendable [weak self] address, value in
            Task { @MainActor in self?.values[address] = value }
        })
        slots = au.slotStatus
        au.onSlotStatus = { [weak self] s in
            Task { @MainActor in self?.slots = s }
        }
        // Opening the view also starts looking for modules, for a host that
        // shows the view before it starts rendering.
        au.startModules()
    }

    func detach() {
        if let t = token, let tree = au?.parameterTree { tree.removeParameterObserver(t) }
        token = nil
        au?.onSlotStatus = nil
    }

    func value(_ a: UInt64) -> Float {
        values[a] ?? AUParams.spec(a)?.defaultValue ?? 0
    }

    /// A slider or button: through the parameter, so the host records it
    /// and the module gets it (implementorValueObserver).
    func set(_ a: UInt64, _ v: Float) {
        values[a] = v
        au?.parameterTree?.parameter(withAddress: a)?.setValue(v, originator: token)
    }

    /// A slider's drag starting or ending, for hosts that record automation.
    func gesture(_ a: UInt64, began: Bool) {
        guard let p = au?.parameterTree?.parameter(withAddress: a) else { return }
        p.setValue(value(a), originator: token, atHostTime: 0, eventType: began ? .touch : .release)
    }
}
