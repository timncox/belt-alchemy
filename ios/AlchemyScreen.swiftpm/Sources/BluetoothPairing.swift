// Apple's own Bluetooth MIDI pairing screen: it finds Bluetooth LE MIDI
// devices nearby (a WIDI Uhost on a module, say) and connects them. A
// connected adapter then shows up as an ordinary MIDI device, which the
// app tries as a module like anything on the USB-C port.

import CoreAudioKit
import SwiftUI

struct BluetoothPairing: UIViewControllerRepresentable {
    @Environment(\.dismiss) private var dismiss

    func makeUIViewController(context: Context) -> UINavigationController {
        let vc = CABTMIDICentralViewController()
        let nav = UINavigationController(rootViewController: vc)
        vc.navigationItem.rightBarButtonItem = UIBarButtonItem(systemItem: .done, primaryAction: UIAction { _ in
            context.coordinator.done()
        })
        return nav
    }

    func updateUIViewController(_ vc: UINavigationController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(done: { dismiss() }) }

    final class Coordinator {
        let done: () -> Void
        init(done: @escaping () -> Void) { self.done = done }
    }
}
