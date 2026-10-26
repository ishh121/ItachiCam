# Meet Gesture FX Python eye worker

This sidecar uses MediaPipe Face Landmarker, Hand Landmarker, and OpenCV on a
small BGR copy of the camera frame. It tracks the index finger near the lips
with temporal hysteresis and returns the two eyelid contours plus iris
geometry. The native camera applies the returned geometry to the full-resolution
YUY2/NV12 frame.

Install into a local virtual environment:

```powershell
python -m venv .venv-eye-worker
.\.venv-eye-worker\Scripts\python.exe -m pip install -r Tools\MeetGestureFX\requirements-eye-worker.txt
```

Download the official MediaPipe Face Landmarker and Hand Landmarker model
bundles:

```powershell
.\.venv-eye-worker\Scripts\python.exe Tools\MeetGestureFX\eye_worker.py --download-models
```

Install automatic startup once:

```powershell
& .\Tools\MeetGestureFX\Install-MeetGestureFxAutoStart.ps1
```

This creates a per-user Windows scheduled task that starts the worker at sign
in and starts it immediately. The worker runs hidden; logs are written to
`C:\ProgramData\MeetGestureFX\eye-worker.stdout.log` and
`C:\ProgramData\MeetGestureFX\eye-worker.stderr.log`.

To remove automatic startup:

```powershell
& .\Tools\MeetGestureFX\Remove-MeetGestureFxAutoStart.ps1
```

Manual start remains available:

```powershell
.\.venv-eye-worker\Scripts\python.exe Tools\MeetGestureFX\eye_worker.py
```

The worker listens only on `127.0.0.1:38741`. The native camera continues to
run its first crow/body-dispersion feature if the worker is unavailable.
