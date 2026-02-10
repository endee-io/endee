#!/bin/bash

# Nexus Quick Start Script
# Automatically starts all services in the correct order

set -e  # Exit on error

echo "🚀 Starting Nexus - AI Knowledge Network"
echo "========================================"

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Check if services are already running
check_port() {
    if lsof -Pi :$1 -sTCP:LISTEN -t >/dev/null 2>&1 ; then
        return 0
    else
        return 1
    fi
}

# Step 1: Start Endee Vector Database
echo ""
echo -e "${BLUE}[1/3] Starting Endee Vector Database...${NC}"
if check_port 3001; then
    echo -e "${GREEN}✓ Endee already running on port 3001${NC}"
else
    if [ ! -f "./run.sh" ]; then
        echo -e "${RED}✗ run.sh not found. Please build Endee first with ./install.sh${NC}"
        exit 1
    fi
    ./run.sh &
    ENDEE_PID=$!
    echo -e "${GREEN}✓ Endee started (PID: $ENDEE_PID)${NC}"
    sleep 3  # Give Endee time to start
fi

# Step 2: Start FastAPI Backend
echo ""
echo -e "${BLUE}[2/3] Starting FastAPI Backend...${NC}"
if check_port 8000; then
    echo -e "${GREEN}✓ Backend already running on port 8000${NC}"
else
    cd nexus-backend
    
    # Check for virtual environment
    if [ ! -d "venv" ]; then
        echo "Creating Python virtual environment..."
        python3 -m venv venv
    fi
    
    # Activate virtual environment
    source venv/bin/activate
    
    # Install dependencies if needed
    if [ ! -f "venv/.installed" ]; then
        echo "Installing Python dependencies..."
        pip install -r requirements.txt > /dev/null 2>&1
        touch venv/.installed
    fi
    
    # Start backend
    python main.py > ../logs/backend.log 2>&1 &
    BACKEND_PID=$!
    echo -e "${GREEN}✓ Backend started (PID: $BACKEND_PID)${NC}"
    
    cd ..
    sleep 3  # Give backend time to start
    
    # Initialize Endee index
    echo "Initializing Nexus system..."
    curl -X POST http://localhost:8000/api/initialize > /dev/null 2>&1 || true
fi

# Step 3: Start Next.js Frontend
echo ""
echo -e "${BLUE}[3/3] Starting Next.js Frontend...${NC}"
if check_port 3000; then
    echo -e "${GREEN}✓ Frontend already running on port 3000${NC}"
else
    cd nexus-frontend
    
    # Install dependencies if needed
    if [ ! -d "node_modules" ]; then
        echo "Installing Node.js dependencies..."
        npm install > /dev/null 2>&1
    fi
    
    # Create .env.local if doesn't exist
    if [ ! -f ".env.local" ]; then
        cp .env.local.example .env.local
    fi
    
    # Start frontend
    npm run dev > ../logs/frontend.log 2>&1 &
    FRONTEND_PID=$!
    echo -e "${GREEN}✓ Frontend started (PID: $FRONTEND_PID)${NC}"
    
    cd ..
fi

# Create logs directory if doesn't exist
mkdir -p logs

# Final status
echo ""
echo "========================================"
echo -e "${GREEN}✓ Nexus is now running!${NC}"
echo ""
echo "📊 Services:"
echo "  • Endee:    http://localhost:3001"
echo "  • Backend:  http://localhost:8000"
echo "  • Frontend: http://localhost:3000"
echo ""
echo "🎯 Open your browser to: http://localhost:3000"
echo ""
echo "📝 API Documentation: http://localhost:8000/docs"
echo ""
echo "To stop all services, run:"
echo "  ./scripts/stop-nexus.sh"
echo ""
echo "Logs are saved in ./logs/"
echo "========================================"
