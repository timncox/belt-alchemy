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
            supportedInterfaceOrientations: [.portrait, .landscapeLeft, .landscapeRight]
        )
    ],
    dependencies: [.package(path: "../AlchemyLink")],
    targets: [
        .executableTarget(name: "AppModule", dependencies: ["AlchemyLink"], path: "Sources")
    ]
)
