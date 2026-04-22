# Server Setup Scripts

This directory contains modular shell scripts for setting up a new server. Each script handles one specific task and can be run independently.

## Prerequisites

- Debian-based Linux server (Debian/Ubuntu)
- sudo privileges
- Internet connection

## Scripts Overview

| Script | Description | Parameters |
|--------|-------------|------------||
| `install-git.sh` | Installs Git | None |
| `clone-repo.sh` | Clones a Git repository using a GitHub token | github-username/repo, github-token |
| `mount-external-disk.sh` | Prepares external disk mounting | None |
| `generate_fstab.sh` | Generates fstab | None |
| `install-docker.sh` | Installs Docker and Docker Compose | None |
| `install-nginx.sh` | Installs and configures Nginx web server | domain |
| `setup-ssl.sh` | Sets up Let's Encrypt SSL certificate | domain [www-domain]
| `ops-agent.sh` | Installs Google Cloud Ops Agent for monitoring and logging | instance-name |

## Usage

### Make scripts executable
```bash
chmod +x *.sh
```

### Run individual scripts

```bash
# Install Git
./install-git.sh

# Clone repository using a GitHub token
./clone-repo.sh your-username/your-repo ghp_xxxxxxxxxxxx

# Mount external disk (follow the manual instructions printed)
./mount-external-disk.sh

# Generate fstab
./generate_fstab.sh

# Install Docker
./install-docker.sh

# Install Nginx (replace with your domain)
./install-nginx.sh dev.endee.io

# Setup SSL (replace with your domain)
./setup-ssl.sh dev.endee.io

# Install Google Cloud Ops Agent (for GCP instances only)
./ops-agent.sh your-instance-name
```

## Typical Setup Order

1. **Install Git** - `./install-git.sh`
2. **Clone Repository** - `./clone-repo.sh your-org/repo ghp_xxxxxxxxxxxx`
3. **Mount External Disk** - `./mount-external-disk.sh`
4. **Generate fstab** - `generate_fstab.sh`
5. **Install Docker** - `./install-docker.sh`
6. **Install Nginx** - `./install-nginx.sh your-domain.com`
7. **Setup SSL** - `./setup-ssl.sh your-domain.com`
8. **Install Google Cloud Ops Agent** - `./ops-agent.sh your-instance-name`

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

- **Clone Repo**: Uses a GitHub Personal Access Token (PAT) embedded in the HTTPS URL. The token is stored in the remote URL so `git pull` works without re-entering credentials
- **GitHub Token**: Create at https://github.com/settings/tokens/new — use a Fine-grained token with **Contents: read-only**, or a Classic token with the **repo** scope
- **External Disk Mount**: Includes manual steps for disk partitioning. Follow the printed instructions carefully
- **SSL Setup**: Requires a valid domain pointing to your server
- **Ops Agent**: GCP-only script. Requires gcloud CLI and user authentication

## Checking Service Status

```bash
# Check if services are running
sudo systemctl status docker
sudo systemctl status nginx
```

## Additional Tools

- `generate_fstab.sh` - Use after mount-external-disk.sh to make external disk mount permanent across reboots
