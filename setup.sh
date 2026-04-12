#!/usr/bin/env bash

# Box AI Engine: Universal UNIX Setup (Linux/macOS/BSD)
set -e

echo "=========================================="
echo "  📦 Box AI Engine: Master UNIX Setup"
echo "=========================================="
echo ""

# 1. Install Requirements
echo "[1/4] Installing Python dependencies..."
python3 -m pip install -r "$(dirname "$0")/box/requirements.txt"

# 2. Setup PATH Persistence
echo "[2/4] Configuring Shell PATH..."
BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
SHELL_NAME=$(basename "$SHELL")
PROFILE_FILE=""

case "$SHELL_NAME" in
    bash) PROFILE_FILE="$HOME/.bashrc" ;;
    zsh)  PROFILE_FILE="$HOME/.zshrc" ;;
    *)    PROFILE_FILE="$HOME/.profile" ;;
esac

# Check if already in file
if ! grep -q "$BIN_DIR" "$PROFILE_FILE" 2>/dev/null; then
    echo "Adding Box to $PROFILE_FILE ..."
    echo "" >> "$PROFILE_FILE"
    echo "# Box AI Engine" >> "$PROFILE_FILE"
    echo "export PATH=\"\$PATH:$BIN_DIR\"" >> "$PROFILE_FILE"
    echo "Success: Box added to your PATH."
else
    echo "Box is already in your shell profile."
fi

# 3. Initial Indexing
echo "[3/4] Initializing Codebase Index (Hybrid Search)..."
./box.sh index || echo "[!] Indexing failed. Ensure the Endee server is running on 8080."

# 4. Finalizing
echo "[4/4] Finalizing setup..."
echo ""
echo "=========================================="
echo "  🎉 Setup Complete!"
echo "=========================================="
echo ""
echo "1. IMPORTANT: Run 'source $PROFILE_FILE' or RESTART your terminal."
echo "2. Run './box.sh serve' to start the Intelligence brain."
echo "3. Open VSCode and use the Box Sidebar."
echo ""
