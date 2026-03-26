#!/bin/bash
set -e

# Check if repo URL is provided
if [ -z "$1" ]; then
  echo "Usage: $0 <repository-url>"
  echo "Example: $0 https://github.com/your-username/your-repo.git"
  echo "Example: $0 git@github.com:your-username/your-repo.git"
  exit 1
fi

REPO_URL=$1

echo "Cloning repository..."

# Navigate to the directory where you want to clone the repo
cd /home/$USER

# Clone the repository
git clone $REPO_URL

# Extract repo name from URL
REPO_NAME=$(basename $REPO_URL .git)

# Navigate into the cloned repository
cd $REPO_NAME

# Check repository status
git status

echo "Repository cloned successfully to /home/$USER/$REPO_NAME"
