#!/bin/bash
# Builds the macOS installer: KITNAP-<version>-macOS.pkg with VST3, AU, the standalone app and the AI
# models as separate, optional parts. The AI runtime goes in once, with the models, instead of inside
# each plugin: all of them find it in the shared folder.
#
# Usage: installer/mac/build-pkg.sh <version> [artefacts-folder] [output-folder] [ai-folder]
set -euo pipefail

version="$1"
artefacts="${2:-build/KITNAP_artefacts/Release}"
output="${3:-build/installer}"
ai="${4:-build/kitnap-ai}"
here="$(cd "$(dirname "$0")" && pwd)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$output"

# component <id> <bundle> <install location> [scripts folder]
component() {
    local id="$1" bundle="$2" location="$3" scripts="${4:-}"
    local root="$work/$id-root"
    mkdir -p "$root"
    cp -R "$bundle" "$root/"
    local copy="$root/$(basename "$bundle")"
    if [ -d "$copy/Contents/Resources" ]; then
        rm -f "$copy"/Contents/Resources/libonnxruntime*.dylib
        # A bundle sealed with its resources is signed again without the runtime.
        if [ -d "$copy/Contents/_CodeSignature" ]; then codesign --force --sign - "$copy"; fi
    fi

    # Stop the installer from "relocating" the bundle to wherever an older copy happens to live.
    pkgbuild --analyze --root "$root" "$work/$id.plist"
    local i=0
    while plutil -extract "$i" xml1 -o /dev/null "$work/$id.plist" 2>/dev/null; do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$work/$id.plist"
        i=$((i + 1))
    done

    # A plain folder (the model) has no bundles to describe.
    local extra=()
    if [ "$i" -gt 0 ]; then extra+=(--component-plist "$work/$id.plist"); fi
    if [ -n "$scripts" ]; then extra+=(--scripts "$scripts"); fi
    pkgbuild --root "$root" \
             --identifier "com.kitnapaudio.kitnap.$id" --version "$version" \
             --install-location "$location" ${extra[@]+"${extra[@]}"} "$work/$id.pkg"
}

component vst3 "$artefacts/VST3/KITNAP.vst3" "/Library/Audio/Plug-Ins/VST3"
component au "$artefacts/AU/KITNAP.component" "/Library/Audio/Plug-Ins/Components" "$here/au-scripts"
component app "$artefacts/Standalone/KITNAP.app" "/Applications"

# The AI models and runtime are shared by the plugins and the app: /Library/Application Support/KITNAP.
mkdir -p "$work/model-files/KITNAP"
cp "$ai/kitnap_drums_sw.onnx" "$ai/kitnap_drumsep5.onnx" "$work/model-files/KITNAP/"
cp "$artefacts"/VST3/KITNAP.vst3/Contents/Resources/libonnxruntime*.dylib "$work/model-files/KITNAP/"
component models "$work/model-files/KITNAP" "/Library/Application Support"

sed "s/@VERSION@/$version/g" "$here/distribution.xml" > "$work/distribution.xml"
productbuild --distribution "$work/distribution.xml" --resources "$here/resources" \
             --package-path "$work" "$output/KITNAP-$version-macOS.pkg"

echo "Built $output/KITNAP-$version-macOS.pkg"
