#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_DIR="${SCRIPT_DIR}/build-v21/FeedbackSuppressor_artefacts/Release"

echo "=== Installing Feedback Suppressor V2.1 Plugins ==="

# 1. Install Audio Unit (AU)
if [ -d "${RELEASE_DIR}/AU/Feedback Suppressor V2.1.component" ]; then
    echo "Installing AU component..."
    mkdir -p ~/Library/Audio/Plug-Ins/Components
    rm -rf ~/Library/Audio/Plug-Ins/Components/"Feedback Suppressor V2.1.component"
    cp -R "${RELEASE_DIR}/AU/Feedback Suppressor V2.1.component" ~/Library/Audio/Plug-Ins/Components/
    echo "  [OK] Installed to ~/Library/Audio/Plug-Ins/Components/Feedback Suppressor V2.1.component"
fi

# 2. Install VST3
if [ -d "${RELEASE_DIR}/VST3/Feedback Suppressor V2.1.vst3" ]; then
    echo "Installing VST3 plugin..."
    if cp -R "${RELEASE_DIR}/VST3/Feedback Suppressor V2.1.vst3" ~/Library/Audio/Plug-Ins/VST3/ 2>/dev/null; then
        echo "  [OK] Installed to ~/Library/Audio/Plug-Ins/VST3/Feedback Suppressor V2.1.vst3"
    elif cp -R "${RELEASE_DIR}/VST3/Feedback Suppressor V2.1.vst3" /Library/Audio/Plug-Ins/VST3/ 2>/dev/null; then
        echo "  [OK] Installed to /Library/Audio/Plug-Ins/VST3/Feedback Suppressor V2.1.vst3"
    else
        echo "  [INFO] Installing to VST3 folder requires permissions."
        echo "  Run:"
        echo "    sudo cp -R \"${RELEASE_DIR}/VST3/Feedback Suppressor V2.1.vst3\" /Library/Audio/Plug-Ins/VST3/"
    fi
fi

# 3. Standalone App
if [ -d "${RELEASE_DIR}/Standalone/Feedback Suppressor V2.1.app" ]; then
    echo "Standalone Application:"
    echo "  -> ${RELEASE_DIR}/Standalone/Feedback Suppressor V2.1.app"
fi

echo "=== Installation Completed Successfully! ==="
