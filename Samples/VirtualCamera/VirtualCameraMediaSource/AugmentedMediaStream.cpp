//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#include "pch.h"
#include "AugmentedMediaStream.h"
#include "mfobjects.h"
#include "d3d11.h"
#include "dxgi.h"
#include "wincodec.h"
#include <MemoryBuffer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.FaceAnalysis.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

#pragma comment(lib, "windowscodecs.lib")

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace winrt::WindowsSample::implementation
{
    namespace
    {
        constexpr float kPi = 3.14159265359f;
        constexpr UINT32 kPythonEyeRequestMagic = 0x3145474D; // MGE1
        constexpr UINT32 kPythonEyeResponseMagic = 0x3245474D; // MGE2
        constexpr UINT32 kPythonEyePort = 38741;
        constexpr UINT32 kPythonEyeWidth = 320;
        constexpr UINT32 kPythonEyeHeight = 180;
        constexpr UINT32 kPythonEyeRecordBytes = 25 * sizeof(float);

#pragma pack(push, 1)
        struct PythonEyeRequestHeader
        {
            UINT32 magic;
            UINT32 frameId;
            UINT32 width;
            UINT32 height;
            UINT32 channels;
        };

        struct PythonEyeResponseHeader
        {
            UINT32 magic;
            UINT32 frameId;
            UINT32 flags;
            UINT32 eyeCount;
            UINT32 payloadBytes;
        };
#pragma pack(pop)

        struct WseFaceMetadataHeader
        {
            UINT32 count;
            UINT32 size;
        };

        struct WseFaceLandmarksMetadata
        {
            float landmarks[70][2];
            float confidence[70];
            UINT32 trackId;
        };

        constexpr UINT32 kWseFaceLandmarksCount = 70;
        constexpr UINT32 kWseLeftEyeFirst = 36;
        constexpr UINT32 kWseLeftEyeLast = 41;
        constexpr UINT32 kWseRightEyeFirst = 42;
        constexpr UINT32 kWseRightEyeLast = 47;
        constexpr UINT32 kWseLeftIris = 68;
        constexpr UINT32 kWseRightIris = 69;

        const GUID kMfWindowsStudioMetadataFaceLandmarks =
        { 0x1239d90e, 0x6920, 0x49a9, { 0xb7, 0x2e, 0x5f, 0x6d, 0x77, 0x94, 0x46, 0x75 } };

        float NextRandom(_Inout_ UINT32& state)
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>((state >> 8) & 0x00FFFFFF) / 16777215.0f;
        }

        BYTE BlendByte(BYTE source, BYTE target, float amount)
        {
            amount = std::clamp(amount, 0.0f, 1.0f);
            return static_cast<BYTE>(source + (target - source) * amount);
        }

        bool IsBodySilhouette(UINT x, UINT y, UINT width, UINT height)
        {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);

            const float headX = (fx - 0.50f) / 0.135f;
            const float headY = (fy - 0.28f) / 0.16f;
            const bool head = headX * headX + headY * headY <= 1.0f;

            if (fy < 0.36f) return head;
            const float shoulderWidth = 0.25f + (fy - 0.36f) * 0.27f;
            return std::abs(fx - 0.50f) <= shoulderWidth && fy <= 0.98f;
        }

        float SmoothStep(float edge0, float edge1, float value)
        {
            if (edge0 == edge1) return value < edge0 ? 0.0f : 1.0f;
            const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        float BodyMaskCoverage(UINT x, UINT y, UINT width, UINT height)
        {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);

            const float headX = (fx - 0.50f) / 0.135f;
            const float headY = (fy - 0.28f) / 0.16f;
            const float headRadius = headX * headX + headY * headY;
            const float headCoverage = 1.0f - SmoothStep(0.78f, 1.04f, headRadius);

            if (fy < 0.31f) return headCoverage;
            if (fy > 0.99f) return 0.0f;

            const float torsoWidth = 0.25f + (fy - 0.36f) * 0.27f;
            const float distanceFromEdge = torsoWidth - std::abs(fx - 0.50f);
            const float torsoCoverage = SmoothStep(-0.018f, 0.018f, distanceFromEdge);
            const float topCoverage = SmoothStep(0.30f, 0.36f, fy);
            return std::max(headCoverage, torsoCoverage * topCoverage);
        }

        bool IsLikelySkin(int luma, int chromaU, int chromaV)
        {
            if (luma <= 20 || luma >= 250) return false;

            // Skin has a noticeably warmer Cr/V channel than Cb/U. Rejecting
            // neutral chroma is important: otherwise most of the camera
            // background is classified as skin and the palm bounding box is
            // rejected as oversized.
            const bool normalOrder = chromaU >= 70 && chromaU <= 150
                && chromaV >= 120 && chromaV <= 220
                && chromaV > chromaU + 20;
            const bool swappedOrder = chromaV >= 70 && chromaV <= 150
                && chromaU >= 120 && chromaU <= 220
                && chromaU > chromaV + 20;
            return normalOrder || swappedOrder;
        }

        template <typename IsSkinFn>
        bool DetectOpenPalmGrid(UINT width, UINT height, IsSkinFn&& isSkin)
        {
            constexpr UINT step = 6;
            const UINT gridWidth = (width + step - 1) / step;
            const UINT gridHeight = ((height * 3) / 4 + step - 1) / step;
            std::vector<BYTE> mask(gridWidth * gridHeight, 0);

            for (UINT gy = 0; gy < gridHeight; ++gy)
            {
                const UINT y = std::min(height - 1, gy * step + step / 2);
                for (UINT gx = 0; gx < gridWidth; ++gx)
                {
                    const UINT x = std::min(width - 1, gx * step + step / 2);
                    mask[gy * gridWidth + gx] = isSkin(x, y) ? 1 : 0;
                }
            }

            std::vector<UINT> queue;
            queue.reserve(gridWidth * gridHeight);
            for (UINT gy = 0; gy < gridHeight; ++gy)
            {
                for (UINT gx = 0; gx < gridWidth; ++gx)
                {
                    const UINT start = gy * gridWidth + gx;
                    if (mask[start] == 0) continue;

                    mask[start] = 0;
                    queue.clear();
                    queue.push_back(start);
                    UINT area = 0;
                    UINT minX = gx;
                    UINT maxX = gx;
                    UINT minY = gy;
                    UINT maxY = gy;

                    for (size_t index = 0; index < queue.size(); ++index)
                    {
                        const UINT cell = queue[index];
                        const UINT cx = cell % gridWidth;
                        const UINT cy = cell / gridWidth;
                        ++area;
                        minX = std::min(minX, cx);
                        maxX = std::max(maxX, cx);
                        minY = std::min(minY, cy);
                        maxY = std::max(maxY, cy);

                        const UINT neighbors[4] = {
                            cx > 0 ? cell - 1 : cell,
                            cx + 1 < gridWidth ? cell + 1 : cell,
                            cy > 0 ? cell - gridWidth : cell,
                            cy + 1 < gridHeight ? cell + gridWidth : cell,
                        };
                        for (UINT neighbor : neighbors)
                        {
                            if (neighbor == cell || mask[neighbor] == 0) continue;
                            mask[neighbor] = 0;
                            queue.push_back(neighbor);
                        }
                    }

                    const UINT boxWidth = (maxX - minX + 1) * step;
                    const UINT boxHeight = (maxY - minY + 1) * step;
                    const float aspect = static_cast<float>(boxHeight) / static_cast<float>(boxWidth);
                    const float fill = static_cast<float>(area) /
                        static_cast<float>((maxX - minX + 1) * (maxY - minY + 1));
                    const bool plausibleHand = area >= 10 && area <= 600
                        && boxWidth >= 30 && boxHeight >= 42
                        && aspect > 0.55f && aspect < 3.2f
                        && fill < 0.92f
                        && minY * step < (height * 55) / 100;
                    if (plausibleHand) return true;
                }
            }
            return false;
        }

        bool DetectOpenPalmNv12(
            _In_reads_bytes_(bufferLength) const BYTE* buffer,
            _In_ DWORD bufferLength,
            _In_ LONG pitch,
            _In_ UINT width,
            _In_ UINT height)
        {
            if (buffer == nullptr || pitch <= 0 || width < 32 || height < 32) return false;
            const DWORD stride = static_cast<DWORD>(pitch);
            const DWORD required = stride * height + stride * (height / 2);
            if (bufferLength < required) return false;

            const BYTE* yPlane = buffer;
            const BYTE* uvPlane = buffer + stride * height;
            return DetectOpenPalmGrid(width, height, [&](UINT x, UINT y) {
                const UINT uvX = x & ~1u;
                const BYTE* yRow = yPlane + y * stride;
                const BYTE* uvRow = uvPlane + (y / 2) * stride;
                return IsLikelySkin(yRow[x], uvRow[uvX], uvRow[uvX + 1]);
            });
        }

        bool DetectOpenPalmYuy2(
            _In_reads_bytes_(bufferLength) const BYTE* buffer,
            _In_ DWORD bufferLength,
            _In_ LONG pitch,
            _In_ UINT width,
            _In_ UINT height)
        {
            if (buffer == nullptr || pitch <= 0 || width < 32 || height < 32) return false;
            const DWORD stride = static_cast<DWORD>(pitch);
            if (bufferLength < stride * height) return false;

            return DetectOpenPalmGrid(width, height, [&](UINT x, UINT y) {
                const BYTE* row = buffer + y * stride;
                const UINT offset = (x & ~1u) * 2;
                return IsLikelySkin(row[offset], row[offset + 1], row[offset + 3]);
            });
        }

        template <typename IsSkinFn>
        bool DetectFingerOnLipGrid(UINT width, UINT height, IsSkinFn&& isSkin)
        {
            constexpr UINT step = 6;
            const UINT gridWidth = (width + step - 1) / step;
            const UINT gridHeight = ((height * 7) / 8 + step - 1) / step;
            std::vector<BYTE> mask(gridWidth * gridHeight, 0);

            for (UINT gy = 0; gy < gridHeight; ++gy)
            {
                const UINT y = std::min(height - 1, gy * step + step / 2);
                for (UINT gx = 0; gx < gridWidth; ++gx)
                {
                    const UINT x = std::min(width - 1, gx * step + step / 2);
                    mask[gy * gridWidth + gx] = isSkin(x, y) ? 1 : 0;
                }
            }

            std::vector<UINT> queue;
            queue.reserve(gridWidth * gridHeight);
            for (UINT gy = 0; gy < gridHeight; ++gy)
            {
                for (UINT gx = 0; gx < gridWidth; ++gx)
                {
                    const UINT start = gy * gridWidth + gx;
                    if (mask[start] == 0) continue;

                    mask[start] = 0;
                    queue.clear();
                    queue.push_back(start);
                    UINT area = 0;
                    UINT minX = gx;
                    UINT maxX = gx;
                    UINT minY = gy;
                    UINT maxY = gy;
                    UINT lowerArea = 0;
                    UINT lowerMinX = gridWidth;
                    UINT lowerMaxX = 0;
                    UINT lowerMinY = gridHeight;
                    UINT lowerMaxY = 0;

                    for (size_t index = 0; index < queue.size(); ++index)
                    {
                        const UINT cell = queue[index];
                        const UINT cx = cell % gridWidth;
                        const UINT cy = cell / gridWidth;
                        ++area;
                        minX = std::min(minX, cx);
                        maxX = std::max(maxX, cx);
                        minY = std::min(minY, cy);
                        maxY = std::max(maxY, cy);

                        const UINT sampleY = cy * step + step / 2;
                        if (sampleY >= (height * 34) / 100 && sampleY <= (height * 72) / 100)
                        {
                            ++lowerArea;
                            lowerMinX = std::min(lowerMinX, cx);
                            lowerMaxX = std::max(lowerMaxX, cx);
                            lowerMinY = std::min(lowerMinY, cy);
                            lowerMaxY = std::max(lowerMaxY, cy);
                        }

                        const UINT neighbors[4] = {
                            cx > 0 ? cell - 1 : cell,
                            cx + 1 < gridWidth ? cell + 1 : cell,
                            cy > 0 ? cell - gridWidth : cell,
                            cy + 1 < gridHeight ? cell + gridWidth : cell,
                        };
                        for (UINT neighbor : neighbors)
                        {
                            if (neighbor == cell || mask[neighbor] == 0) continue;
                            mask[neighbor] = 0;
                            queue.push_back(neighbor);
                        }
                    }

                    if (lowerArea == 0 || lowerMinX == gridWidth) continue;

                    const UINT lowerWidth = (lowerMaxX - lowerMinX + 1) * step;
                    const UINT lowerHeight = (lowerMaxY - lowerMinY + 1) * step;
                    const float lowerCenter =
                        ((static_cast<float>(lowerMinX + lowerMaxX + 1) * 0.5f) * step) /
                        static_cast<float>(width);
                    const float lowerAspect = static_cast<float>(lowerHeight) /
                        static_cast<float>(std::max(1u, lowerWidth));
                    const float fill = static_cast<float>(lowerArea) /
                        static_cast<float>((lowerMaxX - lowerMinX + 1) * (lowerMaxY - lowerMinY + 1));

                    // A finger held vertically at the lips appears as a narrow,
                    // central skin extension below the wider face component.
                    const bool plausibleFinger = area >= 12 && area <= 900
                        && lowerArea >= 8
                        && lowerWidth >= 18 && lowerWidth <= width / 6
                        && lowerHeight >= 42
                        && lowerAspect >= 1.35f
                        && lowerCenter >= 0.36f && lowerCenter <= 0.64f
                        && minY * step < (height * 48) / 100
                        && maxY * step > (height * 48) / 100
                        && fill < 0.90f;
                    if (plausibleFinger) return true;
                }
            }
            return false;
        }

        bool DetectFingerOnLipNv12(
            _In_reads_bytes_(bufferLength) const BYTE* buffer,
            _In_ DWORD bufferLength,
            _In_ LONG pitch,
            _In_ UINT width,
            _In_ UINT height)
        {
            if (buffer == nullptr || pitch <= 0 || width < 32 || height < 32) return false;
            const DWORD stride = static_cast<DWORD>(pitch);
            const DWORD required = stride * height + stride * (height / 2);
            if (bufferLength < required) return false;

            const BYTE* yPlane = buffer;
            const BYTE* uvPlane = buffer + stride * height;
            return DetectFingerOnLipGrid(width, height, [&](UINT x, UINT y) {
                const UINT uvX = x & ~1u;
                const BYTE* yRow = yPlane + y * stride;
                const BYTE* uvRow = uvPlane + (y / 2) * stride;
                return IsLikelySkin(yRow[x], uvRow[uvX], uvRow[uvX + 1]);
            });
        }

        bool DetectFingerOnLipYuy2(
            _In_reads_bytes_(bufferLength) const BYTE* buffer,
            _In_ DWORD bufferLength,
            _In_ LONG pitch,
            _In_ UINT width,
            _In_ UINT height)
        {
            if (buffer == nullptr || pitch <= 0 || width < 32 || height < 32) return false;
            const DWORD stride = static_cast<DWORD>(pitch);
            if (bufferLength < stride * height) return false;

            return DetectFingerOnLipGrid(width, height, [&](UINT x, UINT y) {
                const BYTE* row = buffer + y * stride;
                const UINT offset = (x & ~1u) * 2;
                return IsLikelySkin(row[offset], row[offset + 1], row[offset + 3]);
            });
        }

        void SetNv12Pixel(
            _Inout_ BYTE* buffer,
            _In_ LONG pitch,
            _In_ UINT height,
            _In_ UINT x,
            _In_ UINT y,
            _In_ BYTE targetY,
            _In_ BYTE targetU,
            _In_ BYTE targetV,
            _In_ float amount)
        {
            const UINT stride = static_cast<UINT>(pitch);
            BYTE* yPlane = buffer;
            BYTE* uvPlane = buffer + stride * height;
            BYTE* yPixel = yPlane + y * stride + x;
            *yPixel = BlendByte(*yPixel, targetY, amount);
            BYTE* uvPixel = uvPlane + (y / 2) * stride + (x & ~1u);
            *uvPixel = BlendByte(*uvPixel, targetU, amount);
            *(uvPixel + 1) = BlendByte(*(uvPixel + 1), targetV, amount);
        }

        void SetYuy2Pixel(
            _Inout_ BYTE* buffer,
            _In_ LONG pitch,
            _In_ UINT x,
            _In_ UINT y,
            _In_ BYTE targetY,
            _In_ BYTE targetU,
            _In_ BYTE targetV,
            _In_ float amount)
        {
            BYTE* pixel = buffer + y * static_cast<UINT>(pitch) + (x & ~1u) * 2;
            const UINT yOffset = (x & 1u) == 0 ? 0 : 2;
            pixel[yOffset] = BlendByte(pixel[yOffset], targetY, amount);
            pixel[1] = BlendByte(pixel[1], targetU, amount);
            pixel[3] = BlendByte(pixel[3], targetV, amount);
        }

        void SetEffectPixel(
            _Inout_ BYTE* buffer,
            _In_ LONG pitch,
            _In_ UINT height,
            _In_ UINT x,
            _In_ UINT y,
            _In_ GUID const& subtype,
            _In_ BYTE targetY,
            _In_ BYTE targetU,
            _In_ BYTE targetV,
            _In_ float amount)
        {
            if (IsEqualGUID(subtype, MFVideoFormat_NV12))
            {
                SetNv12Pixel(buffer, pitch, height, x, y, targetY, targetU, targetV, amount);
            }
            else if (IsEqualGUID(subtype, MFVideoFormat_YUY2))
            {
                SetYuy2Pixel(buffer, pitch, x, y, targetY, targetU, targetV, amount);
            }
        }

        void CopyBackgroundPixel(
            _Inout_ BYTE* buffer,
            _In_ LONG pitch,
            _In_ UINT height,
            _In_ UINT targetX,
            _In_ UINT targetY,
            _In_ UINT sourceX,
            _In_ UINT sourceY,
            _In_ GUID const& subtype,
            _In_ float amount)
        {
            if (amount <= 0.0f) return;

            if (IsEqualGUID(subtype, MFVideoFormat_YUY2))
            {
                BYTE* row = buffer + targetY * static_cast<UINT>(pitch);
                const BYTE* sourceRow = buffer + sourceY * static_cast<UINT>(pitch);
                BYTE* targetPair = row + (targetX & ~1u) * 2;
                const BYTE* sourcePair = sourceRow + (sourceX & ~1u) * 2;
                const UINT targetYOffset = (targetX & 1u) == 0 ? 0 : 2;
                const UINT sourceYOffset = (sourceX & 1u) == 0 ? 0 : 2;
                targetPair[targetYOffset] = BlendByte(targetPair[targetYOffset], sourcePair[sourceYOffset], amount);
                targetPair[1] = BlendByte(targetPair[1], sourcePair[1], amount);
                targetPair[3] = BlendByte(targetPair[3], sourcePair[3], amount);
                return;
            }

            if (IsEqualGUID(subtype, MFVideoFormat_NV12))
            {
                const UINT stride = static_cast<UINT>(pitch);
                BYTE* yPlane = buffer;
                BYTE* uvPlane = buffer + stride * height;
                BYTE* targetYPixel = yPlane + targetY * stride + targetX;
                const BYTE* sourceYPixel = yPlane + sourceY * stride + sourceX;
                *targetYPixel = BlendByte(*targetYPixel, *sourceYPixel, amount);

                BYTE* targetUv = uvPlane + (targetY / 2) * stride + (targetX & ~1u);
                const BYTE* sourceUv = uvPlane + (sourceY / 2) * stride + (sourceX & ~1u);
                targetUv[0] = BlendByte(targetUv[0], sourceUv[0], amount);
                targetUv[1] = BlendByte(targetUv[1], sourceUv[1], amount);
            }
        }

        void ReadEffectPixel(
            _In_reads_bytes_(bufferLength) const BYTE* buffer,
            _In_ LONG pitch,
            _In_ UINT height,
            _In_ UINT x,
            _In_ UINT y,
            _In_ GUID const& subtype,
            _Out_ BYTE* pixelY,
            _Out_ BYTE* pixelU,
            _Out_ BYTE* pixelV)
        {
            if (IsEqualGUID(subtype, MFVideoFormat_YUY2))
            {
                const BYTE* pair = buffer + y * static_cast<UINT>(pitch) + (x & ~1u) * 2;
                *pixelY = pair[(x & 1u) == 0 ? 0 : 2];
                *pixelU = pair[1];
                *pixelV = pair[3];
                return;
            }

            const UINT stride = static_cast<UINT>(pitch);
            const BYTE* yPlane = buffer;
            const BYTE* uvPlane = buffer + stride * height;
            *pixelY = yPlane[y * stride + x];
            const BYTE* uv = uvPlane + (y / 2) * stride + (x & ~1u);
            *pixelU = uv[0];
            *pixelV = uv[1];
        }

        bool FindShushEyes(
            _In_reads_bytes_(bufferLength) const BYTE* buffer,
            _In_ DWORD bufferLength,
            _In_ LONG pitch,
            _In_ UINT width,
            _In_ UINT height,
            _In_ GUID const& subtype,
            _Out_ ShushEyePoint* leftEye,
            _Out_ ShushEyePoint* rightEye)
        {
            if (buffer == nullptr || leftEye == nullptr || rightEye == nullptr || pitch <= 0) return false;

            const UINT stride = static_cast<UINT>(pitch);
            const UINT required = IsEqualGUID(subtype, MFVideoFormat_NV12)
                ? stride * height + stride * (height / 2)
                : stride * height;
            if (bufferLength < required) return false;

            constexpr UINT step = 8;
            const UINT scanHeight = (height * 65) / 100;
            const UINT gridWidth = (width + step - 1) / step;
            const UINT gridHeight = (scanHeight + step - 1) / step;
            std::vector<BYTE> mask(gridWidth * gridHeight, 0);

            for (UINT gy = 0; gy < gridHeight; ++gy)
            {
                const UINT y = std::min(height - 1, gy * step + step / 2);
                for (UINT gx = 0; gx < gridWidth; ++gx)
                {
                    const UINT x = std::min(width - 1, gx * step + step / 2);
                    BYTE pixelY = 0;
                    BYTE pixelU = 128;
                    BYTE pixelV = 128;
                    ReadEffectPixel(buffer, pitch, height, x, y, subtype, &pixelY, &pixelU, &pixelV);
                    mask[gy * gridWidth + gx] = IsLikelySkin(pixelY, pixelU, pixelV) ? 1 : 0;
                }
            }

            std::vector<UINT> queue;
            queue.reserve(gridWidth * gridHeight);
            UINT faceMinX = 0;
            UINT faceMaxX = 0;
            UINT faceMinY = 0;
            UINT faceMaxY = 0;
            UINT bestArea = 0;
            float bestScore = 0.0f;

            for (UINT gy = 0; gy < gridHeight; ++gy)
            {
                for (UINT gx = 0; gx < gridWidth; ++gx)
                {
                    const UINT start = gy * gridWidth + gx;
                    if (mask[start] == 0) continue;

                    mask[start] = 0;
                    queue.clear();
                    queue.push_back(start);
                    UINT area = 0;
                    UINT minX = gx;
                    UINT maxX = gx;
                    UINT minY = gy;
                    UINT maxY = gy;

                    for (size_t index = 0; index < queue.size(); ++index)
                    {
                        const UINT cell = queue[index];
                        const UINT cx = cell % gridWidth;
                        const UINT cy = cell / gridWidth;
                        ++area;
                        minX = std::min(minX, cx);
                        maxX = std::max(maxX, cx);
                        minY = std::min(minY, cy);
                        maxY = std::max(maxY, cy);

                        const UINT neighbors[4] = {
                            cx > 0 ? cell - 1 : cell,
                            cx + 1 < gridWidth ? cell + 1 : cell,
                            cy > 0 ? cell - gridWidth : cell,
                            cy + 1 < gridHeight ? cell + gridWidth : cell,
                        };
                        for (UINT neighbor : neighbors)
                        {
                            if (neighbor == cell || mask[neighbor] == 0) continue;
                            mask[neighbor] = 0;
                            queue.push_back(neighbor);
                        }
                    }

                    const UINT boxWidth = (maxX - minX + 1) * step;
                    const UINT boxHeight = (maxY - minY + 1) * step;
                    const float centerX =
                        (static_cast<float>(minX + maxX + 1) * 0.5f * step) /
                        static_cast<float>(width);
                    const float top = static_cast<float>(minY * step) / static_cast<float>(height);
                    if (area < 35 || boxWidth < width / 12 || boxHeight < height / 12
                        || boxWidth > width * 0.62f || boxHeight > height * 0.58f
                        || centerX < 0.22f || centerX > 0.78f || top > 0.42f)
                    {
                        continue;
                    }

                    const float score = static_cast<float>(area) *
                        (1.0f - std::abs(centerX - 0.5f));
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestArea = area;
                        faceMinX = minX * step;
                        faceMaxX = std::min(width - 1, (maxX + 1) * step);
                        faceMinY = minY * step;
                        faceMaxY = std::min(height - 1, (maxY + 1) * step);
                    }
                }
            }

            if (bestArea == 0) return false;

            const int faceWidth = static_cast<int>(faceMaxX - faceMinX);
            const int faceHeight = static_cast<int>(faceMaxY - faceMinY);
            const int searchTop = static_cast<int>(faceMinY + faceHeight * 0.24f);
            const int searchBottom = static_cast<int>(faceMinY + faceHeight * 0.62f);
            const int searchStep = 4;
            const int windowX = std::max(12, faceWidth / 12);
            const int windowY = std::max(7, faceHeight / 18);

            auto findOneEye = [&](bool left, ShushEyePoint* result)
            {
                const int regionLeft = left
                    ? static_cast<int>(faceMinX + faceWidth * 0.12f)
                    : static_cast<int>(faceMinX + faceWidth * 0.52f);
                const int regionRight = left
                    ? static_cast<int>(faceMinX + faceWidth * 0.48f)
                    : static_cast<int>(faceMinX + faceWidth * 0.88f);
                int bestDarkScore = 0;
                int bestX = 0;
                int bestY = 0;
                int bestCount = 0;
                int bestSumX = 0;
                int bestSumY = 0;

                for (int centerY = searchTop; centerY <= searchBottom; centerY += searchStep)
                {
                    for (int centerX = regionLeft; centerX <= regionRight; centerX += searchStep)
                    {
                        int darkScore = 0;
                        int darkCount = 0;
                        int sumX = 0;
                        int sumY = 0;
                        for (int y = centerY - windowY; y <= centerY + windowY; y += 4)
                        {
                            for (int x = centerX - windowX; x <= centerX + windowX; x += 4)
                            {
                                if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) continue;
                                BYTE pixelY = 0;
                                BYTE pixelU = 128;
                                BYTE pixelV = 128;
                                ReadEffectPixel(
                                    buffer,
                                    pitch,
                                    height,
                                    static_cast<UINT>(x),
                                    static_cast<UINT>(y),
                                    subtype,
                                    &pixelY,
                                    &pixelU,
                                    &pixelV);
                                const int chromaDistance =
                                    std::abs(static_cast<int>(pixelU) - 128) +
                                    std::abs(static_cast<int>(pixelV) - 128);
                                if (pixelY < 112 && chromaDistance < 105)
                                {
                                    darkScore += 1;
                                    ++darkCount;
                                    sumX += x;
                                    sumY += y;
                                }
                            }
                        }

                        if (darkScore > bestDarkScore)
                        {
                            bestDarkScore = darkScore;
                            bestX = centerX;
                            bestY = centerY;
                            bestCount = darkCount;
                            bestSumX = sumX;
                            bestSumY = sumY;
                        }
                    }
                }

                if (bestDarkScore < 4 || bestCount == 0) return false;
                result->x = bestSumX / bestCount;
                result->y = bestSumY / bestCount;
                result->radiusX = std::clamp(faceWidth / 20, 10, 30);
                result->radiusY = std::clamp(faceHeight / 28, 6, 16);
                (void)bestX;
                (void)bestY;
                return true;
            };

            return findOneEye(true, leftEye) && findOneEye(false, rightEye);
        }

        bool FindShushEyesInFaceBox(
            _In_reads_bytes_(bufferLength) const BYTE* buffer,
            _In_ DWORD bufferLength,
            _In_ LONG pitch,
            _In_ UINT width,
            _In_ UINT height,
            _In_ GUID const& subtype,
            _In_ RECT const& faceRect,
            _Out_ ShushEyePoint* leftEye,
            _Out_ ShushEyePoint* rightEye)
        {
            if (buffer == nullptr || leftEye == nullptr || rightEye == nullptr || pitch <= 0) return false;
            const UINT stride = static_cast<UINT>(pitch);
            const UINT required = IsEqualGUID(subtype, MFVideoFormat_NV12)
                ? stride * height + stride * (height / 2)
                : stride * height;
            if (bufferLength < required) return false;

            const int faceMinX = std::clamp<LONG>(faceRect.left, 0, static_cast<LONG>(width));
            const int faceMaxX = std::clamp<LONG>(faceRect.right, 0, static_cast<LONG>(width));
            const int faceMinY = std::clamp<LONG>(faceRect.top, 0, static_cast<LONG>(height));
            const int faceMaxY = std::clamp<LONG>(faceRect.bottom, 0, static_cast<LONG>(height));
            const int faceWidth = faceMaxX - faceMinX;
            const int faceHeight = faceMaxY - faceMinY;
            if (faceWidth < 32 || faceHeight < 32) return false;

            const int searchTop = static_cast<int>(faceMinY + faceHeight * 0.20f);
            const int searchBottom = static_cast<int>(faceMinY + faceHeight * 0.52f);
            const int searchStep = 4;
            const int windowX = std::max(10, faceWidth / 12);
            const int windowY = std::max(6, faceHeight / 18);

            auto findOneEye = [&](bool left, ShushEyePoint* result)
            {
                const int regionLeft = left
                    ? static_cast<int>(faceMinX + faceWidth * 0.10f)
                    : static_cast<int>(faceMinX + faceWidth * 0.52f);
                const int regionRight = left
                    ? static_cast<int>(faceMinX + faceWidth * 0.48f)
                    : static_cast<int>(faceMinX + faceWidth * 0.90f);
                int bestDarkScore = 0;
                int bestCount = 0;
                int bestSumX = 0;
                int bestSumY = 0;

                for (int centerY = searchTop; centerY <= searchBottom; centerY += searchStep)
                {
                    for (int centerX = regionLeft; centerX <= regionRight; centerX += searchStep)
                    {
                        int darkScore = 0;
                        int darkCount = 0;
                        int sumX = 0;
                        int sumY = 0;
                        for (int y = centerY - windowY; y <= centerY + windowY; y += 3)
                        {
                            for (int x = centerX - windowX; x <= centerX + windowX; x += 3)
                            {
                                if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) continue;
                                BYTE pixelY = 0;
                                BYTE pixelU = 128;
                                BYTE pixelV = 128;
                                ReadEffectPixel(
                                    buffer,
                                    pitch,
                                    height,
                                    static_cast<UINT>(x),
                                    static_cast<UINT>(y),
                                    subtype,
                                    &pixelY,
                                    &pixelU,
                                    &pixelV);
                                const int chromaDistance =
                                    std::abs(static_cast<int>(pixelU) - 128) +
                                    std::abs(static_cast<int>(pixelV) - 128);
                                if (pixelY < 118 && chromaDistance < 115)
                                {
                                    ++darkScore;
                                    ++darkCount;
                                    sumX += x;
                                    sumY += y;
                                }
                            }
                        }

                        if (darkScore > bestDarkScore)
                        {
                            bestDarkScore = darkScore;
                            bestCount = darkCount;
                            bestSumX = sumX;
                            bestSumY = sumY;
                        }
                    }
                }

                if (bestDarkScore < 5 || bestCount == 0) return false;
                result->x = bestSumX / bestCount;
                result->y = bestSumY / bestCount;
                result->radiusX = std::clamp(faceWidth / 18, 10, 42);
                result->radiusY = std::clamp(faceHeight / 32, 6, 20);
                return true;
            };

            return findOneEye(true, leftEye) && findOneEye(false, rightEye);
        }

        void FillBodyWithNearbyBackground(
            _Inout_ BYTE* buffer,
            _In_ LONG pitch,
            _In_ UINT height,
            _In_ UINT width,
            _In_ GUID const& subtype,
            _In_ float amount)
        {
            if (amount <= 0.0f) return;

            for (UINT y = 0; y < height; ++y)
            {
                UINT left = width;
                UINT right = 0;
                for (UINT x = 0; x < width; ++x)
                {
                    if (BodyMaskCoverage(x, y, width, height) < 0.05f) continue;
                    left = std::min(left, x);
                    right = std::max(right, x);
                }

                if (left == width || right <= left) continue;

                const UINT leftSource = left > 24 ? left - 24 : 0;
                const UINT rightSource = right + 24 < width ? right + 24 : width - 1;
                BYTE leftY = 0;
                BYTE leftU = 128;
                BYTE leftV = 128;
                BYTE rightY = 0;
                BYTE rightU = 128;
                BYTE rightV = 128;
                ReadEffectPixel(buffer, pitch, height, leftSource, y, subtype, &leftY, &leftU, &leftV);
                ReadEffectPixel(buffer, pitch, height, rightSource, y, subtype, &rightY, &rightU, &rightV);

                for (UINT x = left; x <= right; ++x)
                {
                    const float mask = BodyMaskCoverage(x, y, width, height) * amount;
                    if (mask <= 0.0f) continue;
                    const float t = static_cast<float>(x - left) / static_cast<float>(std::max(1u, right - left));
                    const BYTE fillY = static_cast<BYTE>(leftY + (rightY - leftY) * t);
                    const BYTE fillU = static_cast<BYTE>(leftU + (rightU - leftU) * t);
                    const BYTE fillV = static_cast<BYTE>(leftV + (rightV - leftV) * t);
                    SetEffectPixel(buffer, pitch, height, x, y, subtype, fillY, fillU, fillV, mask);
                }
            }
        }

        bool IsDispersionSelfTestEnabled()
        {
            return GetFileAttributesW(L"C:\\ProgramData\\MeetGestureFX\\meet-gesture-fx-force.txt") != INVALID_FILE_ATTRIBUTES;
        }

        void AppendRuntimeLog(_In_ LPCWSTR message)
        {
            HANDLE file = CreateFileW(
                L"C:\\ProgramData\\MeetGestureFX\\meet-gesture-fx-runtime.log",
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            DWORD written = 0;
            WriteFile(file, message, static_cast<DWORD>(wcslen(message) * sizeof(wchar_t)), &written, nullptr);
            WriteFile(file, L"\r\n", 4, &written, nullptr);
            CloseHandle(file);
        }

        void DrawCrow(
            _Inout_ BYTE* buffer,
            _In_ LONG pitch,
            _In_ UINT height,
            _In_ UINT width,
            _In_ GUID const& subtype,
            _In_ const CrowParticle& crow,
            _In_ float alpha)
        {
            if (alpha <= 0.0f) return;
            const int centerX = static_cast<int>(crow.x);
            const int centerY = static_cast<int>(crow.y);
            const int size = std::max(8, static_cast<int>(crow.scale));
            const int wing = std::max(size * 2, size * 2 + static_cast<int>(std::sin(crow.phase) * size));
            const int thickness = std::max(1, size / 12);

            auto paint = [&](int x, int y, BYTE targetY, BYTE targetU, BYTE targetV)
            {
                for (int dy = -thickness; dy <= thickness; ++dy)
                {
                    for (int dx = -thickness; dx <= thickness; ++dx)
                    {
                        const int px = x + dx;
                        const int py = y + dy;
                        if (px < 0 || py < 0 || px >= static_cast<int>(width) || py >= static_cast<int>(height)) continue;
                        SetEffectPixel(buffer, pitch, height, static_cast<UINT>(px), static_cast<UINT>(py), subtype, targetY, targetU, targetV, alpha);
                    }
                }
            };

            auto line = [&](int x0, int y0, int x1, int y1)
            {
                const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
                for (int i = 0; i <= steps; ++i)
                {
                    const float t = steps == 0 ? 0.0f : static_cast<float>(i) / static_cast<float>(steps);
                    paint(
                        static_cast<int>(x0 + (x1 - x0) * t),
                        static_cast<int>(y0 + (y1 - y0) * t),
                        16,
                        128,
                        128);
                }
            };

            // Large, readable crow silhouette: filled body/head plus broad
            // angular wings and a tapered tail.
            paint(centerX, centerY, 16, 128, 128);
            paint(centerX + size, centerY - size / 3, 16, 128, 128);
            line(centerX - size, centerY, centerX + size, centerY);
            line(centerX - size / 2, centerY, centerX - wing, centerY - size * 2);
            line(centerX - size / 2, centerY, centerX - wing + size / 2, centerY + size);
            line(centerX + size / 2, centerY, centerX + wing, centerY - size * 3 / 2);
            line(centerX + size / 2, centerY, centerX + wing - size / 2, centerY + size / 2);
            line(centerX - size, centerY, centerX - size * 2, centerY + size);

            const int eyeX = centerX + size + std::max(1, size / 3);
            const int eyeY = centerY - size / 2;
            paint(eyeX, eyeY, 76, 84, 255);
        }

        // Bring-up marker for the first end-to-end milestone. This deliberately
        // stays in NV12 so the virtual camera can be tested by Windows Camera
        // and Google Meet before adding hand tracking or a GPU compositor.
        HRESULT DrawMeetGestureFxMarker(
            _Inout_updates_bytes_(bufferLength) BYTE* buffer,
            _In_ DWORD bufferLength,
            _In_ LONG pitch,
            _In_ UINT width,
            _In_ UINT height,
            _In_ REFGUID subtype)
        {
            RETURN_HR_IF_NULL(E_INVALIDARG, buffer);
            if (pitch <= 0 || width == 0 || height == 0)
            {
                return S_OK;
            }

            const DWORD stride = static_cast<DWORD>(pitch);
            const bool isNv12 = IsEqualGUID(subtype, MFVideoFormat_NV12);
            const bool isYuy2 = IsEqualGUID(subtype, MFVideoFormat_YUY2);
            if (!isNv12 && !isYuy2)
            {
                return MF_E_INVALIDMEDIATYPE;
            }

            const DWORD required = isNv12
                ? stride * height + stride * (height / 2)
                : stride * height;
            RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), bufferLength < required);

            UINT border = (width < height ? width : height) / 28;
            if (border < 4) border = 4;

            if (isYuy2)
            {
                // YUY2 stores two pixels as Y0 U Y1 V.
                for (UINT y = 0; y < height; ++y)
                {
                    const bool horizontalBorder = y < border || y >= height - border;
                    BYTE* row = buffer + y * stride;
                    for (UINT x = 0; x < width; x += 2)
                    {
                        const bool marked = horizontalBorder
                            || x < border
                            || x + 1 >= width - border;
                        if (marked)
                        {
                            row[x * 2] = 235;
                            row[x * 2 + 1] = 128;
                            row[x * 2 + 2] = 235;
                            row[x * 2 + 3] = 128;
                        }
                    }
                }
                return S_OK;
            }

            BYTE* yPlane = buffer;
            BYTE* uvPlane = buffer + stride * height;
            // Bright border: it is easy to identify in a remote Meet call and
            // does not depend on a particular RGB conversion path.
            for (UINT y = 0; y < height; ++y)
            {
                const bool horizontalBorder = y < border || y >= height - border;
                BYTE* yRow = yPlane + y * stride;
                for (UINT x = 0; x < width; ++x)
                {
                    if (horizontalBorder || x < border || x >= width - border)
                    {
                        yRow[x] = 235;
                    }
                }
            }

            // Neutral chroma keeps the marker white in NV12.
            for (UINT y = 0; y < height / 2; ++y)
            {
                BYTE* uvRow = uvPlane + y * stride;
                for (UINT x = 0; x < width; x += 2)
                {
                    const bool horizontalBorder = y < border / 2 || y >= (height / 2) - (border / 2);
                    if (horizontalBorder || x < border || x >= width - border)
                    {
                        uvRow[x] = 128;
                        if (x + 1 < stride) uvRow[x + 1] = 128;
                    }
                }
            }

            return S_OK;
        }
    }

    /// <summary>
    /// Helper method to clone a MediaType
    /// </summary>
    /// <param name="pSourceMediaType"></param>
    /// <param name="ppDestMediaType"></param>
    /// <returns></returns>
    HRESULT CloneMediaType(_In_ IMFMediaType* pSourceMediaType, _Out_ IMFMediaType** ppDestMediaType)
    {
        wil::com_ptr_nothrow<IMFVideoMediaType> spVideoMT;

        RETURN_HR_IF_NULL(E_INVALIDARG, pSourceMediaType);
        RETURN_HR_IF_NULL(E_POINTER, ppDestMediaType);

        *ppDestMediaType = nullptr;

        if (SUCCEEDED(pSourceMediaType->QueryInterface(IID_PPV_ARGS(&spVideoMT))))
        {
            wil::com_ptr_nothrow<IMFVideoMediaType> spClonedVideoMT;

            RETURN_IF_FAILED(MFCreateVideoMediaType(spVideoMT->GetVideoFormat(), &spClonedVideoMT));
            RETURN_IF_FAILED(spVideoMT->CopyAllItems(spClonedVideoMT.get()));
            RETURN_IF_FAILED(spClonedVideoMT->QueryInterface(IID_PPV_ARGS(ppDestMediaType)));
        }
        else
        {
            wil::com_ptr_nothrow<IMFMediaType> spClonedMT;

            RETURN_IF_FAILED(MFCreateMediaType(&spClonedMT));
            RETURN_IF_FAILED(pSourceMediaType->CopyAllItems(spClonedMT.get()));
            RETURN_IF_FAILED(spClonedMT->QueryInterface(IID_PPV_ARGS(ppDestMediaType)));
        }

        return S_OK;
    }

    AugmentedMediaStream::~AugmentedMediaStream()
    {
        Shutdown();
    }

    /// <summary>
    /// Initialize the stream. This is where we filter the MediaTypes exposed out of this stream.
    /// </summary>
    /// <param name="pSource"></param>
    /// <param name="dwStreamId"></param>
    /// <param name="pStreamDesc"></param>
    /// <param name="dwWorkQueue"></param>
    /// <returns></returns>
    HRESULT AugmentedMediaStream::Initialize(
        _In_ AugmentedMediaSource* pSource, 
        _In_ DWORD dwStreamId, 
        _In_ IMFStreamDescriptor* pStreamDesc, 
        _In_ DWORD /*dwWorkQueue*/ )
    {
        winrt::slim_lock_guard lock(m_Lock);

        DEBUG_MSG(L"AugmentedMediaStream Initialize enter");
        
        RETURN_HR_IF_NULL(E_INVALIDARG, pSource);
        RETURN_HR_IF_NULL(E_INVALIDARG, pStreamDesc);
        
        m_parent = pSource;
        m_dwStreamId = dwStreamId;
        
        // look at stream descriptor, extract list of MediaTypes and filter to preserve only supported MediaTypes
        wil::com_ptr_nothrow<IMFStreamDescriptor> spStreamDesc = pStreamDesc;
        RETURN_IF_FAILED(spStreamDesc->GetStreamIdentifier(&m_dwDevSourceStreamIdentifier));

        wil::com_ptr_nothrow<IMFMediaTypeHandler> spSourceStreamMediaTypeHandler;
        RETURN_IF_FAILED(spStreamDesc->GetMediaTypeHandler(&spSourceStreamMediaTypeHandler));
        
        ULONG ulMediaTypeCount = 0;
        ULONG validMediaTypeCount = 0;
        RETURN_IF_FAILED(spSourceStreamMediaTypeHandler->GetMediaTypeCount(&ulMediaTypeCount));
        wil::unique_cotaskmem_array_ptr<wil::com_ptr_nothrow<IMFMediaType>> sourceStreamMediaTypeList = 
            wilEx::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(ulMediaTypeCount);
        RETURN_IF_NULL_ALLOC(sourceStreamMediaTypeList.get());

        for (DWORD i = 0; i < ulMediaTypeCount; i++)
        {
            DEBUG_MSG(L"Looking at MediaType number=%u", i);
            wil::com_ptr_nothrow<IMFMediaType> spMediaType;
            RETURN_IF_FAILED(spSourceStreamMediaTypeHandler->GetMediaTypeByIndex(i, &spMediaType));
            
            GUID majorType;
            spMediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);

            GUID subtype;
            spMediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
            
            UINT width = 0, height = 0;
            MFGetAttributeSize(spMediaType.get(), MF_MT_FRAME_SIZE, &width, &height);

            UINT numerator = 0, denominator = 0;
            MFGetAttributeRatio(spMediaType.get(), MF_MT_FRAME_RATE, &numerator, &denominator);
            int framerate = (int)((float)numerator / denominator + 0.5f); //fps

            // Check if this MediaType conforms to our heuristic we decided to filter with:
            // 1- is it a Video MediaType
            // 2- is it a NV12 subtype
            // 3- is its width between [1, 1920] and its height between [1, 1080]
            // 4- is its framerate less than or equal to 30 fps and above or equal to 15fps
            if (IsEqualGUID(majorType, MFMediaType_Video)
                && (IsEqualGUID(subtype, MFVideoFormat_NV12) || IsEqualGUID(subtype, MFVideoFormat_YUY2))
                && ((width <= 1920 && width > 0) && (height <= 1080 && height > 0))
                && (framerate <= 30 && framerate >= 15))
            {
                DEBUG_MSG(L"Found a valid and compliant Mediatype=%u", i);

                // copy the MediaType and cache it
                wil::com_ptr_nothrow<IMFMediaType> spMediaTypeCopy;
                RETURN_IF_FAILED(CloneMediaType(spMediaType.get(), &spMediaTypeCopy));
                m_mediaTypeCache.push_back(spMediaTypeCopy);
                sourceStreamMediaTypeList[validMediaTypeCount] = spMediaTypeCopy.detach();
                validMediaTypeCount++;
            }
        }
        if (validMediaTypeCount == 0)
        {
            DEBUG_MSG(L"Did not find any valid compliant Mediatype");
            return MF_E_INVALIDMEDIATYPE;
        }

        wil::unique_cotaskmem_array_ptr<wil::com_ptr_nothrow<IMFMediaType>> m_mediaTypeList = 
            wilEx::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(validMediaTypeCount);
        RETURN_IF_NULL_ALLOC(m_mediaTypeList.get());

        for (DWORD i = 0; i < validMediaTypeCount; i++)
        {
            wil::com_ptr_nothrow<IMFMediaType> spSrcStreamMediaType = sourceStreamMediaTypeList[i];
            if(spSrcStreamMediaType != nullptr)
            {
                m_mediaTypeList[i] = spSrcStreamMediaType.detach();
            }
        }

        RETURN_IF_FAILED(MFCreateAttributes(&m_spAttributes, 10));
        RETURN_IF_FAILED(spStreamDesc->CopyAllItems(m_spAttributes.get()));
        RETURN_IF_FAILED(_SetStreamAttributes(m_spAttributes.get()));
        
        wil::com_ptr_nothrow<IMFMediaTypeHandler> spTypeHandler;
        
        // create our stream descriptor
        RETURN_IF_FAILED(MFCreateStreamDescriptor(
            m_dwStreamId /*StreamId*/, 
            validMediaTypeCount /*MT count*/, 
            m_mediaTypeList.get(), &m_spStreamDesc));
        RETURN_IF_FAILED(m_spStreamDesc->GetMediaTypeHandler(&spTypeHandler));
        RETURN_IF_FAILED(spTypeHandler->SetCurrentMediaType(m_mediaTypeList[0]));

        // set same attributes on stream descriptor as on stream attribute store
        RETURN_IF_FAILED(spStreamDesc->CopyAllItems(m_spStreamDesc.get()));
        RETURN_IF_FAILED(_SetStreamAttributes(m_spStreamDesc.get())); 

        RETURN_IF_FAILED(MFCreateEventQueue(&m_spEventQueue));
        RETURN_IF_FAILED(MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &m_dwSerialWorkQueueId));
        auto ptr = winrt::make_self<CAsyncCallback<AugmentedMediaStream>>(this, &AugmentedMediaStream::OnMediaStreamEvent, m_dwSerialWorkQueueId);
        m_xOnMediaStreamEvent.attach(ptr.detach());

        DEBUG_MSG(L"Initialize exit | stream id: %d | dev source streamId: %d | MediaType count: %u", m_dwStreamId, m_dwDevSourceStreamIdentifier, validMediaTypeCount);
        return S_OK;
    }

    bool AugmentedMediaStream::EnsureCrowSpriteLoaded()
    {
        if (m_crowSpriteLoadAttempted)
        {
            return !m_crowSpritePixels.empty();
        }

        m_crowSpriteLoadAttempted = true;
        wil::com_ptr_nothrow<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            AppendRuntimeLog(L"Meet Gesture FX crow sprite: WIC factory unavailable");
            return false;
        }

        wil::com_ptr_nothrow<IWICBitmapDecoder> decoder;
        WCHAR spritePath[MAX_PATH] = {};
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&AppendRuntimeLog),
            &module))
        {
            AppendRuntimeLog(L"Meet Gesture FX crow sprite: module path unavailable");
            return false;
        }
        const DWORD modulePathLength = GetModuleFileNameW(module, spritePath, ARRAYSIZE(spritePath));
        if (modulePathLength == 0 || modulePathLength >= ARRAYSIZE(spritePath))
        {
            AppendRuntimeLog(L"Meet Gesture FX crow sprite: module filename unavailable");
            return false;
        }
        WCHAR* slash = wcsrchr(spritePath, L'\\');
        if (slash == nullptr)
        {
            AppendRuntimeLog(L"Meet Gesture FX crow sprite: module directory unavailable");
            return false;
        }
        wcscpy_s(slash + 1, ARRAYSIZE(spritePath) - static_cast<size_t>(slash + 1 - spritePath), L"crow_public_domain.png");

        hr = factory->CreateDecoderFromFilename(
            spritePath,
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder);
        if (FAILED(hr))
        {
            AppendRuntimeLog(L"Meet Gesture FX crow sprite: asset file unavailable");
            return false;
        }

        wil::com_ptr_nothrow<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) return false;

        wil::com_ptr_nothrow<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return false;
        hr = converter->Initialize(
            frame.get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return false;

        UINT width = 0;
        UINT height = 0;
        RETURN_IF_FAILED(converter->GetSize(&width, &height));
        if (width == 0 || height == 0 || width > 4096 || height > 4096) return false;

        m_crowSpriteWidth = width;
        m_crowSpriteHeight = height;
        m_crowSpritePixels.resize(static_cast<size_t>(width) * height * 4);
        hr = converter->CopyPixels(
            nullptr,
            width * 4,
            static_cast<UINT>(m_crowSpritePixels.size()),
            m_crowSpritePixels.data());
        if (FAILED(hr))
        {
            m_crowSpritePixels.clear();
            return false;
        }

        AppendRuntimeLog(L"Meet Gesture FX crow sprite: loaded realistic alpha sprite");
        return true;
    }

    void AugmentedMediaStream::DrawCrowSprite(
        _Inout_ BYTE* buffer,
        _In_ LONG pitch,
        _In_ GUID const& subtype,
        _In_ const CrowParticle& crow,
        _In_ float alpha)
    {
        if (alpha <= 0.0f || m_crowSpritePixels.empty() || m_crowSpriteWidth == 0 || m_crowSpriteHeight == 0)
        {
            return;
        }

        const float spriteHeight = std::max(72.0f, crow.scale);
        const float imageScale = spriteHeight / static_cast<float>(m_crowSpriteHeight);
        const float spriteWidth = static_cast<float>(m_crowSpriteWidth) * imageScale;
        const float angle = std::atan2(crow.vy, crow.vx);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const int minX = static_cast<int>(crow.x - spriteWidth * 0.5f) - 1;
        const int maxX = static_cast<int>(crow.x + spriteWidth * 0.5f) + 1;
        const int minY = static_cast<int>(crow.y - spriteHeight * 0.5f) - 1;
        const int maxY = static_cast<int>(crow.y + spriteHeight * 0.5f) + 1;

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const float dx = static_cast<float>(x) - crow.x;
                const float dy = static_cast<float>(y) - crow.y;
                const float localX = (dx * c + dy * s) / imageScale + static_cast<float>(m_crowSpriteWidth) * 0.5f;
                const float localY = (-dx * s + dy * c) / imageScale + static_cast<float>(m_crowSpriteHeight) * 0.5f;
                const int sourceX = static_cast<int>(localX);
                const int sourceY = static_cast<int>(localY);
                if (sourceX < 0 || sourceY < 0
                    || sourceX >= static_cast<int>(m_crowSpriteWidth)
                    || sourceY >= static_cast<int>(m_crowSpriteHeight)
                    || x < 0 || y < 0
                    || x >= static_cast<int>(m_width)
                    || y >= static_cast<int>(m_height))
                {
                    continue;
                }

                const size_t offset = (static_cast<size_t>(sourceY) * m_crowSpriteWidth + sourceX) * 4;
                const BYTE sourceAlpha = m_crowSpritePixels[offset + 3];
                if (sourceAlpha < 16) continue;

                const float pixelAlpha = alpha * static_cast<float>(sourceAlpha) / 255.0f;
                SetEffectPixel(buffer, pitch, m_height, static_cast<UINT>(x), static_cast<UINT>(y), subtype, 16, 128, 128, pixelAlpha);
            }
        }

        // The source silhouette has no eye detail; add one consistent red eye
        // on the head so the real feather outline remains readable in Meet.
        const float eyeLocalX = static_cast<float>(m_crowSpriteWidth) * (0.70f - 0.50f) * imageScale;
        const float eyeLocalY = static_cast<float>(m_crowSpriteHeight) * (0.47f - 0.50f) * imageScale;
        const int eyeX = static_cast<int>(crow.x + eyeLocalX * c - eyeLocalY * s);
        const int eyeY = static_cast<int>(crow.y + eyeLocalX * s + eyeLocalY * c);
        for (int dy = -2; dy <= 2; ++dy)
        {
            for (int dx = -2; dx <= 2; ++dx)
            {
                if (dx * dx + dy * dy > 4) continue;
                const int x = eyeX + dx;
                const int y = eyeY + dy;
                if (x < 0 || y < 0 || x >= static_cast<int>(m_width) || y >= static_cast<int>(m_height)) continue;
                SetEffectPixel(buffer, pitch, m_height, static_cast<UINT>(x), static_cast<UINT>(y), subtype, 76, 84, 255, alpha);
            }
        }
    }

    bool AugmentedMediaStream::ReadFaceLandmarksFromSample(_In_ IMFSample* inputSample)
    {
        m_hasFaceLandmarks = false;
        if (inputSample == nullptr) return false;

        wil::com_ptr_nothrow<IMFAttributes> captureMetadata;
        if (FAILED(inputSample->GetUnknown(
                MFSampleExtension_CaptureMetadata,
                IID_PPV_ARGS(&captureMetadata))))
        {
            return false;
        }

        UINT32 blobSize = 0;
        if (FAILED(captureMetadata->GetBlobSize(kMfWindowsStudioMetadataFaceLandmarks, &blobSize))
            || blobSize < sizeof(WseFaceMetadataHeader))
        {
            return false;
        }

        std::vector<BYTE> blob(blobSize);
        if (FAILED(captureMetadata->GetBlob(
                kMfWindowsStudioMetadataFaceLandmarks,
                blob.data(),
                blobSize,
                &blobSize)))
        {
            return false;
        }

        WseFaceMetadataHeader header = {};
        memcpy(&header, blob.data(), sizeof(header));
        if (header.count == 0 || header.count > 8 || header.size > blobSize)
        {
            return false;
        }

        const size_t recordsOffset = sizeof(WseFaceMetadataHeader);
        const size_t recordSize = sizeof(WseFaceLandmarksMetadata);
        if (recordsOffset + static_cast<size_t>(header.count) * recordSize > blobSize)
        {
            return false;
        }

        float bestScore = 0.0f;
        ShushEyePoint bestLeft;
        ShushEyePoint bestRight;
        for (UINT32 faceIndex = 0; faceIndex < header.count; ++faceIndex)
        {
            WseFaceLandmarksMetadata face = {};
            memcpy(
                &face,
                blob.data() + recordsOffset + static_cast<size_t>(faceIndex) * recordSize,
                sizeof(face));

            auto point = [&](UINT32 index)
            {
                return std::pair<float, float>{
                    std::clamp(face.landmarks[index][0], 0.0f, 1.0f),
                    std::clamp(face.landmarks[index][1], 0.0f, 1.0f) };
            };

            auto makeEye = [&](UINT32 first, UINT32 last, UINT32 irisIndex, ShushEyePoint* result)
            {
                float minX = 1.0f;
                float maxX = 0.0f;
                float minY = 1.0f;
                float maxY = 0.0f;
                float sumX = 0.0f;
                float sumY = 0.0f;
                float confidence = 0.0f;
                UINT32 pointCount = 0;
                for (UINT32 index = first; index <= last; ++index)
                {
                    const auto [x, y] = point(index);
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                    sumX += x;
                    sumY += y;
                    confidence += face.confidence[index];
                    ++pointCount;
                }

                if (pointCount == 0 || maxX <= minX || maxY <= minY) return 0.0f;
                confidence /= static_cast<float>(pointCount);

                auto iris = point(irisIndex);
                if (face.confidence[irisIndex] < 0.20f
                    || iris.first < minX || iris.first > maxX
                    || iris.second < minY || iris.second > maxY)
                {
                    iris = {
                        sumX / static_cast<float>(pointCount),
                        sumY / static_cast<float>(pointCount) };
                }

                result->x = static_cast<int>(iris.first * static_cast<float>(m_width) + 0.5f);
                result->y = static_cast<int>(iris.second * static_cast<float>(m_height) + 0.5f);
                result->radiusX = std::clamp(
                    static_cast<int>((maxX - minX) * static_cast<float>(m_width) * 0.48f),
                    8,
                    48);
                result->radiusY = std::clamp(
                    static_cast<int>((maxY - minY) * static_cast<float>(m_height) * 0.75f),
                    5,
                    24);
                return confidence;
            };

            ShushEyePoint left;
            ShushEyePoint right;
            const float leftConfidence = makeEye(kWseLeftEyeFirst, kWseLeftEyeLast, kWseLeftIris, &left);
            const float rightConfidence = makeEye(kWseRightEyeFirst, kWseRightEyeLast, kWseRightIris, &right);
            if (leftConfidence < 0.20f || rightConfidence < 0.20f) continue;

            const float center = static_cast<float>(left.x + right.x) /
                (2.0f * static_cast<float>(m_width));
            const float score = std::min(leftConfidence, rightConfidence) *
                (1.0f - std::abs(center - 0.5f));
            if (score > bestScore)
            {
                bestScore = score;
                bestLeft = left;
                bestRight = right;
            }
        }

        if (bestScore < 0.20f) return false;
        m_faceLeftEye = bestLeft;
        m_faceRightEye = bestRight;
        m_hasFaceLandmarks = true;
        return true;
    }

    bool AugmentedMediaStream::UpdateFaceBoxFromFrame(
        _In_reads_bytes_(bufferLength) const BYTE* buffer,
        _In_ DWORD bufferLength,
        _In_ LONG pitch,
        _In_ GUID const& subtype)
    {
        if (buffer == nullptr || pitch <= 0 || m_width == 0 || m_height == 0) return false;

        if (!m_faceDetectorAttempted)
        {
            m_faceDetectorAttempted = true;
            try
            {
                m_faceDetector = winrt::Windows::Media::FaceAnalysis::FaceDetector::CreateAsync().get();
                if (m_faceDetector != nullptr)
                {
                    AppendRuntimeLog(L"Meet Gesture FX face detector initialized");
                }
            }
            catch (...)
            {
                AppendRuntimeLog(L"Meet Gesture FX face detector unavailable");
                m_faceDetector = nullptr;
            }
        }

        if (m_faceDetector == nullptr) return false;

        if (m_faceDetectorSkipFrames > 0)
        {
            --m_faceDetectorSkipFrames;
            return m_hasFaceBox;
        }
        m_faceDetectorSkipFrames = 4;

        constexpr UINT32 kDetectionWidth = 320;
        const UINT32 detectionHeight = std::max<UINT32>(96, (m_height * kDetectionWidth) / std::max<UINT32>(1, m_width));
        const UINT required = IsEqualGUID(subtype, MFVideoFormat_NV12)
            ? static_cast<UINT>(pitch) * m_height + static_cast<UINT>(pitch) * (m_height / 2)
            : static_cast<UINT>(pitch) * m_height;
        if (bufferLength < required) return false;

        try
        {
            auto bitmap = winrt::Windows::Graphics::Imaging::SoftwareBitmap(
                winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                kDetectionWidth,
                detectionHeight,
                winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);
            auto bitmapBuffer = bitmap.LockBuffer(
                winrt::Windows::Graphics::Imaging::BitmapBufferAccessMode::Write);
            auto plane = bitmapBuffer.GetPlaneDescription(0);
            auto reference = bitmapBuffer.CreateReference();
            auto byteAccess = reference.as<::Windows::Foundation::IMemoryBufferByteAccess>();
            BYTE* pixels = nullptr;
            UINT32 capacity = 0;
            winrt::check_hresult(byteAccess->GetBuffer(&pixels, &capacity));
            if (pixels == nullptr || capacity < static_cast<UINT32>(plane.StartIndex + plane.Stride * detectionHeight))
            {
                return false;
            }

            auto clampByte = [](int value) -> BYTE {
                return static_cast<BYTE>(std::clamp(value, 0, 255));
            };

            for (UINT y = 0; y < detectionHeight; ++y)
            {
                BYTE* row = pixels + plane.StartIndex + y * plane.Stride;
                const UINT sourceY = std::min(m_height - 1, (y * m_height) / detectionHeight);
                for (UINT x = 0; x < kDetectionWidth; ++x)
                {
                    const UINT sourceX = std::min(m_width - 1, (x * m_width) / kDetectionWidth);
                    BYTE pixelY = 0;
                    BYTE pixelU = 128;
                    BYTE pixelV = 128;
                    ReadEffectPixel(buffer, pitch, m_height, sourceX, sourceY, subtype, &pixelY, &pixelU, &pixelV);
                    const int c = std::max(0, static_cast<int>(pixelY) - 16);
                    const int d = static_cast<int>(pixelU) - 128;
                    const int e = static_cast<int>(pixelV) - 128;
                    BYTE* pixel = row + x * 4;
                    pixel[0] = clampByte((298 * c + 516 * d + 128) >> 8);
                    pixel[1] = clampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
                    pixel[2] = clampByte((298 * c + 409 * e + 128) >> 8);
                    pixel[3] = 255;
                }
            }

            auto faces = m_faceDetector.DetectFacesAsync(bitmap).get();
            if (faces == nullptr || faces.Size() == 0) return false;

            float bestArea = 0.0f;
            RECT bestRect{};
            for (UINT32 i = 0; i < faces.Size(); ++i)
            {
                const auto faceBox = faces.GetAt(i).FaceBox();
                const LONG left = std::clamp<LONG>(
                    static_cast<LONG>(faceBox.X * static_cast<float>(m_width) / static_cast<float>(kDetectionWidth)),
                    0,
                    static_cast<LONG>(m_width));
                const LONG top = std::clamp<LONG>(
                    static_cast<LONG>(faceBox.Y * static_cast<float>(m_height) / static_cast<float>(detectionHeight)),
                    0,
                    static_cast<LONG>(m_height));
                const LONG right = std::clamp<LONG>(
                    static_cast<LONG>((faceBox.X + faceBox.Width) * static_cast<float>(m_width) / static_cast<float>(kDetectionWidth)),
                    0,
                    static_cast<LONG>(m_width));
                const LONG bottom = std::clamp<LONG>(
                    static_cast<LONG>((faceBox.Y + faceBox.Height) * static_cast<float>(m_height) / static_cast<float>(detectionHeight)),
                    0,
                    static_cast<LONG>(m_height));
                const float area = static_cast<float>(std::max<LONG>(0, right - left)) *
                    static_cast<float>(std::max<LONG>(0, bottom - top));
                if (area > bestArea)
                {
                    bestArea = area;
                    bestRect = { left, top, right, bottom };
                }
            }

            if (bestArea <= 0.0f) return false;
            m_faceBox = bestRect;
            m_hasFaceBox = true;
            if (!m_faceBoxLogWritten)
            {
                AppendRuntimeLog(L"Meet Gesture FX face box detected for eye filter");
                m_faceBoxLogWritten = true;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void AugmentedMediaStream::ClosePythonEyeWorker()
    {
        if (m_pythonEyeSocket != INVALID_SOCKET)
        {
            shutdown(m_pythonEyeSocket, SD_BOTH);
            closesocket(m_pythonEyeSocket);
            m_pythonEyeSocket = INVALID_SOCKET;
        }
        if (m_winsockInitialized)
        {
            WSACleanup();
            m_winsockInitialized = false;
        }
    }

    bool AugmentedMediaStream::QueryPythonEyeWorker(
        _In_reads_bytes_(bufferLength) const BYTE* buffer,
        _In_ DWORD bufferLength,
        _In_ LONG pitch,
        _In_ GUID const& subtype)
    {
        if (buffer == nullptr || pitch <= 0 || m_width == 0 || m_height == 0
            || (!IsEqualGUID(subtype, MFVideoFormat_NV12) && !IsEqualGUID(subtype, MFVideoFormat_YUY2)))
        {
            return m_pythonShushActive;
        }

        ++m_pythonEyeFrameCounter;
        if ((m_pythonEyeFrameCounter % 3) != 0)
        {
            return m_pythonShushActive;
        }

        if (m_pythonEyeRetryCountdown > 0)
        {
            --m_pythonEyeRetryCountdown;
            return m_pythonShushActive;
        }

        if (!m_winsockInitialized)
        {
            WSADATA data = {};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                m_pythonEyeRetryCountdown = 30;
                return false;
            }
            m_winsockInitialized = true;
        }

        auto closeSocket = [&]()
        {
            if (m_pythonEyeSocket != INVALID_SOCKET)
            {
                closesocket(m_pythonEyeSocket);
                m_pythonEyeSocket = INVALID_SOCKET;
            }
            m_pythonEyeRetryCountdown = 30;
            m_pythonShushActive = false;
            m_pythonEyeCount = 0;
        };

        if (m_pythonEyeSocket == INVALID_SOCKET)
        {
            m_pythonEyeSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_pythonEyeSocket == INVALID_SOCKET)
            {
                closeSocket();
                return false;
            }

            u_long nonBlocking = 1;
            if (ioctlsocket(m_pythonEyeSocket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
            {
                closeSocket();
                return false;
            }

            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(kPythonEyePort));
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            const int connectResult = connect(
                m_pythonEyeSocket,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address));
            if (connectResult == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
                {
                    closeSocket();
                    if (!m_pythonWorkerLogWritten)
                    {
                        AppendRuntimeLog(L"Meet Gesture FX Python eye worker unavailable; crow effect remains native");
                        m_pythonWorkerLogWritten = true;
                    }
                    return false;
                }

                fd_set writeSet;
                FD_ZERO(&writeSet);
                FD_SET(m_pythonEyeSocket, &writeSet);
                timeval timeout = {};
                timeout.tv_usec = 20000;
                if (select(0, nullptr, &writeSet, nullptr, &timeout) <= 0)
                {
                    closeSocket();
                    return false;
                }
                int socketError = 0;
                int socketErrorSize = sizeof(socketError);
                getsockopt(
                    m_pythonEyeSocket,
                    SOL_SOCKET,
                    SO_ERROR,
                    reinterpret_cast<char*>(&socketError),
                    &socketErrorSize);
                if (socketError != 0)
                {
                    closeSocket();
                    return false;
                }
            }

            nonBlocking = 0;
            ioctlsocket(m_pythonEyeSocket, FIONBIO, &nonBlocking);
            // MediaPipe Tasks inference can legitimately take longer than a
            // single camera frame, especially on the first few frames while
            // XNNPACK warms up. A 40 ms receive timeout made the native side
            // abort the connection before the Python worker could reply.
            const int timeoutMs = 2000;
            setsockopt(
                m_pythonEyeSocket,
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                sizeof(timeoutMs));
            setsockopt(
                m_pythonEyeSocket,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                sizeof(timeoutMs));
        }

        const UINT32 sampleWidth = kPythonEyeWidth;
        const UINT32 sampleHeight = std::max<UINT32>(96, (m_height * sampleWidth) / std::max<UINT32>(1, m_width));
        const UINT stride = static_cast<UINT>(pitch);
        const UINT required = IsEqualGUID(subtype, MFVideoFormat_NV12)
            ? stride * m_height + stride * (m_height / 2)
            : stride * m_height;
        if (bufferLength < required) return m_pythonShushActive;

        std::vector<BYTE> payload(static_cast<size_t>(sampleWidth) * sampleHeight * 3);
        auto clampByte = [](int value) -> BYTE {
            return static_cast<BYTE>(std::clamp(value, 0, 255));
        };
        for (UINT y = 0; y < sampleHeight; ++y)
        {
            const UINT sourceY = std::min(m_height - 1, (y * m_height) / sampleHeight);
            for (UINT x = 0; x < sampleWidth; ++x)
            {
                const UINT sourceX = std::min(m_width - 1, (x * m_width) / sampleWidth);
                BYTE pixelY = 0;
                BYTE pixelU = 128;
                BYTE pixelV = 128;
                ReadEffectPixel(buffer, pitch, m_height, sourceX, sourceY, subtype, &pixelY, &pixelU, &pixelV);
                const int c = std::max(0, static_cast<int>(pixelY) - 16);
                const int d = static_cast<int>(pixelU) - 128;
                const int e = static_cast<int>(pixelV) - 128;
                BYTE* pixel = payload.data() + (static_cast<size_t>(y) * sampleWidth + x) * 3;
                pixel[0] = clampByte((298 * c + 516 * d + 128) >> 8);
                pixel[1] = clampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
                pixel[2] = clampByte((298 * c + 409 * e + 128) >> 8);
            }
        }

        const PythonEyeRequestHeader request = {
            kPythonEyeRequestMagic,
            ++m_pythonEyeFrameId,
            sampleWidth,
            sampleHeight,
            3 };

        auto sendAll = [&](const BYTE* data, size_t size) -> bool
        {
            while (size > 0)
            {
                const int chunk = send(
                    m_pythonEyeSocket,
                    reinterpret_cast<const char*>(data),
                    static_cast<int>(std::min<size_t>(size, INT_MAX)),
                    0);
                if (chunk <= 0) return false;
                data += chunk;
                size -= static_cast<size_t>(chunk);
            }
            return true;
        };

        auto receiveAll = [&](BYTE* data, size_t size) -> bool
        {
            while (size > 0)
            {
                const int chunk = recv(
                    m_pythonEyeSocket,
                    reinterpret_cast<char*>(data),
                    static_cast<int>(std::min<size_t>(size, INT_MAX)),
                    0);
                if (chunk <= 0) return false;
                data += chunk;
                size -= static_cast<size_t>(chunk);
            }
            return true;
        };

        if (!sendAll(reinterpret_cast<const BYTE*>(&request), sizeof(request))
            || !sendAll(payload.data(), payload.size()))
        {
            closeSocket();
            return false;
        }

        PythonEyeResponseHeader response = {};
        if (!receiveAll(reinterpret_cast<BYTE*>(&response), sizeof(response))
            || response.magic != kPythonEyeResponseMagic
            || response.frameId != request.frameId
            || response.eyeCount > 2
            || response.payloadBytes != response.eyeCount * kPythonEyeRecordBytes)
        {
            closeSocket();
            return false;
        }

        std::vector<BYTE> responsePayload(response.payloadBytes);
        if (!responsePayload.empty() && !receiveAll(responsePayload.data(), responsePayload.size()))
        {
            closeSocket();
            return false;
        }

        m_pythonEyeCount = response.eyeCount;
        m_pythonShushActive = (response.flags & 1u) != 0;
        for (UINT32 eyeIndex = 0; eyeIndex < m_pythonEyeCount; ++eyeIndex)
        {
            float values[25] = {};
            memcpy(values, responsePayload.data() + eyeIndex * kPythonEyeRecordBytes, kPythonEyeRecordBytes);
            PythonEyeGeometry& eye = m_pythonEyes[eyeIndex];
            eye.centerX = values[0];
            eye.centerY = values[1];
            eye.radiusX = values[2];
            eye.radiusY = values[3];
            eye.irisCenterX = values[4];
            eye.irisCenterY = values[5];
            eye.irisRadiusX = values[6];
            eye.irisRadiusY = values[7];
            for (size_t pointIndex = 0; pointIndex < eye.contour.size(); ++pointIndex)
            {
                eye.contour[pointIndex] = values[8 + pointIndex];
            }
            eye.confidence = values[24];
        }
        return m_pythonShushActive;
    }

    void AugmentedMediaStream::RenderPythonEyeFilter(
        _Inout_ BYTE* buffer,
        _In_ DWORD bufferLength,
        _In_ LONG pitch,
        _In_ GUID const& subtype)
    {
        if (!m_pythonShushActive || m_pythonEyeCount == 0 || buffer == nullptr || pitch <= 0) return;
        if (!IsEqualGUID(subtype, MFVideoFormat_NV12) && !IsEqualGUID(subtype, MFVideoFormat_YUY2)) return;

        const UINT stride = static_cast<UINT>(pitch);
        const UINT required = IsEqualGUID(subtype, MFVideoFormat_NV12)
            ? stride * m_height + stride * (m_height / 2)
            : stride * m_height;
        if (bufferLength < required) return;

        auto insideContour = [](int x, int y, const std::array<POINT, 8>& contour) -> bool
        {
            bool inside = false;
            for (size_t i = 0, j = contour.size() - 1; i < contour.size(); j = i++)
            {
                const bool crosses = ((contour[i].y > y) != (contour[j].y > y));
                if (crosses)
                {
                    const float intersection = static_cast<float>(contour[j].x - contour[i].x)
                        * static_cast<float>(y - contour[i].y)
                        / static_cast<float>(contour[j].y - contour[i].y)
                        + static_cast<float>(contour[i].x);
                    if (static_cast<float>(x) < intersection) inside = !inside;
                }
            }
            return inside;
        };

        for (UINT32 eyeIndex = 0; eyeIndex < m_pythonEyeCount; ++eyeIndex)
        {
            const PythonEyeGeometry& eye = m_pythonEyes[eyeIndex];
            std::array<POINT, 8> contour{};
            int minX = static_cast<int>(m_width);
            int maxX = 0;
            int minY = static_cast<int>(m_height);
            int maxY = 0;
            for (size_t pointIndex = 0; pointIndex < contour.size(); ++pointIndex)
            {
                contour[pointIndex].x = std::clamp(
                    static_cast<int>(eye.contour[pointIndex * 2] * static_cast<float>(m_width)),
                    0,
                    static_cast<int>(m_width) - 1);
                contour[pointIndex].y = std::clamp(
                    static_cast<int>(eye.contour[pointIndex * 2 + 1] * static_cast<float>(m_height)),
                    0,
                    static_cast<int>(m_height) - 1);
                minX = std::min(minX, static_cast<int>(contour[pointIndex].x));
                maxX = std::max(maxX, static_cast<int>(contour[pointIndex].x));
                minY = std::min(minY, static_cast<int>(contour[pointIndex].y));
                maxY = std::max(maxY, static_cast<int>(contour[pointIndex].y));
            }

            if (maxX <= minX || maxY <= minY) continue;
            const float irisX = eye.irisCenterX * static_cast<float>(m_width);
            const float irisY = eye.irisCenterY * static_cast<float>(m_height);
            const float irisRadiusX = std::max(2.0f, eye.irisRadiusX * static_cast<float>(m_width));
            const float irisRadiusY = std::max(2.0f, eye.irisRadiusY * static_cast<float>(m_height));

            minX = std::max(0, minX - 3);
            maxX = std::min(static_cast<int>(m_width) - 1, maxX + 3);
            minY = std::max(0, minY - 3);
            maxY = std::min(static_cast<int>(m_height) - 1, maxY + 3);
            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    if (!insideContour(x, y, contour)) continue;
                    const float dx = (static_cast<float>(x) - irisX) / irisRadiusX;
                    const float dy = (static_cast<float>(y) - irisY) / irisRadiusY;
                    const float irisDistance = std::sqrt(dx * dx + dy * dy);
                    const float irisMask = 1.0f - SmoothStep(0.78f, 1.0f, irisDistance);
                    if (irisMask <= 0.0f) continue;

                    BYTE pixelY = 0;
                    BYTE pixelU = 128;
                    BYTE pixelV = 128;
                    ReadEffectPixel(
                        buffer,
                        pitch,
                        m_height,
                        static_cast<UINT>(x),
                        static_cast<UINT>(y),
                        subtype,
                        &pixelY,
                        &pixelU,
                        &pixelV);
                    // Preserve the camera's luminance and fine iris texture;
                    // only steer chroma toward crimson. This keeps catchlights
                    // and natural iris contrast instead of painting a solid
                    // red disk over the eye.
                    const float normalizedLuma = std::clamp(
                        (static_cast<float>(pixelY) - 16.0f) / 219.0f,
                        0.0f,
                        1.0f);
                    const float highlight = SmoothStep(0.78f, 0.98f, normalizedLuma);
                    const float pupil = 1.0f - SmoothStep(0.12f, 0.38f, irisDistance);
                    const float innerRed = 1.0f - SmoothStep(0.22f, 0.78f, irisDistance);
                    const float outerRing = SmoothStep(0.62f, 0.98f, irisDistance);
                    const float amount = std::clamp(
                        irisMask * (0.90f - 0.48f * highlight),
                        0.0f,
                        1.0f);

                    const BYTE targetU = static_cast<BYTE>(std::clamp(
                        static_cast<int>(108.0f - 15.0f * innerRed + 8.0f * outerRing),
                        70,
                        150));
                    const BYTE targetV = static_cast<BYTE>(std::clamp(
                        static_cast<int>(184.0f + 44.0f * innerRed - 16.0f * outerRing),
                        120,
                        245));
                    const BYTE targetY = static_cast<BYTE>(std::clamp(
                        static_cast<int>(static_cast<float>(pixelY) * (1.0f - 0.58f * pupil) + 8.0f * pupil),
                        16,
                        235));
                    SetEffectPixel(
                        buffer,
                        pitch,
                        m_height,
                        static_cast<UINT>(x),
                        static_cast<UINT>(y),
                        subtype,
                        targetY,
                        targetU,
                        targetV,
                        amount);
                }
            }
        }
    }

    /// <summary>
    /// This is where some of the potential magic happens, where the pixel buffer is processed according to 
    /// the state this virtual camera stream is put into. This can be manipulated for example using 
    /// a custom DDI..
    /// </summary>
    /// <param name="inputSample"></param>
    /// <returns></returns>
    void AugmentedMediaStream::StartCrowDispersion()
    {
        EnsureCrowSpriteLoaded();
        if (!m_dispersionLogWritten)
        {
            AppendRuntimeLog(L"Meet Gesture FX dispersion started: live-frame render path");
            m_dispersionLogWritten = true;
        }
        m_meetGestureFxState = MeetGestureFxState::Dispersing;
        m_meetGestureFxFrame = 0;
        m_meetGestureFxCooldown = IsDispersionSelfTestEnabled() ? 0 : 150;
        m_crows.clear();
        m_crows.reserve(16);

        for (UINT i = 0; i < 16; ++i)
        {
            const float angle = NextRandom(m_randomState) * 2.0f * kPi;
            const float speed = 5.0f + NextRandom(m_randomState) * 4.0f;
            CrowParticle crow;
            crow.x = static_cast<float>(m_width) * (0.30f + NextRandom(m_randomState) * 0.40f);
            crow.y = static_cast<float>(m_height) * (0.18f + NextRandom(m_randomState) * 0.52f);
            crow.vx = std::cos(angle) * speed;
            crow.vy = std::sin(angle) * speed - 0.7f;
            crow.lifetime = 35.0f + NextRandom(m_randomState) * 55.0f;
            crow.scale = 84.0f;
            crow.phase = NextRandom(m_randomState) * 2.0f * kPi;
            m_crows.push_back(crow);
        }
    }

    void AugmentedMediaStream::RenderShushEyeEffect(
        _Inout_ BYTE* buffer,
        _In_ DWORD bufferLength,
        _In_ LONG pitch,
        _In_ GUID const& subtype,
        _In_ bool fingerOnLip)
    {
        const bool isNv12 = IsEqualGUID(subtype, MFVideoFormat_NV12);
        const bool isYuy2 = IsEqualGUID(subtype, MFVideoFormat_YUY2);
        if ((!isNv12 && !isYuy2) || buffer == nullptr || pitch <= 0)
        {
            return;
        }

        const UINT stride = static_cast<UINT>(pitch);
        const UINT required = isNv12
            ? stride * m_height + stride * (m_height / 2)
            : stride * m_height;
        if (bufferLength < required) return;

        if (m_shushCooldown > 0)
        {
            --m_shushCooldown;
        }

        if (fingerOnLip && !m_hasFaceLandmarks)
        {
            UpdateFaceBoxFromFrame(buffer, bufferLength, pitch, subtype);
        }

        if (m_shushFramesRemaining == 0
            && m_meetGestureFxState == MeetGestureFxState::Idle
            && m_shushCooldown == 0)
        {
            if (fingerOnLip)
            {
                m_shushFramesRemaining = 90; // approximately three seconds at 30 fps
                m_shushCooldown = 150;
                AppendRuntimeLog(L"Meet Gesture FX shush effect started: red eye glow");
            }
        }

        if (m_shushFramesRemaining == 0) return;

        ShushEyePoint leftEye = m_faceLeftEye;
        ShushEyePoint rightEye = m_faceRightEye;
        bool eyesFound = m_hasFaceLandmarks;
        if (!eyesFound && m_hasFaceBox)
        {
            eyesFound = FindShushEyesInFaceBox(
                buffer,
                bufferLength,
                pitch,
                m_width,
                m_height,
                subtype,
                m_faceBox,
                &leftEye,
                &rightEye);
        }
        if (!eyesFound)
        {
            eyesFound = FindShushEyes(
                buffer,
                bufferLength,
                pitch,
                m_width,
                m_height,
                subtype,
                &leftEye,
                &rightEye);
        }
        if (!eyesFound)
        {
            // Never draw fixed circles at the image center. If the actual eyes
            // cannot be located in this frame, leave the frame untouched.
            --m_shushFramesRemaining;
            return;
        }

        const float pulse = 0.78f + 0.22f * std::sin(static_cast<float>(m_shushFramesRemaining) * 0.34f);
        auto drawEyeFilter = [&](const ShushEyePoint& eye)
        {
            const int glowRadiusX = m_hasFaceLandmarks ? eye.radiusX : eye.radiusX + 9;
            const int glowRadiusY = m_hasFaceLandmarks ? eye.radiusY : eye.radiusY + 6;
            for (int y = eye.y - glowRadiusY; y <= eye.y + glowRadiusY; ++y)
            {
                for (int x = eye.x - glowRadiusX; x <= eye.x + glowRadiusX; ++x)
                {
                    if (x < 0 || y < 0 || x >= static_cast<int>(m_width) || y >= static_cast<int>(m_height)) continue;
                    const float dx = static_cast<float>(x - eye.x) / static_cast<float>(glowRadiusX);
                    const float dy = static_cast<float>(y - eye.y) / static_cast<float>(glowRadiusY);
                    const float ellipse = dx * dx + dy * dy;
                    if (ellipse > 1.0f) continue;

                    BYTE pixelY = 0;
                    BYTE pixelU = 128;
                    BYTE pixelV = 128;
                    ReadEffectPixel(
                        buffer,
                        pitch,
                        m_height,
                        static_cast<UINT>(x),
                        static_cast<UINT>(y),
                        subtype,
                        &pixelY,
                        &pixelU,
                        &pixelV);

                    const float edge = 1.0f - std::sqrt(ellipse);
                    const int chromaDistance =
                        std::abs(static_cast<int>(pixelU) - 128) +
                        std::abs(static_cast<int>(pixelV) - 128);
                    const bool isEyePixel = m_hasFaceLandmarks
                        || (pixelY < 120 && chromaDistance < 115);
                    const float opacity = m_hasFaceLandmarks
                        ? pulse * std::clamp(0.62f + 0.38f * edge, 0.0f, 1.0f)
                        : (isEyePixel
                            ? 0.94f * pulse * std::clamp(0.55f + edge, 0.0f, 1.0f)
                            : 0.12f * pulse * edge);
                    SetEffectPixel(
                        buffer,
                        pitch,
                        m_height,
                        static_cast<UINT>(x),
                        static_cast<UINT>(y),
                        subtype,
                        82,
                        86,
                        255,
                        isEyePixel ? opacity : 0.0f);
                }
            }
        };

        drawEyeFilter(leftEye);
        drawEyeFilter(rightEye);

        --m_shushFramesRemaining;
    }

    void AugmentedMediaStream::RenderCrowDispersion(
        _Inout_ BYTE* buffer,
        _In_ DWORD bufferLength,
        _In_ LONG pitch,
        _In_ GUID const& subtype,
        _In_ bool suppressNewTrigger)
    {
        const bool isNv12 = IsEqualGUID(subtype, MFVideoFormat_NV12);
        const bool isYuy2 = IsEqualGUID(subtype, MFVideoFormat_YUY2);
        if ((!isNv12 && !isYuy2) || buffer == nullptr || pitch <= 0)
        {
            return;
        }

        const bool selfTest = IsDispersionSelfTestEnabled();
        if (m_meetGestureFxState == MeetGestureFxState::Idle)
        {
            if (selfTest)
            {
                m_meetGestureFxCooldown = 0;
            }
            else if (m_meetGestureFxCooldown > 0)
            {
                --m_meetGestureFxCooldown;
            }
            const bool palmDetected = isNv12
                ? DetectOpenPalmNv12(buffer, bufferLength, pitch, m_width, m_height)
                : DetectOpenPalmYuy2(buffer, bufferLength, pitch, m_width, m_height);
            if (m_meetGestureFxCooldown == 0 && !suppressNewTrigger && (palmDetected || selfTest))
            {
                if (selfTest)
                {
                    AppendRuntimeLog(L"Meet Gesture FX ProcessSample reached forced dispersion");
                }
                StartCrowDispersion();
            }
        }

        if (m_meetGestureFxState == MeetGestureFxState::Idle)
        {
            return;
        }

        constexpr UINT32 kDisperseFrames = 18;
        constexpr UINT32 kInvisibleFrames = 90;
        constexpr UINT32 kReformFrames = 24;
        const UINT32 frame = m_meetGestureFxFrame++;

        if (frame >= kDisperseFrames + kInvisibleFrames)
        {
            m_meetGestureFxState = MeetGestureFxState::Reforming;
        }
        if (frame >= kDisperseFrames + kInvisibleFrames + kReformFrames)
        {
            m_meetGestureFxState = MeetGestureFxState::Idle;
            m_meetGestureFxFrame = 0;
            m_crows.clear();
            return;
        }

        float bodyFade = 1.0f;
        if (m_meetGestureFxState == MeetGestureFxState::Dispersing)
        {
            bodyFade = 0.0f;
        }
        else if (m_meetGestureFxState == MeetGestureFxState::Reforming)
        {
            const UINT32 reformFrame = frame - kDisperseFrames - kInvisibleFrames;
            bodyFade = static_cast<float>(reformFrame) / static_cast<float>(kReformFrames);
        }

        const UINT stride = static_cast<UINT>(pitch);
        const UINT required = isNv12
            ? stride * m_height + stride * (m_height / 2)
            : stride * m_height;
        if (bufferLength < required) return;

        // Restore the original first-effect behavior: during the invisible
        // interval, paint the fixed silhouette from nearby live background
        // pixels, then fade that fill out while the live body reforms. This is
        // intentionally independent of the Python eye worker.
        if (bodyFade < 1.0f)
        {
            FillBodyWithNearbyBackground(
                buffer,
                pitch,
                m_height,
                m_width,
                subtype,
                1.0f - bodyFade);
        }

        const bool spriteLoaded = !m_crowSpritePixels.empty();
        for (auto& crow : m_crows)
        {
            crow.x += crow.vx;
            crow.y += crow.vy;
            crow.vy += 0.035f;
            crow.phase += 0.45f;
            crow.age += 1.0f;
            const float fade = std::clamp(1.0f - crow.age / crow.lifetime, 0.0f, 1.0f);
            if (spriteLoaded)
            {
                DrawCrowSprite(buffer, pitch, subtype, crow, fade);
            }
            else
            {
                DrawCrow(buffer, pitch, m_height, m_width, subtype, crow, fade);
            }
        }

        m_crows.erase(
            std::remove_if(m_crows.begin(), m_crows.end(), [](CrowParticle const& crow) {
                return crow.age >= crow.lifetime;
            }),
            m_crows.end());
    }

    HRESULT AugmentedMediaStream::ProcessSample(_In_ IMFSample* inputSample)
    {
        winrt::slim_lock_guard lock(m_Lock);

        if (!m_runtimeLogWritten)
        {
            AppendRuntimeLog(L"Meet Gesture FX ProcessSample reached frame processing");
            m_runtimeLogWritten = true;
        }

        wil::com_ptr_nothrow<IMFMediaBuffer> spBuffer;
        RETURN_IF_FAILED(inputSample->GetBufferByIndex(0, &spBuffer));

        wil::com_ptr_nothrow<IMFMediaTypeHandler> mediaTypeHandler;
        wil::com_ptr_nothrow<IMFMediaType> currentMediaType;
        GUID subtype = GUID_NULL;
        RETURN_IF_FAILED(m_spStreamDesc->GetMediaTypeHandler(&mediaTypeHandler));
        RETURN_IF_FAILED(mediaTypeHandler->GetCurrentMediaType(&currentMediaType));
        RETURN_IF_FAILED(currentMediaType->GetGUID(MF_MT_SUBTYPE, &subtype));

        LONG pitch = 0;
        BYTE* bufferStart = nullptr;
        DWORD bufferLength = 0;
        BYTE* pbuf = nullptr;
        wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;

        RETURN_IF_FAILED(spBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D)));
        RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_ReadWrite,
            &pbuf,
            &pitch,
            &bufferStart,
            &bufferLength));

        HRESULT processingResult = S_OK;
        // This camera is dedicated to Meet Gesture FX. Some camera clients
        // send the sample's legacy custom-control flag as OFF during startup;
        // honoring that flag would silently disable the effect in Meet.
        if (IsDispersionSelfTestEnabled())
        {
            processingResult = DrawMeetGestureFxMarker(pbuf, bufferLength, pitch, m_width, m_height, subtype);
        }
        if (SUCCEEDED(processingResult))
        {
            const bool pythonShushActive = QueryPythonEyeWorker(pbuf, bufferLength, pitch, subtype);
            RenderCrowDispersion(pbuf, bufferLength, pitch, subtype, pythonShushActive);
            RenderPythonEyeFilter(pbuf, bufferLength, pitch, subtype);
        }

        HRESULT unlockResult = buffer2D->Unlock2D();
        RETURN_IF_FAILED(processingResult);
        RETURN_IF_FAILED(unlockResult);

        if (SUCCEEDED(_CheckShutdownRequiresLock()))
        {
            // queue event
            RETURN_IF_FAILED(m_spEventQueue->QueueEventParamUnk(
                MEMediaSample,
                GUID_NULL,
                S_OK,
                inputSample));
        }
        return S_OK;
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // IMFMediaEventGenerator

    IFACEMETHODIMP AugmentedMediaStream::BeginGetEvent(
            _In_ IMFAsyncCallback* pCallback,
            _In_ IUnknown* punkState
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->BeginGetEvent(pCallback, punkState));

        return S_OK;
    }

    IFACEMETHODIMP AugmentedMediaStream::EndGetEvent(
            _In_ IMFAsyncResult* pResult,
            _COM_Outptr_ IMFMediaEvent** ppEvent
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->EndGetEvent(pResult, ppEvent));

        return S_OK;
    }

    IFACEMETHODIMP AugmentedMediaStream::GetEvent(
            DWORD dwFlags,
            _COM_Outptr_ IMFMediaEvent** ppEvent
        )
    {
        // NOTE:
        // GetEvent can block indefinitely, so we don't hold the lock.
        // This requires some juggling with the event queue pointer.
        wil::com_ptr_nothrow<IMFMediaEventQueue> spQueue;

        {
            winrt::slim_lock_guard lock(m_Lock);

            RETURN_IF_FAILED(_CheckShutdownRequiresLock());
            spQueue = m_spEventQueue;
        }

        // Now get the event.
        RETURN_IF_FAILED(spQueue->GetEvent(dwFlags, ppEvent));

        return S_OK;
    }

    IFACEMETHODIMP AugmentedMediaStream::QueueEvent(
            MediaEventType eventType,
            REFGUID guidExtendedType,
            HRESULT hrStatus,
            _In_opt_ PROPVARIANT const* pvValue
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(eventType, guidExtendedType, hrStatus, pvValue));

        return S_OK;
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // IMFMediaStream

    IFACEMETHODIMP AugmentedMediaStream::GetMediaSource(
            _COM_Outptr_ IMFMediaSource** ppMediaSource
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
        *ppMediaSource = nullptr;

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        RETURN_IF_FAILED(m_parent.copy_to(ppMediaSource));

        return S_OK;
    }

    IFACEMETHODIMP AugmentedMediaStream::GetStreamDescriptor(
            _COM_Outptr_ IMFStreamDescriptor** ppStreamDescriptor
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
        *ppStreamDescriptor = nullptr;

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        if (m_spStreamDesc != nullptr)
        {
            RETURN_IF_FAILED(m_spStreamDesc.copy_to(ppStreamDescriptor));
        }
        else
        {
            return E_UNEXPECTED;
        }

        return S_OK;
    }

    IFACEMETHODIMP AugmentedMediaStream::RequestSample(
            _In_ IUnknown* pToken
        )
    {
        winrt::slim_lock_guard lock(m_Lock);
        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        RETURN_IF_FAILED(m_spDevStream->RequestSample(pToken));

        return S_OK;
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // IMFMediaStream2
    IFACEMETHODIMP AugmentedMediaStream::SetStreamState(MF_STREAM_STATE state)
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        DEBUG_MSG(L"[%d] SetStreamState %d", m_dwStreamId, state);

        wil::com_ptr_nothrow<IMFMediaStream2> spStream2;
        RETURN_IF_FAILED(m_spDevStream->QueryInterface(IID_PPV_ARGS(&spStream2)));
        RETURN_IF_FAILED(spStream2->SetStreamState(state));

        return S_OK;
    }

    IFACEMETHODIMP AugmentedMediaStream::GetStreamState(_Out_ MF_STREAM_STATE* pState)
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_POINTER, pState);
        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        wil::com_ptr_nothrow<IMFMediaStream2> spStream2;
        RETURN_IF_FAILED(m_spDevStream->QueryInterface(IID_PPV_ARGS(&spStream2)));
        RETURN_IF_FAILED(spStream2->GetStreamState(pState));

        return S_OK;
    }

    void AugmentedMediaStream::OnMediaStreamEvent(_In_ IMFAsyncResult* pResult)
    {
        HRESULT hr = S_OK;

        // Forward deviceStream event
        wil::com_ptr_nothrow<IUnknown> spUnknown;
        wil::com_ptr_nothrow<IMFMediaStream> spMediaStream;
        wil::com_ptr_nothrow<IMFMediaEvent> spEvent;
        MediaEventType met = MEUnknown;
        bool bForwardEvent = true;

        CHECKHR_GOTO(pResult->GetState(&spUnknown), done);

        CHECKHR_GOTO(spUnknown->QueryInterface(IID_PPV_ARGS(&spMediaStream)), done);

        CHECKHR_GOTO(spMediaStream->EndGetEvent(pResult, &spEvent), done);
        CHECKNULL_GOTO(spEvent, MF_E_UNEXPECTED, done);

        CHECKHR_GOTO(spEvent->GetType(&met), done);
        DEBUG_MSG(L"[%d] OnMediaStreamEvent, streamId: %d, met:%d ", m_dwStreamId, met);
        
        // This shows how to intercept sample from physical camera
        // and do custom processing on the sample.
        // 
        if (met == MEMediaSample)
        {
            wil::com_ptr_nothrow<IMFSample> spSample;
            wil::com_ptr_nothrow<IUnknown> spToken;
            wil::com_ptr_nothrow<IUnknown> spSampleUnk;

            wil::unique_prop_variant propVar = {};
            CHECKHR_GOTO(spEvent->GetValue(&propVar), done);
            if (VT_UNKNOWN != propVar.vt)
            {
                CHECKHR_GOTO(MF_E_UNEXPECTED, done);
            }
            spSampleUnk = propVar.punkVal;
            CHECKHR_GOTO(spSampleUnk->QueryInterface(IID_PPV_ARGS(&spSample)), done);

            CHECKHR_GOTO(ProcessSample(spSample.get()), done);
            bForwardEvent = false;
        }
        else if (met == MEStreamStarted)
        {
            CHECKHR_GOTO(SetStreamState(MF_STREAM_STATE_RUNNING), done);
            CHECKHR_GOTO(Start(), done);
        }
        else if (met == MEStreamStopped)
        {
            CHECKHR_GOTO(SetStreamState(MF_STREAM_STATE_STOPPED), done);
            CHECKHR_GOTO(Stop(), done);
        }
        else if (met == MEError)
        {
            goto done;
        }

    done:
        wil::com_ptr_nothrow<AugmentedMediaSource> spParent;
        {
            winrt::slim_lock_guard lock(m_Lock);
            if (SUCCEEDED(_CheckShutdownRequiresLock()))
            {
                if (FAILED(hr) || met == MEError)
                {
                    spParent = m_parent;
                    DEBUG_MSG(L"[stream:%d] error when processing stream event: met=%d, hr=0x%08x", m_dwStreamId, met, hr);
                }
                else
                {
                    // Forward event
                    if (bForwardEvent)
                    {
                        (void)(m_spEventQueue->QueueEvent(spEvent.get()));
                    }

                    // Continue listening to source event
                    (void)(spMediaStream->BeginGetEvent(m_xOnMediaStreamEvent.get(), m_spDevStream.get()));
                }
            }
            if (FAILED(hr) && spParent != nullptr)
            {
                (void)spParent->QueueEvent(MEError, GUID_NULL, hr, nullptr);
            }
        }
        
        DEBUG_MSG(L"[%d] OnMediaStreamEvent exit", m_dwStreamId);
    }

    HRESULT AugmentedMediaStream::Shutdown()
    {
        HRESULT hr = S_OK;
        winrt::slim_lock_guard lock(m_Lock);

        m_isShutdown = true;
        ClosePythonEyeWorker();
        m_parent.reset();
        m_spDevStream.reset();

        if (m_spEventQueue != nullptr)
        {
            hr = m_spEventQueue->Shutdown();
            m_spEventQueue.reset();
        }
        m_spStreamDesc.reset();
        
        m_spAttributes.reset();

        return hr;
    }

    HRESULT AugmentedMediaStream::SetMediaStream(_In_ IMFMediaStream* pMediaStream)
    {
        winrt::slim_lock_guard lock(m_Lock);
        DEBUG_MSG(L"[%d] Set MediaStream %p ", m_dwStreamId, pMediaStream);

        RETURN_HR_IF_NULL(E_INVALIDARG, pMediaStream);
        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        m_spDevStream = pMediaStream;
        RETURN_IF_FAILED(m_spDevStream->BeginGetEvent(m_xOnMediaStreamEvent.get(), m_spDevStream.get()));

        return S_OK;
    }

    HRESULT AugmentedMediaStream::Start()
    {
        winrt::slim_lock_guard lock(m_Lock);

        wil::com_ptr_nothrow<IMFMediaTypeHandler> spMTHandler;
        RETURN_IF_FAILED(m_spStreamDesc->GetMediaTypeHandler(&spMTHandler));

        wil::com_ptr_nothrow<IMFMediaType> spMediaType;

        GUID subType;
        RETURN_IF_FAILED(spMTHandler->GetCurrentMediaType(&spMediaType));
        RETURN_IF_FAILED(spMediaType->GetGUID(MF_MT_SUBTYPE, &subType));
        MFGetAttributeSize(spMediaType.get(), MF_MT_FRAME_SIZE, &m_width, &m_height);

        DEBUG_MSG(L"AugmentedMediaStream Start().. with mediatype: %s, %dx%d ", winrt::to_hstring(subType).data(), m_width, m_height);

        // Post MEStreamStarted event to signal stream has started 
        RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));

        return S_OK;
    }

    HRESULT AugmentedMediaStream::Stop()
    {
        winrt::slim_lock_guard lock(m_Lock);

        // Post MEStreamStopped event to signal stream has stopped
        RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
        return S_OK;
    }

    _Requires_lock_held_(m_Lock)
    HRESULT AugmentedMediaStream::_CheckShutdownRequiresLock()
    {
        if (m_isShutdown)
        {
            return MF_E_SHUTDOWN;
        }

        if (m_spEventQueue == nullptr)
        {
            return E_UNEXPECTED;

        }
        return S_OK;
    }

    _Requires_lock_held_(m_Lock)
    HRESULT AugmentedMediaStream::_SetStreamAttributes(
        _In_ IMFAttributes* pAttributeStore
    )
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, pAttributeStore);

        // adjust accordingly if other types of stream are wrapped
        RETURN_IF_FAILED(pAttributeStore->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_dwStreamId));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

        return S_OK;
    }

    
}

