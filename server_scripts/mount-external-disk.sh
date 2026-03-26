#!/bin/bash
set -e

echo "Setting up NVMe disk mount..."

# Check available block devices
lsblk

# Create a directory for mounting the disk
mkdir -p ssd

# Update packages and install required tools
sudo apt update
sudo apt install -y fdisk tmux htop nvme-cli

echo ""
echo "Next steps (manual):"
echo "1. Partition the disk: sudo fdisk /dev/nvme0n1"
echo "   - Type 'n' for new partition"
echo "   - Type 'p' for primary"
echo "   - Accept defaults"
echo "   - Type 'w' to write"
echo ""
echo "2. After partitioning, run the following commands:"
echo "   sudo mkfs.ext4 /dev/nvme0n1p1"
echo "   sudo mount /dev/nvme0n1p1 ssd"
echo "   cd ssd"
echo "   sudo chown \$USER ."
echo "   lsblk"
echo ""
echo "3. Configure Docker to wait for local filesystem:"

# Configure Docker to wait for local filesystem before starting
sudo mkdir -p /etc/systemd/system/docker.service.d
echo -e '[Unit]\nAfter=local-fs.target\nRequires=local-fs.target' | sudo tee /etc/systemd/system/docker.service.d/override.conf
sudo systemctl daemon-reload

# Verify Docker service dependencies
systemctl show docker.service -p After -p Requires

echo ""
echo "4. Add system binary paths to PATH:"
echo "   nano ~/.bashrc"
echo "   Add: export PATH=\"\$PATH:/usr/sbin:/sbin\""
echo "   Then: source ~/.bashrc"
echo ""
echo "5. Run generate_fstab.sh to make mount permanent"
