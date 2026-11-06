// swift-tools-version: 5.9
// The iPhone app's protocol core: HostLink over USB MIDI SysEx, the
// module's descriptor, and a pretend module. No UI, no CoreMIDI, so it
// builds and tests anywhere Swift runs:  swift test
import PackageDescription

let package = Package(
    name: "AlchemyLink",
    platforms: [.iOS(.v16), .macOS(.v13)],
    products: [.library(name: "AlchemyLink", targets: ["AlchemyLink"])],
    targets: [
        .target(name: "AlchemyLink"),
        .testTarget(name: "AlchemyLinkTests", dependencies: ["AlchemyLink"],
                    resources: [.copy("Resources/hlmidi_golden.json")]),
    ]
)
