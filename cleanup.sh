#!/bin/bash

DIR="${1:-${CLEANUP_DIR:-/home/debian/mnt/deleted}}"
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


# Incase of multiple directories to be cleared
# replace the dir path as DIR="${1:-/path/to/default/dir}"
# ./clean-quick.sh "/home/debian/mnt/deleted" pass the path as an argument
