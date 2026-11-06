// HostLink over a plain MIDI byte stream: exactly the bytes that would cross
// USB MIDI (F0 7D 48 4C 01 <7-bit packed> F7 for HostLink, any other MIDI
// left alone), with no USB-MIDI event packets and no other framing. The
// module emulator's `--hostlink <port>` TCP socket carries this; the app's
// TCP wrapper (EmulatorLink, Network.framework) is a MIDIByteStream. Chunks
// may be split anywhere, even inside a SysEx message. No Network here, so
// it is tested on Linux.

import Foundation

/// A raw MIDI byte stream, both ways.
public protocol MIDIByteStream: AnyObject {
    /// MIDI bytes out, as they would cross the cable.
    func write(_ bytes: [UInt8])
    /// Called with bytes as they arrive, in order, split anywhere, from any thread.
    func setReadHandler(_ handler: @escaping ([UInt8]) -> Void)
}

/// A ByteTransport for HostLinkClient over a MIDIByteStream: HostLink bytes
/// out as the same SysEx messages MIDIConnection sends over CoreMIDI, and
/// the stream's SysEx decoded back to HostLink bytes with the same decoder.
public final class MIDIStreamLink: ByteTransport, @unchecked Sendable {
    public let stream: MIDIByteStream
    private let lock = NSLock()
    private var decoder = SysExPipe.Decoder()
    private var handler: (([UInt8]) -> Void)?

    public init(stream: MIDIByteStream) {
        self.stream = stream
        stream.setReadHandler { [weak self] bytes in self?.receive(bytes) }
    }

    /// HostLink bytes as the MIDI bytes that carry them.
    public static func encode(_ hostLink: [UInt8]) -> [UInt8] {
        SysExPipe.encodeStream(hostLink).flatMap { $0 }
    }

    public func setReceiveHandler(_ h: @escaping ([UInt8]) -> Void) {
        lock.lock(); handler = h; lock.unlock()
    }

    public func send(_ bytes: [UInt8]) {
        guard !bytes.isEmpty else { return }
        stream.write(Self.encode(bytes))
    }

    /// Bytes from the stream (any split); HostLink bytes go to the handler.
    public func receive(_ chunk: [UInt8]) {
        var raw: [UInt8] = []
        lock.lock()
        decoder.feed(chunk) { raw.append($0) }
        let h = handler
        lock.unlock()
        if !raw.isEmpty { h?(raw) }
    }

    /// SysEx messages decoded so far (for a status line).
    public var messages: Int { lock.lock(); defer { lock.unlock() }; return decoder.messages }
}

/// Where the module emulators listen: Left and Right on 127.0.0.1 by default
/// (the iOS Simulator shares the Mac's localhost; on a device, the Mac's address).
public struct EmulatorSettings: Codable, Equatable, Sendable {
    public var host = "127.0.0.1"
    public var leftPort: UInt16 = 5071
    public var rightPort: UInt16 = 5072
    public init() {}

    public var ports: [UInt16] { leftPort == rightPort ? [leftPort] : [leftPort, rightPort] }

    /// A port typed in a text field; nil unless 1...65535.
    public static func port(_ s: String) -> UInt16? {
        guard let v = Int(s.trimmingCharacters(in: .whitespaces)), (1...65535).contains(v) else { return nil }
        return UInt16(v)
    }
}
