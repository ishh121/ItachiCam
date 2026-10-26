# Meet Gesture FX native camera

This workspace is based on Microsoft's Windows Camera virtual-camera sample.
The first implementation milestone is deliberately small: add a visible NV12
border to the physical camera frames as they pass through an augmented virtual
camera. Once that border is visible to a second Google Meet participant, hand
tracking and 3D effects can be added without guessing whether the virtual
camera path works.

Implementation details and bring-up instructions are in:

```text
Samples/VirtualCamera/MEET_GESTURE_FX.md
```

Changed source files:

```text
Samples/VirtualCamera/VirtualCameraMediaSource/AugmentedMediaSource.h
Samples/VirtualCamera/VirtualCameraMediaSource/AugmentedMediaStream.cpp
```

The current machine is Windows 11 Home Single Language, 64-bit, build 26200.
The local shell does not currently have `cl`, `msbuild`, `cmake`, or `vswhere`,
so compilation is blocked until Visual Studio C++/Windows SDK/WDK are installed.
