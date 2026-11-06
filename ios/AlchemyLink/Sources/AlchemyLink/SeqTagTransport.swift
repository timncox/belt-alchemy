// Two hosts on one module: the app and the AUv3 extension each open their
// own CoreMIDI connection to the same Alchemy Lab, and the module answers
// every request on the one USB MIDI stream, so both sides hear every reply.
// HostLinkClient keeps one request in flight and takes only the reply with
// its sequence number and type, but every client numbers from 1 upwards, so
// two of them would often wait on the same number at once.
//
// This wrapper gives one side its own half of the number space: requests go
// out with the top bit of the sequence number set, only replies with that
// bit set come back in (restored to the client's own number), and the rest
// (the other host's replies) are dropped here. The app's client counts up
// from 1 without the bit, so the two never collide until the app has sent
// 32 767 requests (about 35 minutes of polling at 15 a second); after that
// the app's numbers wrap into this half. The extension stays correct either
// way (it only takes replies to its own pending number, as before); a
// collision can only cost the app one mismatched reply.
//
// What tagging can't fix: the module runs one HostLink parser, so a frame
// longer than one SysEx message (140 bytes: descriptor chunks, files) from
// one host can be split by the other host's message and fail its CRC (a
// retry, not a corruption), and BLOB_BEGIN / DATA / COMMIT is one transfer
// at a time on the module, so writes of the running state from both hosts
// at the same instant can fail with a checksum or state error and be
// retried. See ios/PLAN.md, "AUv3".

import Foundation

public final class SeqTagTransport: ByteTransport, @unchecked Sendable {
    public static let tagBit: UInt16 = 0x8000

    public let inner: ByteTransport
    private let lock = NSLock()
    private var outSplit = FrameSplitter()
    private var inSplit = FrameSplitter()
    /// Wire number -> the client's own, for the last few requests.
    private var sent: [UInt16: UInt16] = [:]
    private var order: [UInt16] = []
    private var handler: (([UInt8]) -> Void)?
    /// Replies that belonged to the other host, for tests and diagnostics.
    public private(set) var dropped = 0

    public init(_ inner: ByteTransport) {
        self.inner = inner
        inner.setReceiveHandler { [weak self] bytes in self?.receive(bytes) }
    }

    public static func wireSeq(_ clientSeq: UInt16) -> UInt16 { (clientSeq & 0x7FFF) | tagBit }

    public func setReceiveHandler(_ h: @escaping ([UInt8]) -> Void) {
        lock.lock(); handler = h; lock.unlock()
    }

    public func send(_ bytes: [UInt8]) {
        var out: [UInt8] = []
        lock.lock()
        for b in bytes {
            outSplit.feed(b) { f in
                let w = Self.wireSeq(f.seq)
                if sent[w] == nil { order.append(w) }
                sent[w] = f.seq
                if order.count > 32 { sent[order.removeFirst()] = nil }
                out += Frame(type: f.type, seq: w, body: f.body).wire()
            }
        }
        lock.unlock()
        if !out.isEmpty { inner.send(out) }
    }

    private func receive(_ bytes: [UInt8]) {
        var out: [UInt8] = []
        lock.lock()
        for b in bytes {
            inSplit.feed(b) { f in
                guard f.seq & Self.tagBit != 0, let mine = sent[f.seq] else { dropped += 1; return }
                out += Frame(type: f.type, seq: mine, body: f.body).wire()
            }
        }
        let h = handler
        lock.unlock()
        if !out.isEmpty { h?(out) }
    }
}
