#!/bin/bash
set -e

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
sudo systemctl status nginx

# Verify Nginx installation (should show Nginx version)
nginx -v

echo "Nginx installation complete!"
