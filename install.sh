#!/bin/bash
set -euo pipefail

DATA_DIR=${1:-./endee-data}
CONTAINER_NAME=endee-server

# Check Docker is available and the daemon is running
if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker is not installed or not in PATH."
    exit 1
fi
if ! docker info >/dev/null 2>&1; then
    echo "ERROR: cannot connect to the Docker daemon. Is it running?"
    exit 1
fi

echo "Using data directory: $DATA_DIR"

# Create the directory if it does not exist
if [ ! -d "$DATA_DIR" ]; then
    echo "Directory '$DATA_DIR' does not exist. Creating..."
    if ! mkdir -p "$DATA_DIR"; then
        echo "Retrying with elevated privileges and taking ownership..."
        if sudo mkdir -p "$DATA_DIR" && sudo chown "$(id -un):$(id -gn)" "$DATA_DIR"; then
            :
        else
            echo "ERROR: Failed to create '$DATA_DIR'. Check parent directory permissions."
            echo "Fix manually: sudo mkdir -p \"$DATA_DIR\" && sudo chown \"$(id -un):$(id -gn)\" \"$DATA_DIR\""
            exit 1
        fi
    fi
    echo "Created '$DATA_DIR'."
fi

# Resolve to an absolute path — Docker bind mounts require it
DATA_DIR=$(cd "$DATA_DIR" && pwd)

# Ensure the directory is writable. If it isn't, take ownership and set
# standard permissions (rwxr-xr-x). Taking ownership fixes the common case
# of a root-owned directory, which a plain 'chmod +w' cannot.
if [ ! -w "$DATA_DIR" ]; then
    echo "Directory '$DATA_DIR' is not writable. Fixing ownership and permissions..."
    if sudo chown "$(id -un):$(id -gn)" "$DATA_DIR" && sudo chmod 755 "$DATA_DIR"; then
        echo "Ownership and write permission granted on '$DATA_DIR'."
    else
        echo "ERROR: Failed to fix permissions on '$DATA_DIR'."
        echo "Fix manually: sudo chown \"$(id -un):$(id -gn)\" \"$DATA_DIR\" && sudo chmod 755 \"$DATA_DIR\""
        exit 1
    fi
fi

# Remove any existing container with the same name so re-runs don't fail
if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
    echo "Removing existing container '$CONTAINER_NAME'..."
    docker rm -f "$CONTAINER_NAME" >/dev/null
fi

echo "All checks passed. Starting $CONTAINER_NAME..."

docker run -d \
  --ulimit nofile=100000:100000 \
  -p 8080:8080 \
  -v "$DATA_DIR":/data \
  --name "$CONTAINER_NAME" \
  --restart unless-stopped \
  endeeio/endee-server:latest

cat <<EOF

╭──────────────────────────────────────────────────────────────╮
│                                                              │
│   🚀  Endee server is up and running!                        │
│       __________        _________                            │
│       ___  ____/_______ ______  /_____ _____                 │
│       __  __/   __  __ \_  __  / _  _ \_  _ \                │
│       _  /___   _  / / // /_/ /  /  __//  __/                │
│       /_____/   /_/ /_/ \__,_/   \___/ \___/                 │
│                                                              │
╰──────────────────────────────────────────────────────────────╯

  🌐  Dashboard      http://localhost:8080
  📁  Data directory $DATA_DIR
  📖  Documentation  https://docs.endee.io

  ── Managing your container ───────────────────────────────────

  📜  View logs      docker logs -f $CONTAINER_NAME
  📊  Check status   docker ps --filter name=$CONTAINER_NAME
  🔄  Restart        docker restart $CONTAINER_NAME
  🛑  Stop           docker stop $CONTAINER_NAME
  🗑️   Remove         docker rm -f $CONTAINER_NAME

  ℹ️   The container auto-restarts on reboot. Run 'docker stop
      $CONTAINER_NAME' to keep it from coming back.

EOF
