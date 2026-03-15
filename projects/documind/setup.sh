#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────── #
#  DocuMind – local dev setup script (macOS / Linux)                          #
# ──────────────────────────────────────────────────────────────────────────── #
set -euo pipefail

echo "╔══════════════════════════════════════════════════════╗"
echo "║           DocuMind – Setup & Launch                  ║"
echo "╚══════════════════════════════════════════════════════╝"

# 1. Start Endee via Docker Compose (background)
echo ""
echo "▶  Starting Endee vector database …"
docker compose up -d endee
echo "   Waiting for Endee to be healthy …"
until docker compose exec endee curl -sf http://localhost:8080/api/v1/indexes >/dev/null 2>&1; do
  sleep 2
  printf "."
done
echo ""
echo "   ✅  Endee is up at http://localhost:8080"

# 2. Python virtual environment
echo ""
echo "▶  Setting up Python virtual environment …"
cd backend
python3 -m venv .venv
source .venv/bin/activate
pip install --quiet --upgrade pip
pip install --quiet -r requirements.txt
echo "   ✅  Python deps installed"

# 3. Copy .env if not present
if [ ! -f ".env" ]; then
  cp .env.example .env
  echo "   📋  Created backend/.env from .env.example (edit it to add API keys)"
fi

# 4. Launch FastAPI backend
echo ""
echo "▶  Starting DocuMind backend on http://localhost:8000 …"
uvicorn main:app --host 0.0.0.0 --port 8000 --reload &
BACKEND_PID=$!
echo "   ✅  Backend PID: $BACKEND_PID"

# 5. Start React frontend
echo ""
echo "▶  Starting React frontend …"
cd ../frontend
npm install --silent
npm start &
FRONTEND_PID=$!
echo "   ✅  Frontend PID: $FRONTEND_PID"

echo ""
echo "═══════════════════════════════════════════════════════"
echo "  DocuMind is running!"
echo "  Frontend : http://localhost:3000"
echo "  Backend  : http://localhost:8000"
echo "  Endee    : http://localhost:8080"
echo "═══════════════════════════════════════════════════════"
echo "Press Ctrl+C to stop …"
wait
