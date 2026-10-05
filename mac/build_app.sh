#!/bin/sh
# CapMeter.app (アドホック署名) を ./CapMeter.app に作る
set -e
cd "$(dirname "$0")"
swift build -c release
APP=CapMeter.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/CapMeter "$APP/Contents/MacOS/CapMeter"
cat > "$APP/Contents/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>容量計</string>
<key>CFBundleDisplayName</key><string>容量計</string>
<key>CFBundleIdentifier</key><string>local.capmeter</string>
<key>CFBundleExecutable</key><string>CapMeter</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
P
codesign --force --sign - "$APP"
echo "built: $(pwd)/$APP"
