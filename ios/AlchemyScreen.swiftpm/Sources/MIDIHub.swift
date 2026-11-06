// Every MIDI device the iPhone or iPad can see, and a HostLink connection to
// as many of them as are Alchemy Labs: one on the USB-C port, two on a USB-C
// hub, or each on its own Bluetooth MIDI adapter. Two modules on a hub both
// call themselves "Alchemy Lab", so a device is found by its CoreMIDI
// unique ID, never its name, and its source and destination are paired
// through their shared entity. Which module is which comes later, from the
// unit number the module itself reports (Phone INFO).
//
// Uses the MIDI 1.0 packet-list API: SysEx arrives as plain bytes there,
// which is all the pipe needs. Apple marks it deprecated in favour of the
// UMP event-list API; it still works on current iOS.

import CoreMIDI
import Foundation
import AlchemyLink

/// A device that can both send and receive: one module, one adapter, or
/// anything else plugged in.
struct MIDIDevice: Identifiable, Hashable {
    /// The source's CoreMIDI unique ID: stable while it stays plugged in
    /// (and, for most USB devices, across replugs on the same port).
    var id: MIDIUniqueID
    var name: String
    var kind: LinkKind
    /// The CoreMIDI device the port belongs to (0 if none) and its name: a
    /// Launchpad Mini MK3 is one device with two ports (DAW and MIDI).
    var deviceID: MIDIUniqueID = 0
    var deviceName: String = ""
    /// One of the on-screen devices (VirtualDevices), not CoreMIDI.
    var isVirtual = false
    var looksLikeModule: Bool { !isVirtual && name.localizedCaseInsensitiveContains("alchemy") }
}

/// A plain MIDI port the app sends whole messages to: a controller on the
/// hub (RawMIDIPort) or an on-screen device (VirtualPort). ControllerHub
/// talks to both the same way.
protocol MIDIPortOut: AnyObject {
    func send(_ messages: [[UInt8]])
}

final class MIDIHub: @unchecked Sendable {
    private var client = MIDIClientRef()
    private var inPort = MIDIPortRef()
    private var outPort = MIDIPortRef()
    private let lock = NSLock()
    private var open: [MIDIUniqueID: MIDIConnection] = [:]
    /// Plain MIDI ports (controllers on the hub), by source unique ID.
    private var raw: [MIDIUniqueID: RawMIDIPort] = [:]

    /// Any MIDI setup change: a module plugged in, unplugged, rebooting into
    /// another firmware, or a Bluetooth adapter coming or going.
    var onSetupChanged: (() -> Void)?

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
        st = MIDIInputPortCreateWithBlock(client, "HostLink in" as CFString, &i) { [weak self] list, refCon in
            self?.receive(list, from: refCon)
        }
        guard st == noErr else { throw MIDIError(status: st, what: "create MIDI input") }
        inPort = i

        var o = MIDIPortRef()
        st = MIDIOutputPortCreate(client, "HostLink out" as CFString, &o)
        guard st == noErr else { throw MIDIError(status: st, what: "create MIDI output") }
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
        // The driver that made the device says how it is reached.
        let ent = entity(e)
        var dev = MIDIDeviceRef()
        if ent != 0, MIDIEntityGetDevice(ent, &dev) == noErr, dev != 0 {
            let owner = string(dev, kMIDIPropertyDriverOwner)
            if !owner.isEmpty { return LinkKind(driverOwner: owner) }
        }
        return LinkKind(driverOwner: string(e, kMIDIPropertyDriverOwner))
    }

    /// The destination that belongs with a source: the one on the same
    /// entity, else (virtual or odd devices) the one with the same name.
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

    /// Every device that can both send and receive, modules first.
    func devices() -> [MIDIDevice] {
        var out: [MIDIDevice] = []
        for i in 0..<MIDIGetNumberOfSources() {
            let s = MIDIGetSource(i)
            guard Self.destination(for: s) != nil else { continue }
            let name = Self.string(s, kMIDIPropertyDisplayName)
            guard !name.isEmpty else { continue }
            var d = MIDIDevice(id: Self.uniqueID(s), name: name, kind: Self.kind(s))
            let ent = Self.entity(s)
            var dev = MIDIDeviceRef()
            if ent != 0, MIDIEntityGetDevice(ent, &dev) == noErr, dev != 0 {
                d.deviceID = Self.uniqueID(dev)
                d.deviceName = Self.string(dev, kMIDIPropertyName)
            }
            out.append(d)
        }
        return out.sorted { ($0.looksLikeModule ? 0 : 1, $0.name) < ($1.looksLikeModule ? 0 : 1, $1.name) }
    }

    func isPresent(_ id: MIDIUniqueID) -> Bool { Self.source(id) != nil }

    // MARK: connections

    /// A HostLink byte pipe to one device. Many can be open at once.
    func connect(_ d: MIDIDevice) -> MIDIConnection? {
        guard let s = Self.source(d.id), let dst = Self.destination(for: s) else { return nil }
        lock.lock()
        if let old = open[d.id] { lock.unlock(); return old }
        let c = MIDIConnection(device: d, dest: dst, outPort: outPort)
        open[d.id] = c
        lock.unlock()
        // The refCon is the unique ID, so replies are sorted to the right
        // module even when both arrive in the same instant.
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
        let stillRaw = raw[id] != nil
        lock.unlock()
        if c != nil, !stillRaw, let s = Self.source(id) { MIDIPortDisconnectSource(inPort, s) }
    }

    /// A plain MIDI port to a device (a controller): its messages as they
    /// come, and plain MIDI out. `receive` is called on CoreMIDI's thread.
    func connectRaw(_ d: MIDIDevice, receive: @escaping ([UInt8]) -> Void) -> RawMIDIPort? {
        guard let s = Self.source(d.id), let dst = Self.destination(for: s) else { return nil }
        lock.lock()
        if let old = raw[d.id] { lock.unlock(); return old }
        let p = RawMIDIPort(device: d, dest: dst, outPort: outPort, receive: receive)
        raw[d.id] = p
        let alsoHostLink = open[d.id] != nil
        lock.unlock()
        if alsoHostLink { return p }   // the source is already connected; replies go to both
        let ref = UnsafeMutableRawPointer(bitPattern: Int(UInt32(bitPattern: d.id)) | 0x1_0000_0000)
        guard MIDIPortConnectSource(inPort, s, ref) == noErr else {
            lock.lock(); raw[d.id] = nil; lock.unlock()
            return nil
        }
        return p
    }

    func disconnectRaw(_ id: MIDIUniqueID) {
        lock.lock()
        let p = raw.removeValue(forKey: id)
        let stillOpen = open[id] != nil
        lock.unlock()
        if p != nil, !stillOpen, let s = Self.source(id) { MIDIPortDisconnectSource(inPort, s) }
    }

    private func receive(_ list: UnsafePointer<MIDIPacketList>, from refCon: UnsafeMutableRawPointer?) {
        guard let r = refCon else { return }
        let id = MIDIUniqueID(bitPattern: UInt32(truncatingIfNeeded: Int(bitPattern: r)))
        lock.lock()
        let c = open[id]
        let p = raw[id]
        lock.unlock()
        c?.receive(list)
        p?.receive(list)
    }
}

/// One device's HostLink pipe: SysEx out to its destination, SysEx in from
/// its source, decoded back to HostLink bytes.
final class MIDIConnection: ByteTransport, @unchecked Sendable {
    let device: MIDIDevice
    private let dest: MIDIEndpointRef
    private let outPort: MIDIPortRef
    private let lock = NSLock()
    private var decoder = SysExPipe.Decoder()
    private var handler: (([UInt8]) -> Void)?

    init(device: MIDIDevice, dest: MIDIEndpointRef, outPort: MIDIPortRef) {
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

/// One controller port: plain MIDI both ways (no HostLink).
final class RawMIDIPort: MIDIPortOut, @unchecked Sendable {
    let device: MIDIDevice
    private let dest: MIDIEndpointRef
    private let outPort: MIDIPortRef
    private let handler: ([UInt8]) -> Void

    init(device: MIDIDevice, dest: MIDIEndpointRef, outPort: MIDIPortRef, receive: @escaping ([UInt8]) -> Void) {
        self.device = device; self.dest = dest; self.outPort = outPort; self.handler = receive
    }

    /// Whole messages (channel messages or complete SysEx), as few packet
    /// lists as fit.
    func send(_ messages: [[UInt8]]) {
        guard !messages.isEmpty else { return }
        let size = 4096
        let buf = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { buf.deallocate() }
        let list = buf.bindMemory(to: MIDIPacketList.self, capacity: 1)
        var packet = MIDIPacketListInit(list)
        var pending = 0
        for msg in messages where !msg.isEmpty && msg.count < 1024 {
            var next: UnsafeMutablePointer<MIDIPacket>? = MIDIPacketListAdd(list, size, packet, 0, msg.count, msg)
            if next == nil {
                // Full: send what's there and start a new list.
                MIDISend(outPort, dest, list)
                packet = MIDIPacketListInit(list)
                pending = 0
                next = MIDIPacketListAdd(list, size, packet, 0, msg.count, msg)
            }
            guard let n = next else { continue }
            packet = n
            pending += 1
        }
        if pending > 0 { MIDISend(outPort, dest, list) }
    }

    fileprivate func receive(_ list: UnsafePointer<MIDIPacketList>) {
        var bytes: [UInt8] = []
        for packet in list.unsafeSequence() {
            let len = Int(packet.pointee.length)
            let data = UnsafeRawPointer(packet) + MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
            bytes += UnsafeBufferPointer(start: data.assumingMemoryBound(to: UInt8.self), count: len)
        }
        if !bytes.isEmpty { handler(bytes) }
    }
}

struct MIDIError: Error, CustomStringConvertible {
    var status: OSStatus
    var what: String
    var description: String { "Couldn't \(what) (\(status))" }
}
