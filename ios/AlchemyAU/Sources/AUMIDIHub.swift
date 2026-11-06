// The extension's own CoreMIDI client: finds Alchemy Labs and opens a
// HostLink pipe to each. Copied from the app's MIDIHub.swift (pairing a
// source with its destination through their shared entity, a device known
// by its CoreMIDI unique ID, never its name) rather than shared, because
// the app's file is the app's to change.
//
// Known limit: the extension finds modules by name only (a device whose
// name contains "alchemy"). The app also connects devices it remembered
// answering as an Alchemy Lab under another name (a renamed Bluetooth
// adapter, kept in the app's @AppStorage "knownDevices"), but the extension
// runs in its own process with its own defaults and can't read that list.
// Name the adapters with "Alchemy" in CME's app, or share the list later
// through an App Group (both targets would need the group entitlement).
//
// Uses the MIDI 1.0 packet-list API, like the app: SysEx arrives as plain
// bytes there. Apple marks it deprecated in favour of the UMP event-list
// API; it still works on current iOS.

import CoreMIDI
import Foundation
import AlchemyLink

struct AUMIDIDevice: Identifiable, Hashable {
    var id: MIDIUniqueID
    var name: String
    var kind: LinkKind
    var looksLikeModule: Bool { name.localizedCaseInsensitiveContains("alchemy") }
}

final class AUMIDIHub: @unchecked Sendable {
    private var client = MIDIClientRef()
    private var inPort = MIDIPortRef()
    private var outPort = MIDIPortRef()
    private let lock = NSLock()
    private var open: [MIDIUniqueID: AUMIDIConnection] = [:]

    /// Any MIDI setup change (called on the main queue).
    var onSetupChanged: (() -> Void)?

    init() throws {
        var c = MIDIClientRef()
        var st = MIDIClientCreateWithBlock("Alchemy Screen AU" as CFString, &c) { [weak self] note in
            if note.pointee.messageID == .msgSetupChanged {
                DispatchQueue.main.async { self?.onSetupChanged?() }
            }
        }
        guard st == noErr else { throw AUMIDIError(status: st, what: "create MIDI client") }
        client = c

        var i = MIDIPortRef()
        st = MIDIInputPortCreateWithBlock(client, "HostLink in (AU)" as CFString, &i) { [weak self] list, refCon in
            self?.receive(list, from: refCon)
        }
        guard st == noErr else { throw AUMIDIError(status: st, what: "create MIDI input") }
        inPort = i

        var o = MIDIPortRef()
        st = MIDIOutputPortCreate(client, "HostLink out (AU)" as CFString, &o)
        guard st == noErr else { throw AUMIDIError(status: st, what: "create MIDI output") }
        outPort = o
    }

    deinit { MIDIClientDispose(client) }

    // MARK: finding devices

    static func string(_ o: MIDIObjectRef, _ p: CFString) -> String {
        var s: Unmanaged<CFString>?
        guard MIDIObjectGetStringProperty(o, p, &s) == noErr, let v = s else { return "" }
        return v.takeRetainedValue() as String
    }

    static func uniqueID(_ o: MIDIObjectRef) -> MIDIUniqueID {
        var v: Int32 = 0
        MIDIObjectGetIntegerProperty(o, kMIDIPropertyUniqueID, &v)
        return v
    }

    private static func entity(_ e: MIDIEndpointRef) -> MIDIEntityRef {
        var ent = MIDIEntityRef()
        MIDIEndpointGetEntity(e, &ent)
        return ent
    }

    private static func kind(_ e: MIDIEndpointRef) -> LinkKind {
        let ent = entity(e)
        var dev = MIDIDeviceRef()
        if ent != 0, MIDIEntityGetDevice(ent, &dev) == noErr, dev != 0 {
            let owner = string(dev, kMIDIPropertyDriverOwner)
            if !owner.isEmpty { return LinkKind(driverOwner: owner) }
        }
        return LinkKind(driverOwner: string(e, kMIDIPropertyDriverOwner))
    }

    /// The destination on the same entity as a source, else (virtual or odd
    /// devices) the one destination with the same name.
    private static func destination(for src: MIDIEndpointRef) -> MIDIEndpointRef? {
        let dests = (0..<MIDIGetNumberOfDestinations()).map { MIDIGetDestination($0) }
        let ent = entity(src)
        if ent != 0, let d = dests.first(where: { entity($0) == ent }) { return d }
        let name = string(src, kMIDIPropertyDisplayName)
        let same = dests.filter { string($0, kMIDIPropertyDisplayName) == name }
        return same.count == 1 ? same[0] : nil
    }

    private static func source(_ id: MIDIUniqueID) -> MIDIEndpointRef? {
        (0..<MIDIGetNumberOfSources()).map { MIDIGetSource($0) }.first { uniqueID($0) == id }
    }

    /// Devices that look like Alchemy Labs (by name; see the note above).
    func modules() -> [AUMIDIDevice] {
        var out: [AUMIDIDevice] = []
        for i in 0..<MIDIGetNumberOfSources() {
            let s = MIDIGetSource(i)
            guard Self.destination(for: s) != nil else { continue }
            let name = Self.string(s, kMIDIPropertyDisplayName)
            let d = AUMIDIDevice(id: Self.uniqueID(s), name: name, kind: Self.kind(s))
            if d.looksLikeModule { out.append(d) }
        }
        return out
    }

    func isPresent(_ id: MIDIUniqueID) -> Bool { Self.source(id) != nil }

    // MARK: connections

    func connect(_ d: AUMIDIDevice) -> AUMIDIConnection? {
        guard let s = Self.source(d.id), let dst = Self.destination(for: s) else { return nil }
        lock.lock()
        if let old = open[d.id] { lock.unlock(); return old }
        let c = AUMIDIConnection(device: d, dest: dst, outPort: outPort)
        open[d.id] = c
        lock.unlock()
        // The refCon carries the unique ID, so replies are sorted to the
        // right module even when both arrive in the same instant.
        let ref = UnsafeMutableRawPointer(bitPattern: Int(UInt32(bitPattern: d.id)) | 0x1_0000_0000)
        guard MIDIPortConnectSource(inPort, s, ref) == noErr else {
            lock.lock(); open[d.id] = nil; lock.unlock()
            return nil
        }
        return c
    }

    func disconnect(_ id: MIDIUniqueID) {
        lock.lock()
        let c = open.removeValue(forKey: id)
        lock.unlock()
        if c != nil, let s = Self.source(id) { MIDIPortDisconnectSource(inPort, s) }
    }

    func disconnectAll() {
        lock.lock()
        let ids = Array(open.keys)
        lock.unlock()
        for id in ids { disconnect(id) }
    }

    private func receive(_ list: UnsafePointer<MIDIPacketList>, from refCon: UnsafeMutableRawPointer?) {
        guard let r = refCon else { return }
        let id = MIDIUniqueID(bitPattern: UInt32(truncatingIfNeeded: Int(bitPattern: r)))
        lock.lock()
        let c = open[id]
        lock.unlock()
        c?.receive(list)
    }
}

/// One device's HostLink pipe: SysEx out to its destination, SysEx in from
/// its source, decoded back to HostLink bytes.
final class AUMIDIConnection: ByteTransport, @unchecked Sendable {
    let device: AUMIDIDevice
    private let dest: MIDIEndpointRef
    private let outPort: MIDIPortRef
    private let lock = NSLock()
    private var decoder = SysExPipe.Decoder()
    private var handler: (([UInt8]) -> Void)?

    init(device: AUMIDIDevice, dest: MIDIEndpointRef, outPort: MIDIPortRef) {
        self.device = device; self.dest = dest; self.outPort = outPort
    }

    func setReceiveHandler(_ h: @escaping ([UInt8]) -> Void) {
        lock.lock(); handler = h; lock.unlock()
    }

    func send(_ bytes: [UInt8]) {
        let size = 1024
        let buf = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { buf.deallocate() }
        let list = buf.bindMemory(to: MIDIPacketList.self, capacity: 1)
        for msg in SysExPipe.encodeStream(bytes) {
            // One complete SysEx message per packet list, as MIDISend wants.
            let first = MIDIPacketListInit(list)
            _ = MIDIPacketListAdd(list, size, first, 0, msg.count, msg)
            MIDISend(outPort, dest, list)
        }
    }

    fileprivate func receive(_ list: UnsafePointer<MIDIPacketList>) {
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

struct AUMIDIError: Error, CustomStringConvertible {
    var status: OSStatus
    var what: String
    var description: String { "Couldn't \(what) (\(status))" }
}
