#!/bin/bash
set -e

echo "Installing Docker..."

# Update packages
sudo apt update

# Install required packages
sudo apt install -y ca-certificates curl gnupg lsb-release

# Add Docker GPG key (old method, no keyrings)
curl -fsSL https://download.docker.com/linux/debian/gpg | sudo apt-key add -

# Add Docker repository
echo "deb [arch=$(dpkg --print-architecture)] https://download.docker.com/linux/debian $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list

# Update again
sudo apt update

# Install Docker
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# Enable and start Docker
sudo systemctl enable docker
sudo systemctl start docker

# Test installation
docker --version

echo "Docker installation complete!"
