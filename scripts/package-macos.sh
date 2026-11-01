#!/bin/bash
# Builds the release ZIP: plug-ins, docs and the complete corresponding source (AGPL).
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(sed -n 's/^project(FeedbackSuppressor VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
name="Feedback Suppressor V2.1"
artefacts=build-v21/FeedbackSuppressor_artefacts/Release
mkdir -p dist
package_dir=$(mktemp -d "$PWD/dist/.package.XXXXXX")
trap 'rm -rf "$package_dir"' EXIT
release_dir="$package_dir/FeedbackSuppressorV2.1-$version"
mkdir -p "$release_dir/source/dependencies"
for bundle in "VST3/$name.vst3" "AU/$name.component" "Standalone/$name.app"; do
  /usr/bin/codesign --verify --deep --strict "$artefacts/$bundle"
  /usr/bin/ditto "$artefacts/$bundle" "$release_dir/$(basename "$bundle")"
done
cp README.md LICENSE THIRD_PARTY.md "$release_dir/"
cp -R docs licenses "$release_dir/"
git archive --format=tar HEAD | tar -x -C "$release_dir/source"
juce_dir=build-v21/_deps/juce-src
tar -czf "$release_dir/source/dependencies/JUCE-8.0.6.tar.gz" --exclude .git -C "$(dirname "$juce_dir")" "$(basename "$juce_dir")"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$release_dir" "$PWD/dist/FeedbackSuppressorV2.1-$version-macOS-universal.zip"
echo "Package: $PWD/dist/FeedbackSuppressorV2.1-$version-macOS-universal.zip"
