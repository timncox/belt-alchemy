// The on-screen devices: a Launchpad Mini MK3, a Launch Control XL, a
// gamepad and a Daisy Patch, for trying controllers, Learn, mappings and
// routing in the Simulator or the demo with no hardware. On in the demo
// and with the module emulator (Controls > On-screen devices).
//
// They enter the app where the real ones do, and nowhere else:
//  - the Launchpad, the XL and the Daisy Patch are MIDI devices in the Rig's
//    device list (with the real ones' names) whose ports Rig.connectRaw
//    opens as VirtualPorts instead of CoreMIDI ports. ControllerHub reads
//    the bytes they send and sends them LEDs, routed notes and mapped CCs
//    as it would a real device's;
//  - the gamepad is a gamepad to ControllerHub, by name, its buttons queued
//    as GameController's are.
// So routes and mappings made against them carry over to the real ones.
// The bytes each device sends and reads are worked out in AlchemyLink
// (VirtualDevices.swift there), with tests.

import CoreMIDI
import Foundation
import SwiftUI
import AlchemyLink

/// One on-screen device's MIDI port, as ControllerHub holds it.
final class VirtualPort: MIDIPortOut, @unchecked Sendable {
    let device: MIDIDevice
    /// Bytes from the device to the app (ControllerHub's handler).
    fileprivate let toApp: ([UInt8]) -> Void
    /// Bytes from the app to the device, taken on the main actor.
    fileprivate let toDevice: @MainActor ([UInt8]) -> Void

    fileprivate init(device: MIDIDevice, toApp: @escaping ([UInt8]) -> Void,
                     toDevice: @escaping @MainActor ([UInt8]) -> Void) {
        self.device = device; self.toApp = toApp; self.toDevice = toDevice
    }

    func send(_ messages: [[UInt8]]) {
        let bytes = messages.flatMap { $0 }
        guard !bytes.isEmpty else { return }
        // Later, as CoreMIDI would deliver it, so nothing runs inside the sender.
        let deliver = toDevice
        Task { @MainActor in deliver(bytes) }
    }
}

@MainActor
final class VirtualDevices: ObservableObject {
    enum Device: String, CaseIterable, Identifiable {
        case launchpad, launchControlXL, gamepad, daisyPatch
        var id: String { rawValue }
        var title: String {
            switch self {
            case .launchpad: return "Launchpad"
            case .launchControlXL: return "Launch Control XL"
            case .gamepad: return "Gamepad"
            case .daisyPatch: return "Daisy Patch"
            }
        }
        var icon: String {
            switch self {
            case .launchpad: return "square.grid.3x3.fill"
            case .launchControlXL: return "slider.vertical.3"
            case .gamepad: return "gamecontroller"
            case .daisyPatch: return "pianokeys"
            }
        }
    }

    @Published private(set) var on = false
    @Published private(set) var launchpad = VirtualLaunchpad()
    @Published private(set) var xl = VirtualLaunchControlXL()
    @Published private(set) var pad = VirtualGamepad()
    @Published private(set) var daisy = VirtualDaisyPatch()
    @Published private(set) var names = VirtualDeviceNames()
    /// The MIDI devices to list with CoreMIDI's (empty while off).
    @Published private(set) var midiDevices: [MIDIDevice] = []
    /// The on-screen gamepad's name while it's on, for ControllerHub.
    @Published private(set) var gamepadName: String?

    /// The gamepad's buttons changed: what's held now (set by ControllerHub).
    var onPad: ((Set<XInputButton>) -> Void)?
    /// The device list changed (the Rig merges it with CoreMIDI's).
    var onDevicesChanged: (() -> Void)?

    /// Open ports, by the MIDI device's id (kept until ControllerHub lets go).
    private var ports: [MIDIUniqueID: VirtualPort] = [:]
    private var which: [MIDIUniqueID: Device] = [:]
    /// A new set of ids each time the devices are listed again (after a
    /// rename), so ControllerHub sees the renamed one as newly plugged in.
    private var generation: Int32 = 0

    init() {
        if let d = UserDefaults.standard.data(forKey: "virtualNames"),
           let n = try? JSONDecoder().decode(VirtualDeviceNames.self, from: d) {
            names = n
        }
    }

    // MARK: on and off

    func setOn(_ want: Bool) {
        guard want != on else { return }
        on = want
        if !want {
            // Let go of everything held, as unplugging does.
            launchpad = VirtualLaunchpad()
            xl = VirtualLaunchControlXL()
            if !pad.held.isEmpty { pad = VirtualGamepad(); onPad?([]) }
            daisy = VirtualDaisyPatch()
        }
        relist()
    }

    func rename(gamepad: String? = nil, daisyPatch: String? = nil) {
        var n = names
        if let g = gamepad?.trimmingCharacters(in: .whitespaces), !g.isEmpty { n.gamepad = g }
        if let d = daisyPatch?.trimmingCharacters(in: .whitespaces), !d.isEmpty { n.daisyPatch = d }
        guard n != names else { return }
        names = n
        if let d = try? JSONEncoder().encode(n) { UserDefaults.standard.set(d, forKey: "virtualNames") }
        if on { generation &+= 1; relist() }
    }

    private func relist() {
        var list: [MIDIDevice] = []
        which = [:]
        if on {
            let named: [(Device, String)] = [(.launchpad, VirtualDeviceNames.launchpad),
                                             (.launchControlXL, VirtualDeviceNames.launchControlXL),
                                             (.daisyPatch, names.daisyPatch)]
            for (i, (dev, name)) in named.enumerated() {
                // Ids well away from CoreMIDI's usual ones; one port per device.
                let id = -(0x0A1C_0000 + (generation & 0xFFFF) * 16 + Int32(i) * 2)
                var d = MIDIDevice(id: id, name: name, kind: .usb)
                d.deviceID = id - 1
                d.deviceName = name
                d.isVirtual = true
                list.append(d)
                which[id] = dev
            }
        }
        midiDevices = list
        gamepadName = on ? names.gamepad : nil
        onDevicesChanged?()
    }

    // MARK: ports (Rig.connectRaw)

    func connect(_ d: MIDIDevice, receive: @escaping ([UInt8]) -> Void) -> VirtualPort? {
        if let p = ports[d.id] { return p }
        guard let dev = which[d.id] else { return nil }
        let p = VirtualPort(device: d, toApp: receive) { [weak self] bytes in self?.fromApp(dev, bytes) }
        ports[d.id] = p
        return p
    }

    func owns(_ id: MIDIUniqueID) -> Bool { ports[id] != nil }

    func disconnect(_ id: MIDIUniqueID) { ports[id] = nil }

    private func port(_ dev: Device) -> VirtualPort? {
        guard on else { return nil }
        return ports.first { which[$0.key] == dev }?.value
    }

    private func fromApp(_ dev: Device, _ bytes: [UInt8]) {
        guard on else { return }
        switch dev {
        case .launchpad: launchpad.receive(bytes)
        case .launchControlXL: xl.receive(bytes)
        case .daisyPatch: daisy.receive(bytes)
        case .gamepad: break
        }
    }

    /// Bytes from a device to the app, as CoreMIDI's thread would hand them over.
    private func emit(_ dev: Device, _ bytes: [UInt8]?) {
        guard let bytes, let p = port(dev) else { return }
        p.toApp(bytes)
    }

    // MARK: playing them

    func launchpad(row: Int, col: Int, down: Bool) {
        emit(.launchpad, VirtualLaunchpad.press(row: row, col: col, down: down))
    }

    func xlKnob(row: Int, col: Int, to v: Int) {
        emit(.launchControlXL, xl.turn(row: row, col: col, to: UInt8(clamping: max(0, v))))
    }

    func xlFader(col: Int, to v: Int) {
        emit(.launchControlXL, xl.slide(col: col, to: UInt8(clamping: max(0, v))))
    }

    func xlButton(row: Int, col: Int, down: Bool) {
        emit(.launchControlXL, VirtualLaunchControlXL.press(row: row, col: col, down: down))
    }

    func gamepad(_ b: XInputButton, down: Bool) {
        guard on, pad.set(b, down: down) else { return }
        onPad?(pad.held)
    }

    func daisyKnob(_ i: Int, to v: Int) {
        emit(.daisyPatch, daisy.turn(i, to: UInt8(clamping: max(0, v))))
    }

    func daisyGate(_ down: Bool) {
        emit(.daisyPatch, daisy.setGate(down))
    }

    func clearMonitor() { daisy.clearMonitor() }
}
