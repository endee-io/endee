#!/usr/bin/env bash

# Box AI Engine: VSCode Extension Build Script (UNIX)
set -e

echo "[Box] Compiling VSCode Extension..."

# Check for npm
if ! command -v npm &> /dev/null; then
    echo "[!] npm not found. Please install Node.js (https://nodejs.org)."
    exit 1
fi

# Install dependencies
echo "[Box] Installing Node.js dependencies..."
npm install

# Compile
echo "[Box] Building TypeScript..."
npm run compile

echo ""
echo "[Box] Extension built successfully!"
echo "[Box] You can now load this folder into VSCode."
echo ""
