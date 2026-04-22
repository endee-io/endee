#!/bin/bash
set -e

# Usage: ./clone-repo.sh <github-username/repo> <github-token>
# Token needs at least 'repo' (read) scope.
# Create one at: https://github.com/settings/tokens/new
#   - Select scope: Contents (read-only) under "Fine-grained tokens", or "repo" under classic tokens

if [ -z "$1" ] || [ -z "$2" ]; then
  echo "Usage: $0 <github-username/repo> <github-token>"
  echo "Example: $0 your-org/your-repo ghp_xxxxxxxxxxxx"
  echo ""
  echo "To create a token:"
  echo "  1. Go to https://github.com/settings/tokens/new"
  echo "  2. For Fine-grained token: grant 'Contents' read-only permission"
  exit 1
fi

REPO=$1
TOKEN=$2
REPO_URL="https://${TOKEN}@github.com/${REPO}.git"
REPO_NAME=$(basename $REPO .git)

echo "Cloning repository $REPO..."
cd /home/$USER
git clone "$REPO_URL"
echo "Repository cloned successfully to /home/$USER/$REPO_NAME"

echo "Configuring git to store token for future pulls..."
cd $REPO_NAME
git remote set-url origin "$REPO_URL"
echo "Done. You can now run 'git pull' without entering credentials."

git status
