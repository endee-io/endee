"""
repo_loader.py

This module downloads GitHub repositories
so they can be parsed and indexed.
"""

import os
import sys
import subprocess


def clone_repo(repo_url, destination="data/repos"):
    """
    Clone a GitHub repository into the data folder.
    """

    # Ensure destination folder exists
    os.makedirs(destination, exist_ok=True)

    repo_name = repo_url.split("/")[-1].replace(".git", "")
    repo_path = os.path.join(destination, repo_name)

    if os.path.exists(repo_path):
        print(f"Repository already exists: {repo_path}")
        return repo_path

    print(f"Cloning repository: {repo_url}")

    subprocess.run(
        ["git", "clone", repo_url, repo_path],
        check=True
    )

    print(f"Repository cloned to: {repo_path}")

    return repo_path


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python repo_loader.py <github_repo_url>")
        sys.exit(1)

    repo_url = sys.argv[1]

    clone_repo(repo_url)