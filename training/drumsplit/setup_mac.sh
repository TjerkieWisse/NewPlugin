#!/bin/bash
# Sets up everything the drum split training needs, in .venv next to this script: Python 3.12 (via
# uv, which installs it without touching the system Python) and the packages in requirements.txt.
# Run it once from Terminal:  ./setup_mac.sh
set -e
cd "$(dirname "$0")"

if ! command -v uv >/dev/null 2>&1; then
    echo "Installing uv (a Python installer, into ~/.local/bin)..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
    export PATH="$HOME/.local/bin:$PATH"
fi

uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python -r requirements.txt

.venv/bin/python -c "import torch; print('PyTorch', torch.__version__, '- Apple GPU available:', torch.backends.mps.is_available())"
echo "Done. Next: .venv/bin/python fetch_data.py --groove-only"
