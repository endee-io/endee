#!/bin/bash
set -e

# Check if email is provided
if [ -z "$1" ]; then
  echo "Usage: $0 <email>"
  echo "Example: $0 your.email@example.com"
  exit 1
fi

EMAIL=$1

echo "Generating SSH key for $EMAIL..."

# Generate a new SSH key pair using RSA
ssh-keygen -t rsa -b 4096 -C "$EMAIL"

# Start the ssh-agent in the background
eval "$(ssh-agent -s)"

# Add your SSH private key to the ssh-agent
ssh-add ~/.ssh/id_rsa

# Display your public key
echo ""
echo "Your public SSH key:"
echo "===================="
cat ~/.ssh/id_rsa.pub
echo "===================="
echo ""
echo "Copy the above key and add it to:"
echo "- GitHub: Settings → SSH and GPG keys → New SSH key"
echo "- GitLab: Preferences → SSH Keys → Add SSH Key"
echo "- Bitbucket: Personal settings → SSH keys → Add key"
echo ""
echo "Test connection with: ssh -T git@github.com"
