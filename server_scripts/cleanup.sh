#!/bin/bash

# Find the project root directory and load environment variables from .env if it exists
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -f "$PROJECT_ROOT/.env" ]; then
    export $(grep -v '^#' "$PROJECT_ROOT/.env" | xargs)
fi

DIR="${1:-${CLEANUP_DIR:-/home/hello/mnt/deleted}}"
if [ "$DIR" != "/" ] && [ -d "$DIR" ]; then
    SIZE=$(du -sh "$DIR" 2>/dev/null | cut -f1)
    echo "Directory size: $SIZE"
    read -p "Delete contents of $DIR? (y/N) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$DIR"/*
        echo "Cleaned $DIR"
    fi
else
    echo "Invalid directory: $DIR"
fi


# Incase of another directory to be cleared
# replace the CLEANUP_DIR variable in the env file or use the below command
# ./server_scripts/cleanup.sh "/home/debian/mnt/deleted" pass the path as an argument
