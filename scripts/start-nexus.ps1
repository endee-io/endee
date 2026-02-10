# PowerShell script to start Nexus on Windows

Write-Host "🚀 Starting Nexus - AI Knowledge Network" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# Function to check if a port is in use
function Test-Port {
    param([int]$Port)
    $connection = Test-NetConnection -ComputerName localhost -Port $Port -WarningAction SilentlyContinue
    return $connection.TcpTestSucceeded
}

# Step 1: Start Endee Vector Database
Write-Host ""
Write-Host "[1/3] Starting Endee Vector Database..." -ForegroundColor Blue

if (Test-Port 3001) {
    Write-Host "✓ Endee already running on port 3001" -ForegroundColor Green
} else {
    if (Test-Path ".\run.sh") {
        Write-Host "Starting Endee..." -ForegroundColor Yellow
        Start-Process -FilePath "bash" -ArgumentList "./run.sh" -WindowStyle Hidden
        Start-Sleep -Seconds 3
        Write-Host "✓ Endee started" -ForegroundColor Green
    } else {
        Write-Host "✗ run.sh not found. Please build Endee first with ./install.sh" -ForegroundColor Red
        Write-Host "Note: You need WSL or Git Bash to run Endee on Windows" -ForegroundColor Yellow
        exit 1
    }
}

# Step 2: Start FastAPI Backend
Write-Host ""
Write-Host "[2/3] Starting FastAPI Backend..." -ForegroundColor Blue

if (Test-Port 8000) {
    Write-Host "✓ Backend already running on port 8000" -ForegroundColor Green
} else {
    Push-Location nexus-backend
    
    # Check for virtual environment
    if (-not (Test-Path "venv")) {
        Write-Host "Creating Python virtual environment..." -ForegroundColor Yellow
        python -m venv venv
    }
    
    # Activate virtual environment and install dependencies
    if (-not (Test-Path "venv\.installed")) {
        Write-Host "Installing Python dependencies..." -ForegroundColor Yellow
        .\venv\Scripts\Activate.ps1
        pip install -r requirements.txt | Out-Null
        New-Item -Path "venv\.installed" -ItemType File | Out-Null
    }
    
    # Create logs directory
    if (-not (Test-Path "..\logs")) {
        New-Item -Path "..\logs" -ItemType Directory | Out-Null
    }
    
    # Start backend in background
    Start-Process powershell -ArgumentList "-NoExit", "-Command", "cd '$PWD'; .\venv\Scripts\Activate.ps1; python main.py" -WindowStyle Hidden
    Start-Sleep -Seconds 3
    Write-Host "✓ Backend started" -ForegroundColor Green
    
    # Initialize Endee index
    Write-Host "Initializing Nexus system..." -ForegroundColor Yellow
    try {
        Invoke-RestMethod -Uri "http://localhost:8000/api/initialize" -Method Post -ErrorAction SilentlyContinue | Out-Null
    } catch {
        # Ignore errors, index may already exist
    }
    
    Pop-Location
}

# Step 3: Start Next.js Frontend
Write-Host ""
Write-Host "[3/3] Starting Next.js Frontend..." -ForegroundColor Blue

if (Test-Port 3000) {
    Write-Host "✓ Frontend already running on port 3000" -ForegroundColor Green
} else {
    Push-Location nexus-frontend
    
    # Install dependencies if needed
    if (-not (Test-Path "node_modules")) {
        Write-Host "Installing Node.js dependencies..." -ForegroundColor Yellow
        npm install | Out-Null
    }
    
    # Create .env.local if doesn't exist
    if (-not (Test-Path ".env.local")) {
        Copy-Item ".env.local.example" ".env.local"
    }
    
    # Start frontend in background
    Start-Process powershell -ArgumentList "-NoExit", "-Command", "cd '$PWD'; npm run dev" -WindowStyle Hidden
    Start-Sleep -Seconds 3
    Write-Host "✓ Frontend started" -ForegroundColor Green
    
    Pop-Location
}

# Final status
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "✓ Nexus is now running!" -ForegroundColor Green
Write-Host ""
Write-Host "📊 Services:" -ForegroundColor Cyan
Write-Host "  • Endee:    http://localhost:3001"
Write-Host "  • Backend:  http://localhost:8000"
Write-Host "  • Frontend: http://localhost:3000"
Write-Host ""
Write-Host "🎯 Open your browser to: http://localhost:3000" -ForegroundColor Yellow
Write-Host ""
Write-Host "📝 API Documentation: http://localhost:8000/docs"
Write-Host ""
Write-Host "To stop all services, close the terminal windows or run:" -ForegroundColor Gray
Write-Host "  .\scripts\stop-nexus.ps1" -ForegroundColor Gray
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
