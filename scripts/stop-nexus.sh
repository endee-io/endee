#!/bin/bash

# Nexus Stop Script
# Stops all Nexus services

echo "🛑 Stopping Nexus services..."

# Stop processes by port
kill_port() {
    PID=$(lsof -ti:$1)
    if [ ! -z "$PID" ]; then
        kill -9 $PID 2>/dev/null
        echo "✓ Stopped service on port $1"
    fi
}

kill_port 3001  # Endee
kill_port 8000  # Backend
kill_port 3000  # Frontend

echo "✓ All Nexus services stopped"
