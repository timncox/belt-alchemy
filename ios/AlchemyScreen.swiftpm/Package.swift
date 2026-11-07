// swift-tools-version: 5.9
// Alchemy Screen: the iPhone as the Alchemy Lab's screen. Open this folder
// in Xcode (File > Open), pick your iPhone or a simulator, and Run. The
// protocol core lives next door in ../AlchemyLink.

import PackageDescription
import AppleProductTypes

let package = Package(
    name: "Alchemy Screen",
    platforms: [.iOS("16.0")],
    products: [
        .iOSApplication(
            name: "Alchemy Screen",
            targets: ["AppModule"],
            bundleIdentifier: "com.example.alchemyscreen",   // change to yours before signing
            displayVersion: "0.1",
            bundleVersion: "1",
            appIcon: .placeholder(icon: .bolt),
            accentColor: .presetColor(.orange),
            supportedDeviceFamilies: [.phone, .pad],
            supportedInterfaceOrientations: [.portrait, .landscapeLeft, .landscapeRight],
            capabilities: [
                .microphone(purposeString: "The mic can move the module's knobs by how loud it is and the note you sing."),
                .bluetoothAlways(purposeString: "Bluetooth connects to the MIDI adapters on your Alchemy Labs."),
                .motionSensors(purposeString: "Tilting and turning the phone can move the module's knobs, when you switch it on."),
                .localNetwork(purposeString: "To reach the Alchemy Lab module emulator on your Mac, for testing without a module.")
            ]
        )
    ],
    dependencies: [.package(path: "../AlchemyLink")],
    targets: [
        .executableTarget(name: "AppModule", dependencies: ["AlchemyLink"], path: "Sources",
                          resources: [.copy("Fonts")])
    ]
)
