#!/bin/bash
# Installs Feedback Suppressor V3 into the current user's plug-in folders.
# Coexists with V2 (different plug-in code / bundle id).
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAME="Feedback Suppressor V3"

# Prefer the V3 build directory, fall back to ./build
RELEASE_DIR=""
for dir in "${SCRIPT_DIR}/build-v3" "${SCRIPT_DIR}/build"; do
    if [ -d "${dir}/FeedbackSuppressor_artefacts/Release/VST3/${NAME}.vst3" ]; then
        RELEASE_DIR="${dir}/FeedbackSuppressor_artefacts/Release"
        break
    fi
done
if [ -z "${RELEASE_DIR}" ]; then
    echo "Build not found. Run: cmake -S . -B build-v3 -DCMAKE_BUILD_TYPE=Release && cmake --build build-v3"
    exit 1
fi

echo "=== Installing ${NAME} from ${RELEASE_DIR} ==="

if [ -d "${RELEASE_DIR}/VST3/${NAME}.vst3" ]; then
    mkdir -p ~/Library/Audio/Plug-Ins/VST3
    rm -rf ~/Library/Audio/Plug-Ins/VST3/"${NAME}.vst3"
    cp -R "${RELEASE_DIR}/VST3/${NAME}.vst3" ~/Library/Audio/Plug-Ins/VST3/
    echo "  [OK] ~/Library/Audio/Plug-Ins/VST3/${NAME}.vst3"
fi

if [ -d "${RELEASE_DIR}/AU/${NAME}.component" ]; then
    mkdir -p ~/Library/Audio/Plug-Ins/Components
    rm -rf ~/Library/Audio/Plug-Ins/Components/"${NAME}.component"
    cp -R "${RELEASE_DIR}/AU/${NAME}.component" ~/Library/Audio/Plug-Ins/Components/
    echo "  [OK] ~/Library/Audio/Plug-Ins/Components/${NAME}.component"
fi

if [ -d "${RELEASE_DIR}/Standalone/${NAME}.app" ]; then
    echo "Standalone application: ${RELEASE_DIR}/Standalone/${NAME}.app"
fi

echo "=== Done. Restart Cubase (or rescan in the VST Plug-in Manager). ==="
