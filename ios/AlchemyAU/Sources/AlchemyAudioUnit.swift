// The Audio Unit: a MIDI processor ('aumi') that hosts such as AUM, Loopy
// Pro and Drambo load to automate one or two Alchemy Labs running Belt.
//
// - Parameters: Module A and Module B, each with Belt's twelve performance
//   knobs and HARD and Mute. Fixed addresses (slot << 8 | param), described
//   in AlchemyLink/AUParams.swift and tested there.
// - The host's changes go to the modules (ModuleBridge, BeltParamDriver),
//   coalesced so a fast automation lane never backs up. Values read back
//   from the modules every ~400 ms update the parameters, except those the
//   host changed in the last second, so a read never fights automation.
// - MIDI: everything that comes in goes straight out to the host's MIDI
//   output (midiOutputEventBlock), and these CCs (any channel) also move
//   parameters:
//       CC 20 Key    21 Scale    22 Retune   23 Amount   24 Harmony
//       CC 25 Formant  26-29 Voice 1-4  30 Doubler  31 Spread
//       CC 32 HARD (64+ = on)  33 Mute (64+ = on)          -> Module A
//       CC 52 ... 65: the same fourteen, in the same order -> Module B
//   A CC sweeps the whole range; an indexed knob (Key, Scale, Voices) takes
//   one of its n equal zones.
// - Audio: none. Hosts differ on whether a MIDI processor needs an audio
//   bus, so, as Apple's template does, it has one stereo output that
//   renders silence.
//
// Threads: the render block touches only ParamMailbox (fixed buffers made
// at init) and RenderState (set in allocateRenderResources); no locks, no
// allocation, no Swift arrays or dictionaries. A 50 Hz timer on the main
// queue (the pump) takes what the render thread wrote and passes it on.
//
// Known limit: the app and this extension can both hold a module and both
// send HostLink. Replies carry sequence numbers and this side tags its own
// (SeqTagTransport), so each ignores the other's replies, but polling
// doubles and simultaneous writes can fail once and be retried. Close the
// app's connection to the module while a host uses the extension.

import AudioToolbox
import AVFoundation
import CoreAudioKit
import Foundation
import AlchemyLink

/// Values per parameter in fixed buffers, so the render thread can write
/// without locking or allocating. Plain aligned 32-bit loads and stores
/// (atomic on arm64); a value the pump reads mid-change is simply read
/// again on the next tick, because the generation number moved.
final class ParamMailbox: @unchecked Sendable {
    let count = AUParams.count
    /// What each parameter holds: read by implementorValueProvider.
    let values: UnsafeMutablePointer<AUValue>
    /// Host parameter events from the render thread (already in `values`):
    /// bumped per event, so the pump knows to send them to the module.
    let hostGen: UnsafeMutablePointer<UInt32>
    /// CC moves from the render thread, not yet in `values`: the pump sets
    /// the parameter, so the host sees the move too.
    let ccValue: UnsafeMutablePointer<AUValue>
    let ccGen: UnsafeMutablePointer<UInt32>
    /// A readback on its way round through implementorValueObserver (NaN
    /// when none), so it isn't mistaken for the host's own change.
    let echo: UnsafeMutablePointer<AUValue>
    /// CC number -> parameter index or -1, and each parameter's shape.
    let ccIndex: UnsafeMutablePointer<Int32>
    let steps: UnsafeMutablePointer<Int32>
    let lo: UnsafeMutablePointer<AUValue>
    let hi: UnsafeMutablePointer<AUValue>

    init() {
        values = .allocate(capacity: count)
        hostGen = .allocate(capacity: count)
        ccValue = .allocate(capacity: count)
        ccGen = .allocate(capacity: count)
        echo = .allocate(capacity: count)
        steps = .allocate(capacity: count)
        lo = .allocate(capacity: count)
        hi = .allocate(capacity: count)
        ccIndex = .allocate(capacity: 128)
        for i in 0..<count {
            let s = AUParams.specs[i % AUParams.perSlot]
            values[i] = s.defaultValue
            ccValue[i] = s.defaultValue
            hostGen[i] = 0
            ccGen[i] = 0
            echo[i] = .nan
            steps[i] = Int32(s.steps)
            lo[i] = s.minValue
            hi[i] = s.maxValue
        }
        for (cc, i) in AUParams.ccIndexTable().enumerated() { ccIndex[cc] = Int32(i) }
    }

    deinit {
        values.deallocate(); hostGen.deallocate(); ccValue.deallocate(); ccGen.deallocate()
        echo.deallocate(); steps.deallocate(); lo.deallocate(); hi.deallocate(); ccIndex.deallocate()
    }

    /// The dense index for an address, -1 outside the scheme (the same
    /// arithmetic as AUParams.index(of:), without optionals or tuples).
    @inline(__always) func index(_ address: AUParameterAddress) -> Int {
        let slot = Int(truncatingIfNeeded: address >> 8), p = Int(truncatingIfNeeded: address & 0xFF)
        guard address >> 8 < UInt64(AUParams.slots), p < AUParams.perSlot else { return -1 }
        return slot * AUParams.perSlot + p
    }

    // Render thread only:

    @inline(__always) func renderParameter(_ address: AUParameterAddress, _ v: AUValue) {
        let i = index(address)
        guard i >= 0 else { return }
        values[i] = min(hi[i], max(lo[i], v))
        hostGen[i] &+= 1
    }

    @inline(__always) func renderCC(_ cc: UInt8, _ value: UInt8) {
        let i = Int(ccIndex[Int(cc & 0x7F)])
        guard i >= 0 else { return }
        ccValue[i] = AUParams.auValue(fromCC: value, steps: Int(steps[i]), min: lo[i], max: hi[i])
        ccGen[i] &+= 1
    }
}

/// What the render block needs beyond the mailbox, set outside rendering.
final class RenderState: @unchecked Sendable {
    var midiOut: AUMIDIOutputEventBlock?
    /// Zeros for output buffers the host left without memory.
    private(set) var silence: UnsafeMutablePointer<Float>?
    private(set) var frames = 0
    private(set) var channels = 0

    func allocate(frames f: Int, channels c: Int) {
        free()
        frames = max(1, f); channels = max(1, c)
        let p = UnsafeMutablePointer<Float>.allocate(capacity: frames * channels)
        p.initialize(repeating: 0, count: frames * channels)
        silence = p
    }

    func free() {
        silence?.deallocate()
        silence = nil
        frames = 0; channels = 0
    }

    deinit { free() }
}

public final class AlchemyAudioUnit: AUAudioUnit {
    let mailbox = ParamMailbox()
    let bridge = ModuleBridge()
    private let render = RenderState()
    private var outputBusArray: AUAudioUnitBusArray!
    private var pumpTimer: DispatchSourceTimer?
    private var hostSeen = [UInt32](repeating: 0, count: AUParams.count)
    private var ccSeen = [UInt32](repeating: 0, count: AUParams.count)

    /// Slot statuses for the view (main queue), kept so a view opened later
    /// starts from the latest.
    private(set) var slotStatus: [SlotStatus] = [SlotStatus(), SlotStatus()]
    var onSlotStatus: (([SlotStatus]) -> Void)?

    public override init(componentDescription: AudioComponentDescription,
                         options: AudioComponentInstantiationOptions = []) throws {
        try super.init(componentDescription: componentDescription, options: options)

        let format = AVAudioFormat(standardFormatWithSampleRate: 44_100, channels: 2)!
        let bus = try AUAudioUnitBus(format: format)
        outputBusArray = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [bus])

        parameterTree = Self.makeTree()
        wireTree()

        let box = mailbox
        bridge.currentValue = { address in
            let i = box.index(address)
            return i >= 0 ? box.values[i] : nil
        }
        bridge.onReadback = { [weak self] values in self?.applyReadback(values) }
        bridge.onStatus = { [weak self] s in
            self?.slotStatus = s
            self?.onSlotStatus?(s)
        }
    }

    deinit {
        pumpTimer?.cancel()
        bridge.stop()
    }

    // MARK: parameters

    static func makeTree() -> AUParameterTree {
        let groups: [AUParameterGroup] = (0..<AUParams.slots).map { slot in
            let params: [AUParameter] = (0..<AUParams.perSlot).map { p in
                let a = AUParams.address(slot: slot, param: p)
                let s = AUParams.specs[p]
                let unit: AudioUnitParameterUnit
                var strings: [String]?
                switch s.kind {
                case .indexed(let labels): unit = .indexed; strings = labels
                case .percent: unit = .percent
                case .bipolar: unit = .generic
                case .toggle: unit = .boolean
                }
                let flags: AudioUnitParameterOptions = [.flag_IsReadable, .flag_IsWritable, .flag_ValuesHaveStrings]
                return AUParameterTree.createParameter(
                    withIdentifier: AUParams.identifier(a)!, name: AUParams.displayName(a)!, address: a,
                    min: s.minValue, max: s.maxValue, unit: unit, unitName: nil,
                    flags: flags, valueStrings: strings, dependentParameters: nil)
            }
            return AUParameterTree.createGroup(withIdentifier: AUParams.slotKeys[slot],
                                               name: AUParams.slotNames[slot], children: params)
        }
        return AUParameterTree.createTree(withChildren: groups)
    }

    private func wireTree() {
        guard let tree = parameterTree else { return }
        let box = mailbox
        let bridge = self.bridge

        // The host (or this extension's view, or the pump passing on a CC)
        // set a value. A readback coming round is recognised by `echo`.
        tree.implementorValueObserver = { param, value in
            let i = box.index(param.address)
            guard i >= 0 else { return }
            let e = box.echo[i]
            box.echo[i] = .nan
            box.values[i] = value
            if e == value { return }
            bridge.hostSet(param.address, value)
        }

        tree.implementorValueProvider = { param in
            let i = box.index(param.address)
            return i >= 0 ? box.values[i] : 0
        }

        tree.implementorStringFromValueCallback = { param, valuePtr in
            let i = box.index(param.address)
            let v = valuePtr?.pointee ?? (i >= 0 ? box.values[i] : 0)
            return AUParams.valueString(param.address, v)
        }

        tree.implementorValueFromStringCallback = { param, text in
            let t = text.trimmingCharacters(in: .whitespaces)
            if let i = param.valueStrings?.firstIndex(where: { $0.caseInsensitiveCompare(t) == .orderedSame }) {
                return AUValue(i)
            }
            switch t.lowercased() {
            case "on": return 1
            case "off": return 0
            default: break
            }
            let digits = t.replacingOccurrences(of: "%", with: "").replacingOccurrences(of: "+", with: "")
                .trimmingCharacters(in: .whitespaces)
            return AUValue(digits).map { min(param.maxValue, max(param.minValue, $0)) } ?? param.value
        }
    }

    /// Values the modules hold that the host isn't moving (main queue).
    private func applyReadback(_ values: [UInt64: Float]) {
        guard let tree = parameterTree else { return }
        for (a, v) in values {
            let i = mailbox.index(a)
            guard i >= 0, let p = tree.parameter(withAddress: a) else { continue }
            mailbox.echo[i] = v
            // Tells the host and the view; implementorValueObserver stores it
            // and, seeing the echo, doesn't send it back to the module.
            p.setValue(v, originator: nil)
            mailbox.echo[i] = .nan
            mailbox.values[i] = v
        }
    }

    /// The pump: what the render thread wrote, passed on (main queue, 50 Hz).
    private func pump() {
        guard let tree = parameterTree else { return }
        for i in 0..<mailbox.count {
            let a = AUParams.address(index: i)
            let hg = mailbox.hostGen[i]
            if hg != hostSeen[i] {
                hostSeen[i] = hg
                // Automation the host scheduled: it knows; tell the module.
                bridge.hostSet(a, mailbox.values[i])
            }
            let cg = mailbox.ccGen[i]
            if cg != ccSeen[i] {
                ccSeen[i] = cg
                // A CC: set the parameter so the host and the view see it;
                // implementorValueObserver stores it and sends it to the
                // module. Should a host not call the observer for this, the
                // check after does both.
                let v = mailbox.ccValue[i]
                tree.parameter(withAddress: a)?.setValue(v, originator: nil)
                if mailbox.values[i] != v {
                    mailbox.values[i] = v
                    bridge.hostSet(a, v)
                }
            }
        }
    }

    private func startPump() {
        guard pumpTimer == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + 0.02, repeating: 0.02, leeway: .milliseconds(5))
        t.setEventHandler { [weak self] in self?.pump() }
        t.resume()
        pumpTimer = t
    }

    private func stopPump() {
        pumpTimer?.cancel()
        pumpTimer = nil
    }

    /// Starts looking for modules: from allocateRenderResources, or when the
    /// view opens (a host that only scans plug-ins opens no MIDI).
    func startModules() { bridge.start() }

    // MARK: buses and MIDI

    public override var outputBusses: AUAudioUnitBusArray { outputBusArray }

    public override var channelCapabilities: [NSNumber]? { [0, 2] }

    public override var midiOutputNames: [String] { ["Alchemy Screen"] }

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
        render.allocate(frames: Int(maximumFramesToRender), channels: Int(outputBusArray[0].format.channelCount))
        // Apple: read midiOutputEventBlock here and keep it for rendering.
        render.midiOut = midiOutputEventBlock
        DispatchQueue.main.async { [weak self] in self?.startPump() }
        startModules()
    }

    public override func deallocateRenderResources() {
        render.midiOut = nil
        render.free()
        DispatchQueue.main.async { [weak self] in self?.stopPump() }
        super.deallocateRenderResources()
    }

    public override var internalRenderBlock: AUInternalRenderBlock {
        let box = mailbox
        let state = render
        let dataOffset = MemoryLayout<AUMIDIEvent>.offset(of: \AUMIDIEvent.data)!
        return { actionFlags, _, frameCount, _, outputData, realtimeEventListHead, _ in
            // Silence on every output channel.
            let abl = UnsafeMutableAudioBufferListPointer(outputData)
            let bytes = Int(frameCount) * MemoryLayout<Float>.size
            for k in 0..<abl.count {
                if let p = abl[k].mData {
                    memset(p, 0, min(bytes, Int(abl[k].mDataByteSize)))
                } else if let s = state.silence, Int(frameCount) <= state.frames {
                    abl[k].mData = UnsafeMutableRawPointer(s + (k % state.channels) * state.frames)
                    abl[k].mDataByteSize = UInt32(bytes)
                }
            }
            actionFlags.pointee.insert(.unitRenderAction_OutputIsSilence)

            var event = realtimeEventListHead
            while let e = event {
                switch e.pointee.head.eventType {
                case .parameter, .parameterRamp:
                    // A ramp goes straight to its end value: the module is
                    // written at control rate anyway.
                    let p = e.pointee.parameter
                    box.renderParameter(p.parameterAddress, p.value)
                case .MIDI, .midiSysEx:
                    let m = e.pointee.MIDI
                    // The bytes start at data[0] and, for SysEx, run on past
                    // the struct for `length` bytes.
                    let raw = (UnsafeRawPointer(e) + dataOffset).assumingMemoryBound(to: UInt8.self)
                    _ = state.midiOut?(m.eventSampleTime, m.cable, Int(m.length), raw)
                    if e.pointee.head.eventType == .MIDI, m.length >= 3, m.data.0 & 0xF0 == 0xB0 {
                        box.renderCC(m.data.1, m.data.2)
                    }
                default:
                    break
                }
                event = UnsafePointer(e.pointee.head.next)
            }
            return noErr
        }
    }
}
