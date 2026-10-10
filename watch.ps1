# Keep the GPU at full power: starts gpu-uncap.exe in watch mode, hidden, and leaves it running. It re-sends the
# "full power" auxiliary power state whenever Dell's controller clamps the GPU (the laptop-mode 25 W / 30 W cap).
# One watcher runs at a time; starting it again does nothing. Nothing here touches Valorant or Vanguard.
$exe = Join-Path $PSScriptRoot 'gpu-uncap.exe'
if (-not (Test-Path $exe)) { Write-Host "gpu-uncap.exe is missing - build it with: gcc -O2 -o gpu-uncap.exe gpu-uncap.c" -ForegroundColor Red; exit 1 }
$log = Join-Path $env:APPDATA 'gpu-uncap\gpu-uncap.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
if (Get-Process gpu-uncap -ErrorAction SilentlyContinue) { Write-Host 'Already running.' -ForegroundColor Green; exit 0 }
# keep the previous run's log (each start truncates the live one)
if (Test-Path $log) { Copy-Item $log ($log -replace '\.log$', '.prev.log') -Force }
Start-Process -FilePath $exe -ArgumentList '-w' -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -RedirectStandardOutput $log
Start-Sleep -Milliseconds 700
if (Get-Process gpu-uncap -ErrorAction SilentlyContinue) { Write-Host "Watching. Log: $log" -ForegroundColor Green }
else { Write-Host 'Failed to start.' -ForegroundColor Red }
