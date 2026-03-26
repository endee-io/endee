# Server Setup Guide

This guide contains commands to set up a new server with essential tools and configurations.

## 1. Install Docker

```bash
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
```

## 2. Install Nginx

```bash
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
```

## 3. Install SSL Certificate (Let's Encrypt)

```bash
# Install Certbot and Nginx plugin
sudo apt-get update
sudo apt-get install -y certbot python3-certbot-nginx

# Issue SSL certificate for your domain
# Replace 'your-domain.com' with your actual domain name
# Replace 'your-email@example.com' with your actual email
sudo certbot --nginx -d your-domain.com -d www.your-domain.com 

# Verify certificate auto-renewal is configured
sudo certbot renew --dry-run

# Check certificate status
sudo certbot certificates
or 
/etc/letsencrypt/live/yourdomain.com/

```

## 4. Mount External Disk (NVMe SSD)

```bash
# Check available block devices
lsblk

# Create a directory for mounting the disk
mkdir ssd

# Update packages and install required tools
sudo apt update
sudo apt install -y fdisk tmux htop nvme-cli

# Partition the NVMe disk (interactive - follow the prompts)
# Type 'n' for new partition, 'p' for primary, accept defaults, then 'w' to write
sudo fdisk /dev/nvme0n1

# Verify the partition was created
lsblk

# Format the partition with ext4 filesystem
sudo mkfs.ext4 /dev/nvme0n1p1

# Mount the partition to the ssd directory
sudo mount /dev/nvme0n1p1 ssd

# Navigate to the mounted directory
cd ssd

# Change ownership to current user
sudo chown $USER .

# Verify the disk is mounted
lsblk

# Recommended: To make the mount permanent across reboots, add to /etc/fstab
# Configure Docker to wait for local filesystem before starting
sudo mkdir -p /etc/systemd/system/docker.service.d
echo -e '[Unit]\nAfter=local-fs.target\nRequires=local-fs.target' | sudo tee /etc/systemd/system/docker.service.d/override.conf
sudo systemctl daemon-reload

# Verify Docker service dependencies
systemctl show docker.service -p After -p Requires

# Add system binary paths to PATH
nano ~/.bashrc
# Add the following line to the end of the file:
# export PATH="$PATH:/usr/sbin:/sbin"
# Then save and exit (Ctrl+X, Y, Enter)

# Reload bashrc
source ~/.bashrc

# Verify findfs is in PATH (output should be /usr/sbin/findfs)
which findfs

# Run the generate_fstab.sh script to automatically configure permanent mount
# ./generate_fstab.sh
# This above script will:
#   1. Display all available disks on the system
#   2. Prompt you to select which disk/partition to permanently mount
#   3. Automatically add the selected disk to /etc/fstab with proper UUID
#   4. Ensure the disk mounts automatically on system reboot

```

## 5. Install Git

```bash
# Update package index
sudo apt-get update

# Install Git
sudo apt-get install -y git

# Verify Git installation
git --version
```

## 6. Clone the Repository

```bash
# Navigate to the directory where you want to clone the repo
cd /home/$USER

# Clone the repository using HTTPS (for public repos or first-time setup)
git clone https://github.com/your-username/your-repo.git

# OR clone using SSH (requires SSH key setup - see section 7 below)
git clone git@github.com:your-username/your-repo.git

# Navigate into the cloned repository
cd your-repo

# Check repository status
git status
```

## 7. SSH Key Generation and Configuration

```bash
# Generate a new SSH key pair using RSA (use your email address)
# Press Enter to accept default file location (~/.ssh/id_rsa)
# Enter a secure passphrase when prompted (or leave empty for no passphrase)
ssh-keygen -t rsa -b 4096 -C "your.email@example.com"

# Start the ssh-agent in the background
eval "$(ssh-agent -s)"

# Add your SSH private key to the ssh-agent
ssh-add ~/.ssh/id_rsa

# Display your public key (copy this output)
cat ~/.ssh/id_rsa.pub
```

### Where to Add the SSH Key:

1. **GitHub/GitLab/Bitbucket:**
   - Copy the entire public key output from the `cat` command above
   - Go to your Git provider's website:
     - **GitHub**: Settings → SSH and GPG keys → New SSH key
     - **GitLab**: Preferences → SSH Keys → Add SSH Key
     - **Bitbucket**: Personal settings → SSH keys → Add key
   - Paste the public key and give it a descriptive title (e.g., "Production Server")
   - Save the key

2. **Test SSH Connection:**
   ```bash
   # Test GitHub connection
   ssh -T git@github.com
   ```

## Additional Useful Commands

```bash
# Check if services are running
sudo systemctl status docker
sudo systemctl status nginx
```
