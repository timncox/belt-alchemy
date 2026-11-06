// One Belt module as the AUv3 extension's parameters see it: the twelve
// performance knobs and the HARD and Mute buttons (AUParams, params 0-13),
// written as the host moves them and read back on request. Foundation only,
// so it is tested against FakeModule on Linux; the extension owns the
// CoreMIDI transport and the timer.
//
// Writes are coalesced like the app's knob writes (ModuleStore.flush): one
// SET_LIVE in flight at a time, at most one per `controlPeriod`, and
// whatever the host moves meanwhile is merged into the next, so a fast
// automation lane never backs up. HARD and Mute are pressed (a tap toggles
// the latch) only when the wanted state differs from the module's last
// reported one.

import Foundation

public final class BeltParamDriver: @unchecked Sendable {
    public enum DriverError: Error, CustomStringConvertible {
        case notBelt(String)
        public var description: String {
            switch self { case .notBelt(let n): return "\(n) isn't running Belt" }
        }
    }

    public let client: HostLinkClient
    public let controlPeriod: Double
    /// How long a written value outranks a read that may have left before it.
    public var writeHold = 1.0
    /// A write the module refused or that timed out (from any thread).
    public var onError: ((Error) -> Void)?

    public private(set) var hello: Hello?
    public private(set) var descriptor: Descriptor?
    public private(set) var phone: PhoneInfo?
    public var unit: UInt32 { phone?.unit ?? 0 }

    private let lock = NSLock()
    private var live: [UInt8] = []
    private var pending: [Int: Float] = [:]
    private var flushing = false
    private var drained: [CheckedContinuation<Void, Never>] = []
    private var recent: [String: (value: Double, at: Double)] = [:]
    private var buttons: [Int: Bool] = [:]      // button -> on, as last read or pressed
    private var traceLast: UInt16?
    /// Writes sent (SET_LIVE and PRESS), for tests.
    public private(set) var writes = 0

    public init(client: HostLinkClient, controlPeriod: Double) {
        self.client = client
        self.controlPeriod = controlPeriod
    }

    private static func now() -> Double { ProcessInfo.processInfo.systemUptime }

    /// The lock, never held across an await.
    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    /// HELLO, the descriptor, Phone INFO and the running state. Throws for a
    /// module that doesn't answer or isn't running Belt.
    @discardableResult
    public func start() async throws -> UInt32 {
        var h: Hello?
        for attempt in 0..<3 where h == nil {
            do { h = try await client.sayHello() }
            catch { if attempt == 2 { throw error }; try? await Task.sleep(nanoseconds: 300_000_000) }
        }
        let d = try await client.descriptor()
        let knobIds = AUParams.specs.compactMap { $0.fieldId }
        // Belt by its fields: another firmware's p0.0 ... p1.5 would mean
        // other things, so it gets no parameters.
        guard knobIds.allSatisfy({ d.field($0) != nil }), d.fields.count >= knobIds.count
        else { throw DriverError.notBelt(h?.moduleName ?? "This module") }
        let p = try await client.phoneInfo()
        let bytes = try await client.getLive()
        locked { hello = h; descriptor = d; phone = p; live = bytes }
        return p?.unit ?? 0
    }

    // MARK: writing

    /// A host value for param 0 ... 13. Returns at once; the write follows.
    public func submit(param: Int, value: Float) {
        guard param >= 0, param < AUParams.perSlot else { return }
        let begin: Bool = locked {
            pending[param] = value
            if flushing { return false }
            flushing = true
            return true
        }
        if begin { Task { await self.drain() } }
    }

    /// Waits until every submitted value has been written (or failed).
    public func idle() async {
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            let done: Bool = locked {
                if !flushing && pending.isEmpty { return true }
                drained.append(c)
                return false
            }
            if done { c.resume() }
        }
    }

    private func drain() async {
        while true {
            let (batch, waiters): ([Int: Float], [CheckedContinuation<Void, Never>]) = locked {
                let b = pending
                pending = [:]
                guard b.isEmpty else { return (b, []) }
                flushing = false
                let w = drained
                drained = []
                return (b, w)
            }
            if batch.isEmpty {
                for w in waiters { w.resume() }
                return
            }
            let t0 = Self.now()
            await write(batch)
            let wait = controlPeriod - (Self.now() - t0)
            if wait > 0 { try? await Task.sleep(nanoseconds: UInt64(wait * 1e9)) }
        }
    }

    private func write(_ batch: [Int: Float]) async {
        let (desc, base) = locked { (descriptor, live) }
        guard let d = desc else { return }

        var changes: [String: Double] = [:]
        for (p, v) in batch {
            if let id = AUParams.specs[p].fieldId { changes[id] = AUParams.moduleValue(fromAU: v, param: p) }
        }
        if !changes.isEmpty {
            let bytes = d.encode(base, changes: changes)
            do {
                try await client.setLive(bytes)
                let t = Self.now()
                locked {
                    // Keep fields a read changed meanwhile; lay ours over them.
                    live = d.encode(live, changes: changes)
                    for (id, v) in changes { recent[id] = (v, t) }
                    writes += 1
                }
            } catch {
                onError?(error)
            }
        }

        for (p, v) in batch {
            guard let b = AUParams.specs[p].button else { continue }
            let want = v >= 0.5
            var known = locked { buttons[b] }
            if known == nil {
                _ = try? await readButtons()
                known = locked { buttons[b] }
            }
            guard known != want else { continue }
            do {
                try await client.press(button: b, .tap)
                locked { buttons[b] = want; writes += 1 }
            } catch {
                onError?(error)
            }
        }
    }

    // MARK: reading

    /// Every parameter's value as the module has it now (params 0 ... 13;
    /// HARD and Mute only from a firmware that answers STATE).
    public func read() async throws -> [Int: Float] {
        let bytes = try await client.getLive()
        let t = Self.now()
        let got: (Descriptor, [UInt8])? = locked {
            guard let d = descriptor else { return nil }
            recent = recent.filter { t - $0.value.at < writeHold }
            // A read that left before our last write would undo it: our
            // recent writes go back over it.
            let merged = d.encode(bytes, changes: recent.mapValues { $0.value })
            live = merged
            return (d, merged)
        }
        guard let g = got else { return [:] }
        let (d, merged) = g

        var out: [Int: Float] = [:]
        let values = d.decode(merged)
        for s in AUParams.specs {
            if let id = s.fieldId, let v = values[id] { out[s.param] = AUParams.auValue(fromModule: v, param: s.param) }
        }
        if let b = try await readButtons() {
            out[12] = b.hard ? 1 : 0
            out[13] = b.muted ? 1 : 0
        }
        return out
    }

    /// The module's own HARD and Mute (STATE), cached for the next press.
    @discardableResult
    private func readButtons() async throws -> BeltState? {
        let (hasState, since) = locked { (phone?.hasState == true, traceLast) }
        guard hasState else { return nil }
        let st = try await client.state(since: since)
        guard let b = st.belt else { return nil }
        locked {
            traceLast = b.traceLast
            buttons[0] = b.hard
            buttons[1] = b.muted
        }
        return b
    }
}
