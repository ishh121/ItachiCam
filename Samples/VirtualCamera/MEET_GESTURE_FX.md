# Meet Gesture FX bring-up

This sample is the native Windows 11 foundation for a Google Meet-visible
camera effect. It uses Microsoft's `VirtualCameraMediaSource` sample rather
than trying to intercept Google Meet's browser camera code.

## What is implemented now

The `AugmentedMediaStream` path now modifies the NV12 sample from the physical
camera before forwarding it to the virtual camera consumer. A bright white
border is enabled by default as a remote-visibility marker.

The first approved effect is also wired into the same native frame path:

```text
open palm → small black fantasy crows with red eyes fly in all directions
→ crows fade → body is hidden for 3 seconds → body reforms
```

The second effect is also wired into the same native frame path:

```text
finger held vertically at the lips → both eyes pulse bright red for about 3 seconds
```

The first effect remains native and independent: the open-palm detector starts
the crow animation, and the body silhouette is filled from nearby live-frame
pixels during the invisible interval before it reforms. The original camera
path remains usable even when the optional Python eye worker is not running.

For the eye effect, an optional Python sidecar uses MediaPipe Face Landmarker
and Hand Landmarker with OpenCV. It returns temporally-smoothed eyelid contours,
iris geometry, and finger-on-lips state over loopback TCP. The native camera
uses that geometry to recolor the full-resolution iris/pupil region with a
feathered crimson filter. If the worker is not running, the crow effect is not
penalized and the eye effect remains inactive.

This is intentionally the first milestone:

```text
physical camera → augmented media source → virtual camera → Google Meet
```

Do not add hand tracking until the border is visible to a second Meet
participant. That proves the virtual camera path is carrying processed pixels.

## Required local setup

The current machine is Windows 11 Home Single Language, 64-bit, build 26200.
Install before building:

- Visual Studio 2022 Community or newer with **Desktop development with C++**.
- Windows SDK 10.0.26100.0 or newer.
- Windows Driver Kit (WDK), because `VirtualCameraMediaSource` is a custom
  Media Foundation source DLL.
- Visual Studio Installer Projects extension for the MSI project.
- Latest Microsoft Visual C++ Redistributable.

The repository's original sample README contains the dependency and install
order. Build these in **Release | x64**:

1. `VirtualCamera_Installer`
2. `VirtualCameraMediaSource`
3. `VirtualCamera_MSI_x64`
4. `VirtualCameraManager_WinRT`
5. `VirtualCameraSystray`
6. `VirtualCameraManager_App`

Install the MSI, then deploy the manager app.

## First end-to-end test

1. Use the manager app to create an **AugmentedMediaSource** virtual camera
   wrapping your physical webcam.
2. Open the Windows Camera app and select the new virtual camera.
3. Confirm that the preview has a white border.
4. Open Google Meet → **Settings → Video**.
5. Select the virtual camera.
6. Join from a second browser profile or device.
7. Confirm the second participant sees the white border.

If the white border is visible remotely, the next implementation slice is to
replace the lightweight detector and centered silhouette fallback with a real
hand-landmark model and segmentation mask while keeping the approved crow
timing and styling unchanged.
