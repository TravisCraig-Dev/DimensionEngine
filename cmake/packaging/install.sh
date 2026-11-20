#!/bin/sh
# Installs Dimension Engine from this unpacked tarball.
#   ./install.sh            for you only: ~/.local/opt/dimension, a menu entry and ~/.local/bin/dimension-editor
#   sudo ./install.sh       for everyone: /opt/dimension, a menu entry and /usr/local/bin/dimension-editor
#   ./install.sh --uninstall  (or sudo ./install.sh --uninstall) takes it away again
# (Or run bin/DimensionEditor straight from here without installing.)
set -e
here=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" = "0" ]; then
    dest=/opt/dimension
    bindir=/usr/local/bin
    apps=/usr/share/applications
    icons=/usr/share/icons/hicolor/256x256/apps
else
    dest="$HOME/.local/opt/dimension"
    bindir="$HOME/.local/bin"
    apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
fi

if [ "$1" = "--uninstall" ]; then
    rm -rf "$dest"
    rm -f "$bindir/dimension-editor" "$apps/dimension-editor.desktop" "$icons/dimension-editor.png"
    echo "Dimension Engine removed from $dest (your projects are untouched)."
    exit 0
fi

echo "Installing Dimension Engine to $dest ..."
rm -rf "$dest"
mkdir -p "$dest" "$bindir" "$apps" "$icons"
cp -R "$here/bin" "$here/Samples" "$here/share" "$dest/"
ln -sf "$dest/bin/DimensionEditor" "$bindir/dimension-editor"
sed "s|/opt/dimension|$dest|g" "$here/share/applications/dimension-editor.desktop" > "$apps/dimension-editor.desktop"
cp "$here/share/icons/dimension-editor.png" "$icons/dimension-editor.png"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$apps" || true

echo "Done. Start it from your applications menu or with: dimension-editor"
command -v vulkaninfo >/dev/null 2>&1 || echo "Note: Dimension needs Vulkan (e.g. sudo apt install libvulkan1 mesa-vulkan-drivers)."
command -v zenity >/dev/null 2>&1 || echo "Note: file dialogs need zenity (e.g. sudo apt install zenity)."
command -v dotnet >/dev/null 2>&1 || echo "Note: C# scripts need the .NET 10 SDK (e.g. sudo apt install dotnet-sdk-10.0)."
