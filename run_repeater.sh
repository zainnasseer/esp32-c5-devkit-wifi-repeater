#!/bin/bash

echo "======================================"
echo " ESP32-C5 Wi-Fi Repeater Auto Runner "
echo "======================================"

# ---- CONFIG ----
ESP_IDF_PATH="$HOME/esp/esp-idf"
PROJECT_PATH="$HOME/development/esp/wifi_repeater"
PORT=""   # leave empty for auto-detect, or set e.g. /dev/cu.usbserial-0001
# ----------------

# Export ESP-IDF environment
if [ -f "$ESP_IDF_PATH/export.sh" ]; then
    echo "[1/5] Exporting ESP-IDF environment..."
    source "$ESP_IDF_PATH/export.sh"
else
    echo "❌ ERROR: ESP-IDF export.sh not found!"
    exit 1
fi

# Go to project directory
echo "[2/5] Navigating to project directory..."
cd "$PROJECT_PATH" || exit 1

# Build
echo "[3/5] Building project..."
idf.py build || exit 1

# Flash
echo "[4/5] Flashing firmware..."
if [ -z "$PORT" ]; then
    idf.py flash || exit 1
else
    idf.py -p "$PORT" flash || exit 1
fi

# Monitor
echo "[5/5] Opening serial monitor..."
idf.py monitor

