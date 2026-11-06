// HostLink over the iPhone's USB-C: the module in Phone mode is a
// class-compliant USB MIDI device named "Alchemy Lab", so CoreMIDI sees it
// with no driver, no app entitlement and no MFi. HostLink bytes go out as
// SysEx (SysExPipe) and come back the same way.
//
// Uses the MIDI 1.0 packet-list API: SysEx arrives as plain bytes there,
// which is all the pipe needs. Apple marks it deprecated in favour of the
// UMP event-list API; it still works on current iOS.

import CoreMIDI
import Foundation
import AlchemyLink

struct MIDIPort: Identifiable, Hashable {
    var id: String { name }
    var name: String
}

final class MIDILink: ByteTransport, @unchecked Sendable {
    private var client = MIDIClientRef()
    private var inPort = MIDIPortRef()
    private var outPort = MIDIPortRef()
    private var source: MIDIEndpointRef = 0
    private var dest: MIDIEndpointRef = 0
    private let lock = NSLock()
    private var decoder = SysExPipe.Decoder()
    private var handler: (([UInt8]) -> Void)?

    /// Called on any MIDI setup change: a module plugged in, unplugged, or
    /// rebooting into another firmware.
    var onSetupChanged: (() -> Void)?

    private(set) var connectedName: String?

    init() throws {
        var c = MIDIClientRef()
        var st = MIDIClientCreateWithBlock("Alchemy Screen" as CFString, &c) { [weak self] note in
            if note.pointee.messageID == .msgSetupChanged {
                DispatchQueue.main.async { self?.onSetupChanged?() }
            }
        }
        guard st == noErr else { throw MIDIError(status: st, what: "create MIDI client") }
        client = c

        var i = MIDIPortRef()
        st = MIDIInputPortCreateWithBlock(client, "HostLink in" as CFString, &i) { [weak self] list, _ in
            self?.receive(list)
        }
        guard st == noErr else { throw MIDIError(status: st, what: "create MIDI input") }
        inPort = i

        var o = MIDIPortRef()
        st = MIDIOutputPortCreate(client, "HostLink out" as CFString, &o)
        guard st == noErr else { throw MIDIError(status: st, what: "create MIDI output") }
        outPort = o
    }

    deinit { MIDIClientDispose(client) }

    // MARK: finding the module

    static func displayName(_ e: MIDIEndpointRef) -> String {
        var s: Unmanaged<CFString>?
        guard MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) == noErr, let v = s else { return "" }
        return v.takeRetainedValue() as String
    }

    /// Every MIDI device that can both send and receive.
    func ports() -> [MIDIPort] {
        let ins = Set((0..<MIDIGetNumberOfSources()).map { Self.displayName(MIDIGetSource($0)) })
        let outs = Set((0..<MIDIGetNumberOfDestinations()).map { Self.displayName(MIDIGetDestination($0)) })
        return ins.intersection(outs).filter { !$0.isEmpty }.sorted().map { MIDIPort(name: $0) }
    }

    /// The port that looks like an Alchemy Lab in Phone mode, if any.
    func likelyModule() -> MIDIPort? {
        ports().first { $0.name.localizedCaseInsensitiveContains("alchemy") }
    }

    func connect(_ port: MIDIPort) -> Bool {
        disconnect()
        let src = (0..<MIDIGetNumberOfSources()).map { MIDIGetSource($0) }.first { Self.displayName($0) == port.name }
        let dst = (0..<MIDIGetNumberOfDestinations()).map { MIDIGetDestination($0) }.first { Self.displayName($0) == port.name }
        guard let s = src, let d = dst, MIDIPortConnectSource(inPort, s, nil) == noErr else { return false }
        lock.lock()
        source = s; dest = d; decoder = SysExPipe.Decoder(); connectedName = port.name
        lock.unlock()
        return true
    }

    func disconnect() {
        lock.lock()
        let s = source
        source = 0; dest = 0; connectedName = nil
        lock.unlock()
        if s != 0 { MIDIPortDisconnectSource(inPort, s) }
    }

    /// Is the endpoint we're talking to still there?
    var stillPresent: Bool {
        guard let n = connectedName else { return false }
        return ports().contains { $0.name == n }
    }

    // MARK: ByteTransport

    func setReceiveHandler(_ h: @escaping ([UInt8]) -> Void) {
        lock.lock(); handler = h; lock.unlock()
    }

    func send(_ bytes: [UInt8]) {
        lock.lock()
        let d = dest
        lock.unlock()
        guard d != 0 else { return }
        let size = 1024
        let buf = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { buf.deallocate() }
        let list = buf.bindMemory(to: MIDIPacketList.self, capacity: 1)
        for msg in SysExPipe.encodeStream(bytes) {
            // One complete SysEx message per packet list, as MIDISend wants.
            let first = MIDIPacketListInit(list)
            _ = MIDIPacketListAdd(list, size, first, 0, msg.count, msg)
            MIDISend(outPort, d, list)
        }
    }

    private func receive(_ list: UnsafePointer<MIDIPacketList>) {
        var raw: [UInt8] = []
        lock.lock()
        for packet in list.unsafeSequence() {
            let len = Int(packet.pointee.length)
            let data = UnsafeRawPointer(packet) + MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
            let bytes = UnsafeBufferPointer(start: data.assumingMemoryBound(to: UInt8.self), count: len)
            decoder.feed(bytes) { raw.append($0) }
        }
        let h = handler
        lock.unlock()
        if !raw.isEmpty { h?(raw) }
    }
}

struct MIDIError: Error, CustomStringConvertible {
    var status: OSStatus
    var what: String
    var description: String { "Couldn't \(what) (\(status))" }
}
