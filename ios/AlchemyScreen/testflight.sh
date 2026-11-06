#!/bin/zsh
# Archive Alchemy Screen and upload it to App Store Connect, where it shows
# up in TestFlight after processing. Run from anywhere:
#
#   TEAM_ID=ABCDE12345 BUNDLE_ID=com.yourname.alchemyscreen ios/AlchemyScreen/testflight.sh
#
# Signing uses the Apple ID signed in to Xcode (Settings > Accounts). To
# upload with an App Store Connect API key instead, also set
#   ASC_KEY_PATH=~/keys/AuthKey_XXXX.p8 ASC_KEY_ID=XXXX ASC_ISSUER_ID=uuid
# Add DRY_RUN=1 to stop after the archive (no upload).
set -euo pipefail

: "${TEAM_ID:?set TEAM_ID to your Apple developer team ID}"
: "${BUNDLE_ID:?set BUNDLE_ID to a bundle ID you own, e.g. com.yourname.alchemyscreen}"
BUILD_NUMBER="${BUILD_NUMBER:-$(date +%Y%m%d%H%M)}"

cd "${0:A:h}"
command -v xcodegen >/dev/null || brew install xcodegen

export TEAM_ID BUNDLE_ID
xcodegen generate

auth=(-allowProvisioningUpdates)
if [[ -n "${ASC_KEY_PATH:-}" ]]; then
  auth+=(-authenticationKeyPath "${ASC_KEY_PATH:A}" -authenticationKeyID "$ASC_KEY_ID" -authenticationKeyIssuerID "$ASC_ISSUER_ID")
fi

rm -rf build/AlchemyScreen.xcarchive build/export
xcodebuild -project AlchemyScreen.xcodeproj -scheme AlchemyScreen \
  -configuration Release -destination 'generic/platform=iOS' \
  -archivePath build/AlchemyScreen.xcarchive \
  CURRENT_PROJECT_VERSION="$BUILD_NUMBER" \
  "${auth[@]}" archive

if [[ -n "${DRY_RUN:-}" ]]; then
  echo "Archived build $BUILD_NUMBER at build/AlchemyScreen.xcarchive (not uploaded)."
  exit 0
fi

xcodebuild -exportArchive -archivePath build/AlchemyScreen.xcarchive \
  -exportOptionsPlist ExportOptions.plist -exportPath build/export \
  "${auth[@]}"

echo "Uploaded build $BUILD_NUMBER. It appears in App Store Connect > TestFlight once Apple finishes processing."
