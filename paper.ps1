param([ValidateSet('start', 'stop', 'reset', 'status', 'logs')][string]$Action = 'start')
$ErrorActionPreference = 'Stop'
$composePath = Join-Path $PSScriptRoot 'compose.paper.yml'
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw 'Install and start Docker Desktop with Linux containers first.' }
switch ($Action) {
    'start' { docker compose -f $composePath up -d --build --wait --wait-timeout 120 }
    'stop' { docker compose -f $composePath down }
    'reset' {
        docker compose -f $composePath down
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        docker compose -f $composePath up -d --wait --wait-timeout 120
    }
    'status' { docker compose -f $composePath ps }
    'logs' { docker compose -f $composePath logs --tail 100 }
}
if ($LASTEXITCODE -ne 0) { throw 'Paper stack command failed. Check Docker Desktop and run .\paper.ps1 logs.' }
if ($Action -in @('start','reset')) { Write-Host 'Paper trading is ready: http://localhost:8080 (simulated orders, volatile book)' }
