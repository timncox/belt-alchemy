# Getting Alchemy Screen into TestFlight

TestFlight needs Xcode, signing and an upload under your Apple developer
account, so this part runs on your Mac. Everything it needs is in this
branch: an XcodeGen spec (`AlchemyScreen/project.yml`), an app icon, an
export options file and one script, `AlchemyScreen/testflight.sh`.

## Only you can supply

1. **Apple Developer Program membership** that is active, and your
   **team ID** (10 characters, under Membership details in your Apple
   developer account).
2. **A bundle ID you own**, for example `com.timcox.alchemyscreen`.
   Automatic signing registers it the first time you archive.
3. **An app record in App Store Connect** for that bundle ID (Apps, then
   the + button, then New App: a name, the bundle ID, a SKU, a language).
   The upload is refused until it exists.
4. **A way to sign in.** Either your Apple ID signed in to Xcode
   (Xcode > Settings > Accounts), or, for a scripted upload, an App Store
   Connect API key (Users and Access > Integrations, role App Manager or
   Admin): the .p8 file, its key ID and the issuer ID.

## The commands (zsh, on the Mac)

```zsh
# the branch
cd ~/path/to/belt-alchemy        # or: git clone https://github.com/timncox/belt-alchemy.git && cd belt-alchemy
git fetch origin claude/iphone-screen
git switch claude/iphone-screen

# tools: full Xcode (not just the command line tools) and XcodeGen
xcode-select -p
brew install xcodegen

# your values
export TEAM_ID=ABCDE12345
export BUNDLE_ID=com.timcox.alchemyscreen

# 1. the protocol core's tests
(cd ios/AlchemyLink && swift test)

# 2. a simulator build first; the app has never been compiled, so fix what it reports
cd ios/AlchemyScreen
xcodegen generate
xcodebuild -project AlchemyScreen.xcodeproj -scheme AlchemyScreen \
  -destination 'generic/platform=iOS Simulator' build

# 3. archive only (signs, sets a build number from the date and time)
DRY_RUN=1 ./testflight.sh

# 4. archive and upload to App Store Connect
./testflight.sh
# or with an API key instead of the Xcode sign-in:
ASC_KEY_PATH=~/keys/AuthKey_XXXXXXXXXX.p8 ASC_KEY_ID=XXXXXXXXXX ASC_ISSUER_ID=your-issuer-uuid ./testflight.sh
```

Each run uses a new build number (`YYYYMMDDHHMM`), so uploads never
clash; set `BUILD_NUMBER=` to choose one. The marketing version is 0.1 in
`project.yml`.

The script runs `xcodegen generate`, then `xcodebuild archive` with
`-allowProvisioningUpdates`, then `xcodebuild -exportArchive` with
`ExportOptions.plist` (method `app-store-connect`, destination `upload`).
On Xcode older than 15.3, change the method to `app-store`.

**Without the script:** after `xcodegen generate`, open
`AlchemyScreen.xcodeproj`, pick your team under Signing & Capabilities,
choose Any iOS Device, then Product > Archive, and in the Organizer
Distribute App > App Store Connect > Upload.

## After the upload

The build shows in App Store Connect under TestFlight once Apple finishes
processing it. Internal testers (people on your App Store Connect team)
can install it right away from the TestFlight app. External testers need
Apple's Beta App Review for the first build.

The app declares no encryption (`ITSAppUsesNonExemptEncryption` is NO),
so uploads don't stop at the export compliance question. It is iPhone
only for now.

**Later:** Xcode Cloud can do this on every push to the branch. In Xcode,
Product > Xcode Cloud > Create Workflow, with an archive action and a
TestFlight internal testing post-action. It still needs the same team,
bundle ID and app record.

## Prompt for Claude Code on the Mac

Paste this into Claude Code from the belt-alchemy folder:

> Check out the claude/iphone-screen branch of this repo. Read
> ios/TESTFLIGHT.md and follow it. My team ID is ABCDE12345 and my
> bundle ID is com.timcox.alchemyscreen. Run the AlchemyLink tests, then
> generate the Xcode project and build the app for the iOS Simulator.
> The SwiftUI app has never been compiled, so fix the compile errors in
> ios/AlchemyScreen.swiftpm/Sources, keeping the changes small and not
> touching the HostLink protocol or the firmware. Run it in the simulator
> and check the demo module on every tab. Then run testflight.sh with
> DRY_RUN=1. Stop and ask me before the real upload, and commit your
> fixes to the branch without pushing.
