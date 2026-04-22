#!/bin/bash
set -e

# Usage: ./install-nginx.sh <domain>
# Example: ./install-nginx.sh dev.endee.io

if [ -z "$1" ]; then
  echo "Usage: $0 <domain>"
  echo "Example: $0 dev.endee.io"
  exit 1
fi

DOMAIN=$1

echo "Installing Nginx..."

# Update package index
sudo apt-get update

# Install Nginx web server
sudo apt-get install -y nginx

# Start Nginx service
sudo systemctl start nginx

# Enable Nginx to start on boot
sudo systemctl enable nginx

# Check Nginx status
sudo systemctl status nginx --no-pager

echo "export PATH='$PATH:/usr/sbin'" >> ~/.bashrc

# Verify Nginx installation (should show Nginx version)
nginx -v

echo "Nginx installation complete!"
