//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#ifndef  AUGMENTEDMEDIASTREAM_H
#define  AUGMENTEDMEDIASTREAM_H

#pragma once
#include "AugmentedMediaSource.h"
#include <winrt/Windows.Media.FaceAnalysis.h>
#include <array>

namespace winrt::WindowsSample::implementation
{
    struct CrowParticle
    {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float age = 0.0f;
        float lifetime = 1.0f;
        float scale = 1.0f;
        float phase = 0.0f;
    };

    struct ShushEyePoint
    {
        int x = 0;
        int y = 0;
        int radiusX = 12;
        int radiusY = 7;
    };

    struct PythonEyeGeometry
    {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float radiusX = 0.0f;
        float radiusY = 0.0f;
        float irisCenterX = 0.0f;
        float irisCenterY = 0.0f;
        float irisRadiusX = 0.0f;
        float irisRadiusY = 0.0f;
        std::array<float, 16> contour{};
        float confidence = 0.0f;
    };

    struct AugmentedMediaStream : winrt::implements<AugmentedMediaStream, IMFMediaStream2>
    {
        friend struct AugmentedMediaSource;

    public:
        ~AugmentedMediaStream();

        // IMFMediaEventGenerator
        IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
        IFACEMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
        IFACEMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
        IFACEMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

        // IMFMediaStream
        IFACEMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
        IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
        IFACEMETHODIMP RequestSample(IUnknown* pToken) override;

        // IMFMediaStream2
        IFACEMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
        IFACEMETHODIMP GetStreamState(_Out_ MF_STREAM_STATE* pState) override;

        // Non-interface methods.
        HRESULT Initialize(_In_ AugmentedMediaSource* pSource, _In_ DWORD dwStreamId, _In_ IMFStreamDescriptor* pStreamDesc, _In_ DWORD dwWorkQueue);
        HRESULT ProcessSample(_In_ IMFSample* inputSample);
        HRESULT Shutdown();
        DWORD StreamIdentifier() { return m_dwStreamId; };
        HRESULT SetMediaStream(_In_ IMFMediaStream* pMediaStream);

        void OnMediaStreamEvent(_In_ IMFAsyncResult* pResult);

        HRESULT Start();
        HRESULT Stop();

        void StartCrowDispersion();
        void RenderCrowDispersion(_Inout_ BYTE* buffer, _In_ DWORD bufferLength, _In_ LONG pitch, _In_ GUID const& subtype, _In_ bool suppressNewTrigger);
        void RenderShushEyeEffect(_Inout_ BYTE* buffer, _In_ DWORD bufferLength, _In_ LONG pitch, _In_ GUID const& subtype, _In_ bool fingerOnLip);
        bool QueryPythonEyeWorker(_In_reads_bytes_(bufferLength) const BYTE* buffer, _In_ DWORD bufferLength, _In_ LONG pitch, _In_ GUID const& subtype);
        void RenderPythonEyeFilter(_Inout_ BYTE* buffer, _In_ DWORD bufferLength, _In_ LONG pitch, _In_ GUID const& subtype);
        void ClosePythonEyeWorker();
        bool ReadFaceLandmarksFromSample(_In_ IMFSample* inputSample);
        bool UpdateFaceBoxFromFrame(_In_reads_bytes_(bufferLength) const BYTE* buffer, _In_ DWORD bufferLength, _In_ LONG pitch, _In_ GUID const& subtype);
        bool EnsureCrowSpriteLoaded();
        void DrawCrowSprite(_Inout_ BYTE* buffer, _In_ LONG pitch, _In_ GUID const& subtype, _In_ const CrowParticle& crow, _In_ float alpha);

    protected:
        _Requires_lock_held_(m_Lock) HRESULT _CheckShutdownRequiresLock();
        _Requires_lock_held_(m_Lock) HRESULT _SetStreamAttributes(_In_ IMFAttributes* pAttributeStore);

        wil::com_ptr_nothrow<CAsyncCallback<AugmentedMediaStream>> m_xOnMediaStreamEvent;
        wil::unique_cotaskmem_array_ptr<wil::com_ptr_nothrow<IMFMediaType>> m_mediaTypeList;
        std::vector<wil::com_ptr_nothrow<IMFMediaType>> m_mediaTypeCache;

        winrt::slim_mutex  m_Lock;

        wil::com_ptr_nothrow<AugmentedMediaSource> m_parent;
        wil::com_ptr_nothrow<IMFMediaStream> m_spDevStream;
        wil::com_ptr_nothrow<IMFStreamDescriptor> m_spStreamDesc;
        wil::com_ptr_nothrow<IMFMediaEventQueue> m_spEventQueue;
        wil::com_ptr_nothrow<IMFAttributes> m_spAttributes;

        bool m_isShutdown = false;
        DWORD m_dwDevSourceStreamIdentifier = 0;
        DWORD m_dwSerialWorkQueueId = 0;
        DWORD m_dwStreamId = 0;
        UINT32 m_width;
        UINT32 m_height;
        bool m_isCustomFXEnabled = false;

        enum class MeetGestureFxState
        {
            Idle,
            Dispersing,
            Invisible,
            Reforming,
        };

        MeetGestureFxState m_meetGestureFxState = MeetGestureFxState::Idle;
        UINT32 m_meetGestureFxFrame = 0;
        UINT32 m_meetGestureFxCooldown = 0;
        // Kept for source compatibility with the retired native eye prototype;
        // the active shush path is the Python eye worker below.
        UINT32 m_shushFramesRemaining = 0;
        UINT32 m_shushCooldown = 0;
        UINT32 m_pythonEyeFrameCounter = 0;
        UINT32 m_pythonEyeRetryCountdown = 0;
        UINT32 m_pythonEyeFrameId = 0;
        bool m_pythonShushActive = false;
        bool m_pythonWorkerLogWritten = false;
        SOCKET m_pythonEyeSocket = INVALID_SOCKET;
        bool m_winsockInitialized = false;
        std::array<PythonEyeGeometry, 2> m_pythonEyes{};
        UINT32 m_pythonEyeCount = 0;
        bool m_hasFaceLandmarks = false;
        ShushEyePoint m_faceLeftEye;
        ShushEyePoint m_faceRightEye;
        bool m_faceDetectorAttempted = false;
        UINT32 m_faceDetectorSkipFrames = 0;
        bool m_hasFaceBox = false;
        bool m_faceBoxLogWritten = false;
        RECT m_faceBox{};
        winrt::Windows::Media::FaceAnalysis::FaceDetector m_faceDetector{ nullptr };
        UINT32 m_randomState = 0x6D2B79F5;
        bool m_runtimeLogWritten = false;
        bool m_dispersionLogWritten = false;
        bool m_crowSpriteLoadAttempted = false;
        UINT32 m_crowSpriteWidth = 0;
        UINT32 m_crowSpriteHeight = 0;
        std::vector<BYTE> m_crowSpritePixels;
        std::vector<CrowParticle> m_crows;
    };
}

#endif
