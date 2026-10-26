$ErrorActionPreference = 'Stop'
Unregister-ScheduledTask -TaskName 'Meet Gesture FX Eye Worker' -Confirm:$false -ErrorAction SilentlyContinue
