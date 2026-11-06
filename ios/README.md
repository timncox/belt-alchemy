# Alchemy Screen (iPhone)

The iPhone as the Alchemy Lab's screen, over the front USB-C in Belt's
Phone USB mode (HostLink carried in USB MIDI SysEx).

- `AlchemyLink/`: the protocol core in Swift, with tests (`swift test`).
- `AlchemyScreen.swiftpm/`: the SwiftUI app, openable in Xcode or Swift Playgrounds.
- `AlchemyScreen/`: an XcodeGen project around the same sources, for archiving.
- `TESTFLIGHT.md`: how to get it into TestFlight.

Not yet built in Xcode or run on a module.
