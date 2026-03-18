#!/bin/sh
set -e

# ============================================================
# Endee Docker Entrypoint
# Validates that NDD_AUTH_TOKEN is set before starting the server
# ============================================================

if [ -z "${NDD_AUTH_TOKEN}" ]; then
    echo ""
    echo "============================================================"
    echo "  ERROR: NDD_AUTH_TOKEN is not set!"
    echo ""
    echo "  Endee requires an authentication token to run."
    echo "  Please set the NDD_AUTH_TOKEN environment variable."
    echo ""
    echo "  Example (docker run):"
    echo "    docker run -e NDD_AUTH_TOKEN=\"your_secure_token\" ..."
    echo ""
    echo "  Example (docker-compose):"
    echo "    environment:"
    echo "      NDD_AUTH_TOKEN: \"your_secure_token\""
    echo ""
    echo "  To generate a secure token:"
    echo "    openssl rand -hex 32"
    echo "============================================================"
    echo ""
    exit 1
fi

echo "[INFO] NDD_AUTH_TOKEN is set. Authentication is ENABLED."

# Execute the main binary with any arguments passed to the container
exec /usr/local/bin/ndd "$@"