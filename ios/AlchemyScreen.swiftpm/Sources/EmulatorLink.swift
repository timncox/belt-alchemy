// The Alchemy Lab module emulator on a Mac, as a module: its `--hostlink
// <port>` TCP socket carries the firmware's Phone-mode USB MIDI stream,
// raw, both ways (the SysEx that would cross the cable, nothing more). Each
// connection is a MIDIByteStream; MIDIStreamLink (AlchemyLink) turns it into
// the same HostLink bytes the USB path gives, so a ModuleStore can't tell
// the difference. The Simulator shares the Mac's localhost, so 127.0.0.1
// reaches it; on a device, use the Mac's address.
//
// Tries again every 2 s while not connected; the Rig makes a module when a
// connection opens and drops it when the connection goes.

import Foundation
import AlchemyLink

#if canImport(Network)
import Network

/// One open TCP connection, as plain MIDI bytes both ways.
final class TCPMIDIStream: MIDIByteStream, @unchecked Sendable {
    private let conn: NWConnection
    private let lock = NSLock()
    private var reader: (([UInt8]) -> Void)?
    /// The other end closed it or it failed (once, on the connection's queue).
    var onEnd: (() -> Void)?

    init(_ conn: NWConnection) { self.conn = conn }

    func write(_ bytes: [UInt8]) {
        guard !bytes.isEmpty else { return }
        conn.send(content: Data(bytes), completion: .contentProcessed { _ in })
    }

    func setReadHandler(_ h: @escaping ([UInt8]) -> Void) {
        lock.lock(); reader = h; lock.unlock()
    }

    func startReading() { next() }

    private func next() {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.lock.lock(); let r = self.reader; self.lock.unlock()
                r?([UInt8](data))
            }
            if isComplete || error != nil {
                let end = self.onEnd
                self.onEnd = nil
                end?()
                return
            }
            self.next()
        }
    }
}

/// Keeps one module emulator connected: one try now, then every 2 s while
/// it isn't. Callbacks come on the main queue.
final class EmulatorEndpoint: @unchecked Sendable {
    enum Status: Equatable {
        case connecting, connected
        case waiting(String)
    }

    let host: String
    let port: UInt16
    var onConnected: ((MIDIStreamLink) -> Void)?
    var onDisconnected: (() -> Void)?
    var onStatus: ((Status) -> Void)?

    private let queue: DispatchQueue
    private var conn: NWConnection?
    private var link: MIDIStreamLink?
    private var running = false
    /// Bumped whenever a connection is dropped, so its late callbacks are ignored.
    private var generation = 0
    static let retry: TimeInterval = 2

    init(host: String, port: UInt16) {
        self.host = host
        self.port = port
        queue = DispatchQueue(label: "emulator.\(port)")
    }

    func start() {
        queue.async { [self] in
            guard !running else { return }
            running = true
            attempt()
        }
    }

    func stop() {
        queue.async { [self] in
            running = false
            drop()
        }
    }

    /// Hang up and try again (the module stopped answering on an open socket).
    func restart() {
        queue.async { [self] in
            guard running else { return }
            lost("No answer from the module.")
        }
    }

    private func attempt() {
        guard running, conn == nil else { return }
        guard let p = NWEndpoint.Port(rawValue: port) else { report(.waiting("Port \(port) isn't valid.")); return }
        let params = NWParameters.tcp
        if let tcp = params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options { tcp.noDelay = true }
        let c = NWConnection(host: NWEndpoint.Host(host), port: p, using: params)
        generation += 1
        let gen = generation
        conn = c
        report(.connecting)
        c.stateUpdateHandler = { [weak self] st in self?.changed(st, gen) }
        c.start(queue: queue)
    }

    private func changed(_ st: NWConnection.State, _ gen: Int) {
        guard gen == generation, let c = conn else { return }
        switch st {
        case .ready:
            let s = TCPMIDIStream(c)
            s.onEnd = { [weak self] in
                guard let self else { return }
                self.queue.async { if gen == self.generation { self.lost("The emulator closed the connection.") } }
            }
            let l = MIDIStreamLink(stream: s)
            link = l
            s.startReading()
            report(.connected)
            let cb = onConnected
            DispatchQueue.main.async { cb?(l) }
        case let .waiting(e):
            // Refused (nothing listening yet): Network would wait for a
            // network change; we try again on our own clock.
            lost(e.localizedDescription)
        case let .failed(e):
            lost(e.localizedDescription)
        default:
            break
        }
    }

    private func lost(_ why: String) {
        drop()
        report(.waiting(why))
        queue.asyncAfter(deadline: .now() + Self.retry) { [weak self] in self?.attempt() }
    }

    private func drop() {
        generation += 1
        conn?.stateUpdateHandler = nil
        conn?.cancel()
        conn = nil
        if link != nil {
            link = nil
            let cb = onDisconnected
            DispatchQueue.main.async { cb?() }
        }
    }

    private func report(_ s: Status) {
        let cb = onStatus
        DispatchQueue.main.async { cb?(s) }
    }
}

#else

/// No Network framework here: never connects.
final class EmulatorEndpoint {
    enum Status: Equatable {
        case connecting, connected
        case waiting(String)
    }

    let host: String
    let port: UInt16
    var onConnected: ((MIDIStreamLink) -> Void)?
    var onDisconnected: (() -> Void)?
    var onStatus: ((Status) -> Void)?

    init(host: String, port: UInt16) { self.host = host; self.port = port }
    func start() { onStatus?(.waiting("Network isn't available on this platform.")) }
    func stop() {}
    func restart() {}
}

#endif

extension EmulatorEndpoint.Status {
    var label: String {
        switch self {
        case .connecting: return "Connecting…"
        case .connected: return "Connected"
        case let .waiting(why): return "Waiting (trying every 2 s): \(why)"
        }
    }
}
