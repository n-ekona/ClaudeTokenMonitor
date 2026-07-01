#!/bin/zsh
# Claude Code トークン監視 — macOS ネイティブアプリ (Swift + WKWebView) のビルド。
# 生成物: dist/ClaudeTokenMonitor.app
set -e
cd "$(dirname "$0")"

APP_NAME="ClaudeTokenMonitor"
DISP_NAME="Claude Code トークン監視"
BUNDLE_ID="jp.nekona.claudetokenmonitor"
VERSION="2.1.0"

DIST="dist"
APP="$DIST/$APP_NAME.app"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"

echo "==> クリーン"
rm -rf "$APP"
mkdir -p "$MACOS" "$RES/web"

echo "==> Swift コンパイル (universal: arm64 + x86_64)"
SRCS=(Sources/Stats.swift Sources/main.swift)
TMP_ARM="$(mktemp -t ctm_arm64)"
TMP_X64="$(mktemp -t ctm_x64)"
swiftc -O -target arm64-apple-macos11  -framework Cocoa -framework WebKit -o "$TMP_ARM" "${SRCS[@]}"
swiftc -O -target x86_64-apple-macos11 -framework Cocoa -framework WebKit -o "$TMP_X64" "${SRCS[@]}"
lipo -create "$TMP_ARM" "$TMP_X64" -output "$MACOS/$APP_NAME"
rm -f "$TMP_ARM" "$TMP_X64"
lipo -archs "$MACOS/$APP_NAME"

echo "==> リソース配置 (web/ + アプリアイコン)"
cp -R web/. "$RES/web/"
cp AppIcon.icns "$RES/AppIcon.icns"

echo "==> Info.plist 生成"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$DISP_NAME</string>
  <key>CFBundleDisplayName</key><string>$DISP_NAME</string>
  <key>CFBundleExecutable</key><string>$APP_NAME</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST

echo "==> 完了: $APP"
