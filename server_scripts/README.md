# Server Setup Scripts

This directory contains modular shell scripts for setting up a new server. Each script handles one specific task and can be run independently.

## Prerequisites

- Debian-based Linux server (Debian/Ubuntu)
- sudo privileges
- Internet connection

## Scripts Overview

| Script | Description | Parameters |
|--------|-------------|------------|
| `install-docker.sh` | Installs Docker and Docker Compose | None |
| `install-nginx.sh` | Installs and configures Nginx web server | None |
| `setup-ssl.sh` | Sets up Let's Encrypt SSL certificate | domain [www-domain] |
| `mount-external-disk.sh` | Prepares external disk mounting (requires manual steps) | None |
| `install-git.sh` | Installs Git | None |
| `clone-repo.sh` | Clones a Git repository | repository-url |
| `setup-ssh-keys.sh` | Generates SSH keys for Git authentication | email |
| `ops-agent.sh` | Installs Google Cloud Ops Agent for monitoring and logging | instance-name |

## Usage

### Make scripts executable
```bash
chmod +x *.sh
```

### Run individual scripts

```bash
# Install Docker
./install-docker.sh

# Install Nginx
./install-nginx.sh

# Setup SSL (replace with your domain)
./setup-ssl.sh example.com www.example.com

# Mount external disk (follow the manual instructions printed)
./mount-external-disk.sh

# Install Git
./install-git.sh

# Clone repository (replace with your repo URL)
./clone-repo.sh https://github.com/your-username/your-repo.git
# OR using SSH:
./clone-repo.sh git@github.com:your-username/your-repo.git

# Setup SSH keys (replace with your email)
./setup-ssh-keys.sh your.email@example.com

# Install Google Cloud Ops Agent (for GCP instances only)
./ops-agent.sh your-instance-name
```

## Typical Setup Order

1. **Install Docker** - `./install-docker.sh`
2. **Install Nginx** - `./install-nginx.sh`
3. **Setup SSL** - `./setup-ssl.sh your-domain.com`
4. **Mount External Disk** - `./mount-external-disk.sh` (follow manual steps)
5. **Install Git** - `./install-git.sh`
6. **Setup SSH Keys** - `./setup-ssh-keys.sh your@email.com`
7. **Clone Repository** - `./clone-repo.sh git@github.com:user/repo.git`

## Google Cloud Ops Agent (GCP Only)

The `ops-agent.sh` script is specifically for Google Cloud Platform instances. It automates the installation and configuration of Google Cloud Operations Agent for monitoring and logging.

### Prerequisites for ops-agent.sh

- Google Cloud SDK (`gcloud`) installed locally
- Active user authentication (not service account)
- `.env` file with `NDD_SERVER_ID` variable in the project root
- Appropriate GCP permissions for compute instances and logging

### What it does

1. Verifies user authentication with gcloud
2. Loads `NDD_SERVER_ID` from `.env` file
3. Automatically detects the instance zone
4. Installs Google Cloud Ops Agent on the target instance
5. Configures Docker log collection from `/var/lib/docker/containers`
6. Injects `NDD_SERVER_ID` label into all logs
7. Sets log retention to 180 days

### Usage Example

```bash
# Create .env file with your server ID
echo 'NDD_SERVER_ID="prod-server-001"' > .env

# Authenticate with gcloud
gcloud auth login

# Run the script
./ops-agent.sh your-instance-name
```

## Notes

- **SSL Setup**: Requires a valid domain pointing to your server
- **External Disk Mount**: Includes manual steps for disk partitioning. Follow the printed instructions carefully
- **Clone Repo**: Use HTTPS URL for first-time setup, SSH URL after setting up SSH keys
- **SSH Keys**: Copy the displayed public key to your Git provider (GitHub/GitLab/Bitbucket)
- **Ops Agent**: GCP-only script. Requires gcloud CLI and user authentication

## Checking Service Status

```bash
# Check if services are running
sudo systemctl status docker
sudo systemctl status nginx
```

## Additional Tools

- `generate_fstab.sh` - Use after mount-external-disk.sh to make external disk mount permanent across reboots
