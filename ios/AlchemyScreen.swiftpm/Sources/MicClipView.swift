// Record a short clip with the phone's mic, listen back, and put it on the
// module's card, in the folder the Samples page is showing. 48 kHz mono
// 16-bit WAV, the plainest file a sample player reads. Belt itself plays no
// samples; this is for the firmwares that do. Over USB a 5 s clip takes a
// couple of seconds to send; over Bluetooth, about two minutes.

import AVFoundation
import SwiftUI
import AlchemyLink

struct MicClipView: View {
    @EnvironmentObject var store: ModuleStore
    @EnvironmentObject var inputs: Inputs
    @Environment(\.dismiss) private var dismiss
    let folder: String

    @StateObject private var rec = ClipRecorder()
    @State private var name = ClipRecorder.defaultName()

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack {
                        Text(String(format: "%.1f s", rec.seconds)).font(.system(.title, design: .rounded).monospacedDigit())
                        Spacer()
                        ProgressView(value: rec.level).frame(width: 120)
                    }
                    HStack(spacing: 16) {
                        if rec.isRecording {
                            Button { rec.stop() } label: { Label("Stop", systemImage: "stop.circle.fill") }.tint(.red)
                        } else {
                            Button { inputs.micOn = false; rec.start() } label: { Label("Record", systemImage: "record.circle") }.tint(.red)
                            if rec.hasClip {
                                Button { rec.play() } label: { Label("Listen", systemImage: "play.circle") }
                            }
                        }
                    }
                    .buttonStyle(.bordered)
                } footer: {
                    Text("Up to 30 seconds. Recording stops by itself at 30 s.")
                }
                if rec.hasClip {
                    Section {
                        TextField("Name", text: $name)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                        Button("Save to \(folder) on the card") {
                            guard let url = rec.export(named: name) else { return }
                            store.upload([url], into: folder)
                            dismiss()
                        }
                        .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                    } footer: {
                        Text("About \(estimate) over \(store.kind.label).")
                    }
                }
                if let p = rec.problem {
                    Section { Label(p, systemImage: "exclamationmark.triangle").foregroundStyle(.orange) }
                }
            }
            .navigationTitle("Record a clip")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { rec.stop(); dismiss() } } }
            .onDisappear { rec.stop() }
        }
    }

    private var estimate: String {
        let bytes = rec.seconds * 48_000 * 2 + 44
        let s = Int((bytes / store.kind.bytesPerSecond).rounded(.up))
        return s < 90 ? "\(max(1, s)) s" : "\(s / 60) min"
    }
}

@MainActor
final class ClipRecorder: NSObject, ObservableObject, AVAudioRecorderDelegate {
    @Published private(set) var isRecording = false
    @Published private(set) var seconds = 0.0
    @Published private(set) var level = 0.0
    @Published private(set) var hasClip = false
    @Published var problem: String?

    private var recorder: AVAudioRecorder?
    private var player: AVAudioPlayer?
    private var meter: Timer?
    private let file = FileManager.default.temporaryDirectory.appendingPathComponent("clip.wav")

    static func defaultName() -> String {
        let f = DateFormatter()
        f.dateFormat = "MMdd-HHmmss"
        return "mic-\(f.string(from: Date()))"
    }

    func start() {
        let session = AVAudioSession.sharedInstance()
        session.requestRecordPermission { ok in
            Task { @MainActor in
                guard ok else { self.problem = "Allow the microphone in Settings > Privacy > Microphone."; return }
                do {
                    try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker])
                    try session.setActive(true)
                    let settings: [String: Any] = [
                        AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: 48_000, AVNumberOfChannelsKey: 1,
                        AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false,
                    ]
                    let r = try AVAudioRecorder(url: self.file, settings: settings)
                    r.delegate = self
                    r.isMeteringEnabled = true
                    guard r.record(forDuration: 30) else { self.problem = "The mic didn't start."; return }
                    self.recorder = r
                    self.isRecording = true
                    self.hasClip = false
                    self.meter = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
                        Task { @MainActor in self?.tick() }
                    }
                } catch {
                    self.problem = "The mic didn't start: \(error.localizedDescription)"
                }
            }
        }
    }

    private func tick() {
        guard let r = recorder, r.isRecording else { return }
        r.updateMeters()
        seconds = r.currentTime
        level = Double(max(0, min(1, (r.averagePower(forChannel: 0) + 60) / 60)))
    }

    func stop() {
        player?.stop()
        guard isRecording else { return }
        recorder?.stop()
        finished()
    }

    nonisolated func audioRecorderDidFinishRecording(_ r: AVAudioRecorder, successfully ok: Bool) {
        Task { @MainActor in self.finished() }
    }

    private func finished() {
        meter?.invalidate(); meter = nil
        isRecording = false
        level = 0
        hasClip = FileManager.default.fileExists(atPath: file.path) && seconds > 0.1
    }

    func play() {
        player = try? AVAudioPlayer(contentsOf: file)
        player?.play()
    }

    /// The clip under its card name ("name.wav"), ready to send.
    func export(named n: String) -> URL? {
        let clean = n.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "/", with: "-")
        let dest = FileManager.default.temporaryDirectory.appendingPathComponent(clean.hasSuffix(".wav") ? clean : clean + ".wav")
        try? FileManager.default.removeItem(at: dest)
        do { try FileManager.default.copyItem(at: file, to: dest); return dest }
        catch { problem = "Couldn't prepare the file: \(error.localizedDescription)"; return nil }
    }
}
