// How a module is reached, and what that means for how often the app asks
// it things. USB MIDI moves about 100 KB/s each way; Bluetooth LE MIDI
// manages about 5 KB/s for SysEx and adds a round trip of 20-30 ms, so over
// Bluetooth the app asks for the screen less often and warns before a big
// file. Everything else (the commands, the screens) is the same on any link.

import Foundation

public enum LinkKind: String, Codable, Sendable {
    /// `emulator`: the Mac's module emulator over TCP (MIDIStreamLink), at USB's pace.
    case usb, bluetooth, network, demo, emulator

    /// Seconds between STATE requests.
    public var statePeriod: Double {
        switch self {
        case .usb, .demo, .emulator: return 1.0 / 15
        case .network: return 1.0 / 10
        case .bluetooth: return 1.0 / 6
        }
    }

    /// Seconds between knob writes from a pad, tilt or the mic. A write
    /// carries the whole running state (about 60 bytes, about 90 as SysEx).
    public var controlPeriod: Double {
        switch self {
        case .usb, .demo, .emulator: return 1.0 / 60
        case .network: return 1.0 / 40
        case .bluetooth: return 1.0 / 20
        }
    }

    /// Rough bytes per second for files, for the time a transfer will take.
    public var bytesPerSecond: Double {
        switch self {
        case .usb, .demo, .emulator: return 60_000
        case .network: return 20_000
        case .bluetooth: return 4_000
        }
    }

    /// Files bigger than this get a "this will take a while" first.
    public var bigFile: Int { self == .bluetooth ? 64_000 : 4_000_000 }

    public var label: String {
        switch self {
        case .usb: return "USB"
        case .bluetooth: return "Bluetooth"
        case .network: return "Network"
        case .demo: return "Demo"
        case .emulator: return "Emulator"
        }
    }

    /// From CoreMIDI's driver owner for the device (kMIDIPropertyDriverOwner).
    public init(driverOwner: String) {
        let d = driverOwner.lowercased()
        if d.contains("bluetooth") || d.contains("btle") { self = .bluetooth }
        else if d.contains("rtp") || d.contains("network") { self = .network }
        else { self = .usb }
    }
}
