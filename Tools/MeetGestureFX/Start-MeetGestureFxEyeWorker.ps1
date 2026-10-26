$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$python = Join-Path $repoRoot '.venv-eye-worker\Scripts\python.exe'
$worker = Join-Path $PSScriptRoot 'eye_worker.py'
$logDir = 'C:\ProgramData\MeetGestureFX'
$stdoutLog = Join-Path $logDir 'eye-worker.stdout.log'
$stderrLog = Join-Path $logDir 'eye-worker.stderr.log'

New-Item -ItemType Directory -Path $logDir -Force | Out-Null

$listener = Get-NetTCPConnection -LocalPort 38741 -State Listen -ErrorAction SilentlyContinue
if ($listener) {
    exit 0
}

if (!(Test-Path -LiteralPath $python) -or !(Test-Path -LiteralPath $worker)) {
    Add-Content -LiteralPath $stderrLog -Value "$(Get-Date -Format o) Missing Python eye-worker files."
    exit 1
}

Start-Process `
    -FilePath $python `
    -ArgumentList @($worker) `
    -WorkingDirectory $repoRoot `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog | Out-Null
