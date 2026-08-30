#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
label=com.azoria.displayd
ble_label=com.azoria.blebridge
install_dir="$HOME/Library/Application Support/AzoriaDisplay"
plist_path="$HOME/Library/LaunchAgents/$label.plist"
ble_plist_path="$HOME/Library/LaunchAgents/$ble_label.plist"
ble_app="$install_dir/Azoria BLE Bridge.app"
ble_binary="$ble_app/Contents/MacOS/AzoriaBLEBridge"
lg_hid_binary="$install_dir/lg-hid-control"
firmware_package="$install_dir/firmware-package"
python_path=$(command -v python3)

mkdir -p "$install_dir/azoria_displayd"
mkdir -p "$ble_app/Contents/MacOS"
mkdir -p "$HOME/Library/LaunchAgents"
mkdir -p "$firmware_package"
/usr/bin/rsync -a \
  --exclude '__pycache__' \
  "$script_dir/azoria_displayd/" "$install_dir/azoria_displayd/"
/usr/bin/install -m 644 "$script_dir/main.py" "$install_dir/main.py"
/usr/bin/install -m 600 "$script_dir/config.local.json" "$install_dir/config.local.json"
build_dir="$script_dir/../firmware/.pio/build/viewe_uedx48480040e_wb_a"
boot_app0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
if [[ -f "$build_dir/bootloader.bin" && -f "$build_dir/partitions.bin" && \
      -f "$build_dir/firmware.bin" && -f "$boot_app0" ]]; then
  /usr/bin/install -m 644 "$build_dir/bootloader.bin" "$firmware_package/bootloader.bin"
  /usr/bin/install -m 644 "$build_dir/partitions.bin" "$firmware_package/partitions.bin"
  /usr/bin/install -m 644 "$build_dir/firmware.bin" "$firmware_package/firmware.bin"
  /usr/bin/install -m 644 "$boot_app0" "$firmware_package/boot_app0.bin"
fi
/usr/bin/install -m 644 "$script_dir/ble-bridge/Info.plist" "$ble_app/Contents/Info.plist"
xcrun clang++ -std=c++17 -O2 \
  -framework IOKit \
  -framework CoreFoundation \
  "$script_dir/lg-hid-control/main.cpp" \
  -o "$lg_hid_binary"
xcrun swiftc -O \
  -framework CoreBluetooth \
  -framework CryptoKit \
  "$script_dir/ble-bridge/AzoriaBLEBridge.swift" \
  -o "$ble_binary"
/usr/bin/codesign --force --sign - "$ble_app"
sed \
  -e "s|__PYTHON__|$python_path|g" \
  -e "s|__MAIN__|$install_dir/main.py|g" \
  -e "s|__CONFIG__|$install_dir/config.local.json|g" \
  "$script_dir/launchd/$label.plist.template" > "$plist_path"
sed \
  -e "s|__BLE_BINARY__|$ble_binary|g" \
  -e "s|__CONFIG__|$install_dir/config.local.json|g" \
  "$script_dir/launchd/$ble_label.plist.template" > "$ble_plist_path"

plutil -lint "$plist_path"
plutil -lint "$ble_plist_path"
launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
launchctl bootout "gui/$(id -u)/$ble_label" 2>/dev/null || true
if ! launchctl bootstrap "gui/$(id -u)" "$plist_path"; then
  sleep 1
  launchctl bootstrap "gui/$(id -u)" "$plist_path"
fi
if ! launchctl bootstrap "gui/$(id -u)" "$ble_plist_path"; then
  sleep 1
  launchctl bootstrap "gui/$(id -u)" "$ble_plist_path"
fi
launchctl kickstart -k "gui/$(id -u)/$label"
launchctl kickstart -k "gui/$(id -u)/$ble_label"
echo "Installed and started $label and $ble_label"
