#!/bin/bash
set -e

echo "Installing Git..."

# Update package index
sudo apt-get update

# Install Git
sudo apt-get install -y git

# Verify Git installation
git --version

echo "Git installation complete!"
