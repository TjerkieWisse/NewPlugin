#!/bin/bash
# Installs KITNAP for the current user: VST3 to ~/.vst3, LV2 to ~/.lv2, the app to ~/.local/bin, and
# the AI models and runtime (shared by all of them) to ~/.config/KITNAP.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

mkdir -p ~/.vst3 ~/.lv2 ~/.local/share/kitnap ~/.local/bin
rm -rf ~/.vst3/KITNAP.vst3 ~/.lv2/KITNAP.lv2 ~/.local/share/kitnap/Standalone
cp -R "$here/VST3/KITNAP.vst3" ~/.vst3/
cp -R "$here/LV2/KITNAP.lv2" ~/.lv2/
cp -R "$here/Standalone" ~/.local/share/kitnap/
ln -sf ~/.local/share/kitnap/Standalone/KITNAP ~/.local/bin/kitnap
mkdir -p ~/.config/KITNAP
rm -f ~/.config/KITNAP/kitnap_drums.onnx ~/.config/KITNAP/kitnap_drumsep.onnx # models older versions used
for file in "$here"/kitnap_drums_sw.onnx "$here"/kitnap_drumsep5.onnx "$here"/libonnxruntime.so.*; do
    if [ -f "$file" ]; then cp "$file" ~/.config/KITNAP/; fi
done

echo "KITNAP installed. Rescan plugins in your DAW, or run 'kitnap' for the standalone app."
