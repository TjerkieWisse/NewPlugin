#!/bin/bash
# Installs KITNAP for the current user: VST3 to ~/.vst3, LV2 to ~/.lv2, the app to ~/.local/bin, and
# the AI models (shared by all of them) to ~/.config/KITNAP.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

mkdir -p ~/.vst3 ~/.lv2 ~/.local/share/kitnap ~/.local/bin
rm -rf ~/.vst3/KITNAP.vst3 ~/.lv2/KITNAP.lv2 ~/.local/share/kitnap/Standalone
cp -R "$here/VST3/KITNAP.vst3" ~/.vst3/
cp -R "$here/LV2/KITNAP.lv2" ~/.lv2/
cp -R "$here/Standalone" ~/.local/share/kitnap/
ln -sf ~/.local/share/kitnap/Standalone/KITNAP ~/.local/bin/kitnap
mkdir -p ~/.config/KITNAP
for model in kitnap_drums_sw.onnx kitnap_drumsep.onnx; do
    if [ -f "$here/$model" ]; then cp "$here/$model" ~/.config/KITNAP/; fi
done

echo "KITNAP installed. Rescan plugins in your DAW, or run 'kitnap' for the standalone app."
