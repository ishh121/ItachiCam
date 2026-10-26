#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$sourceDll = Join-Path $repoRoot 'Samples\VirtualCamera\x64\Release\VirtualCameraMediaSource.dll'
$sourceAsset = Join-Path $repoRoot 'Samples\VirtualCamera\x64\Release\crow_public_domain.png'
$targetDll = 'C:\Program Files\MeetGestureFX\VirtualCameraMediaSource-1400.dll'
$targetAsset = 'C:\Program Files\MeetGestureFX\crow_public_domain.png'
$clsid = '{7B89B92E-FE71-42D0-8A41-E137D06EA184}'

Stop-Service FrameServerMonitor -Force -ErrorAction SilentlyContinue
Stop-Service FrameServer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 5

Copy-Item -LiteralPath $sourceDll -Destination $targetDll -Force
Copy-Item -LiteralPath $sourceAsset -Destination $targetAsset -Force

Set-ItemProperty -Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32" -Name '(default)' -Value $targetDll
Set-ItemProperty -Path "HKCU:\Software\Classes\CLSID\$clsid\InprocServer32" -Name '(default)' -Value $targetDll

Start-Service FrameServer
Start-Service FrameServerMonitor -ErrorAction SilentlyContinue

Get-FileHash -Algorithm SHA256 -LiteralPath $targetDll
Get-Service FrameServer, FrameServerMonitor | Select-Object Name, Status
