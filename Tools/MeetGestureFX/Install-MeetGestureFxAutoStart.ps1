$ErrorActionPreference = 'Stop'

$taskName = 'Meet Gesture FX Eye Worker'
$launcher = (Resolve-Path (Join-Path $PSScriptRoot 'Start-MeetGestureFxEyeWorker.ps1')).Path
$userId = "$env:USERDOMAIN\$env:USERNAME"

$action = New-ScheduledTaskAction `
    -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$launcher`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $userId
$principal = New-ScheduledTaskPrincipal `
    -UserId $userId `
    -LogonType Interactive `
    -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet `
    -Hidden `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -MultipleInstances IgnoreNew

Register-ScheduledTask `
    -TaskName $taskName `
    -Action $action `
    -Trigger $trigger `
    -Principal $principal `
    -Settings $settings `
    -Force | Out-Null

& $launcher

Get-ScheduledTask -TaskName $taskName | Select-Object TaskName, State
