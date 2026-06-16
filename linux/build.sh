#!/bin/sh
# Claude Code トークン監視 — Linux ネイティブアプリ (C + GTK3 + WebKitGTK) のビルド。
# 生成物: ./tokenmonitor
set -e
cd "$(dirname "$0")"

PKGS="gtk+-3.0 webkit2gtk-4.1 json-glib-1.0"
if ! pkg-config --exists webkit2gtk-4.1; then
  if pkg-config --exists webkit2gtk-4.0; then
    PKGS="gtk+-3.0 webkit2gtk-4.0 json-glib-1.0"
  else
    echo "エラー: webkit2gtk-4.1 / 4.0 が見つかりません。" >&2
    echo "  Debian/Ubuntu: sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev libjson-glib-dev" >&2
    echo "  Fedora:        sudo dnf install gtk3-devel webkit2gtk4.1-devel json-glib-devel" >&2
    echo "  Arch:          sudo pacman -S gtk3 webkit2gtk-4.1 json-glib" >&2
    exit 1
  fi
fi

echo "==> ビルド ($PKGS)"
${CC:-cc} -O2 -Wall $(pkg-config --cflags $PKGS) \
  -o tokenmonitor src/main.c src/stats.c \
  $(pkg-config --libs $PKGS)
echo "==> 完了: ./tokenmonitor"
