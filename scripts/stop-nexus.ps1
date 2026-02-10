# PowerShell script to stop Nexus services on Windows

Write-Host "🛑 Stopping Nexus services..." -ForegroundColor Yellow

# Function to kill process on port
function Stop-PortProcess {
    param([int]$Port)
    
    $process = Get-NetTCPConnection -LocalPort $Port -ErrorAction SilentlyContinue | 
               Select-Object -ExpandProperty OwningProcess -Unique
    
    if ($process) {
        Stop-Process -Id $process -Force -ErrorAction SilentlyContinue
        Write-Host "✓ Stopped service on port $Port" -ForegroundColor Green
    }
}

# Stop services
Stop-PortProcess 3001  # Endee
Stop-PortProcess 8000  # Backend
Stop-PortProcess 3000  # Frontend

Write-Host "✓ All Nexus services stopped" -ForegroundColor Green
