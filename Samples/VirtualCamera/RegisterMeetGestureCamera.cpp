#include <windows.h>
#include <algorithm>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <cfgmgr32.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "cfgmgr32.lib")

// {7B89B92E-FE71-42D0-8A41-E137D06EA184}
static constexpr wchar_t kMediaSourceClsid[] = L"{7B89B92E-FE71-42D0-8A41-E137D06EA184}";

// VirtualCameraMediaSource activation attributes.
static const GUID kVcamDeviceInfo =
{ 0x3c31a5f8, 0x2795, 0x4fb9, { 0xa0, 0xa1, 0xc7, 0x33, 0xa6, 0x5c, 0x0c, 0xe8 } };
static const GUID kVcamKind =
{ 0xc7f7c57b, 0xdf30, 0x41d0, { 0xaf, 0xfc, 0x15, 0x20, 0x1c, 0xdf, 0x92, 0x0d } };

// DEVPKEY_DeviceInterface_VCamCreate_*.
static const DEVPROPKEY kSourceId =
{ 0x6ac1fbf7, 0x45f7, 0x4e06, { 0xbd, 0xa7, 0xf8, 0x17, 0xeb, 0xfa, 0x04, 0xd1 }, 4 };
static const DEVPROPKEY kFriendlyName =
{ 0x6ac1fbf7, 0x45f7, 0x4e06, { 0xbd, 0xa7, 0xf8, 0x17, 0xeb, 0xfa, 0x04, 0xd1 }, 5 };
static const DEVPROPKEY kLifetime =
{ 0x6ac1fbf7, 0x45f7, 0x4e06, { 0xbd, 0xa7, 0xf8, 0x17, 0xeb, 0xfa, 0x04, 0xd1 }, 6 };
static const DEVPROPKEY kAccess =
{ 0x6ac1fbf7, 0x45f7, 0x4e06, { 0xbd, 0xa7, 0xf8, 0x17, 0xeb, 0xfa, 0x04, 0xd1 }, 7 };

static const GUID kWindowsStudioCameraControlPropertySet =
{ 0x1666d655, 0x21a6, 0x4982, { 0x97, 0x28, 0x52, 0xc3, 0x9e, 0x86, 0x9f, 0x90 } };
static constexpr ULONG kWindowsStudioSupportedProperty = 0;
static constexpr ULONG kWindowsStudioFaceMetadataProperty = 5;
static constexpr ULONGLONG kWindowsStudioFaceLandmarksFlag = 0x2;

static std::string ToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

static void AppendLog(const std::wstring& message)
{
    WCHAR tempPath[MAX_PATH] = {};
    DWORD length = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
    if (length == 0 || length >= ARRAYSIZE(tempPath)) return;
    std::wstring path(tempPath, length);
    path += L"meet-gesture-vcam.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::string line = ToUtf8(message + L"\r\n");
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

static void PrintHr(const wchar_t* step, HRESULT hr)
{
    std::wcerr << step << L" failed: 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << L"\n";
    wchar_t message[128] = {};
    swprintf_s(message, L"%s hr=0x%08X", step, static_cast<unsigned long>(hr));
    AppendLog(message);
}

static void WriteYuy2Ppm(const BYTE* bytes, DWORD length, UINT width, UINT height)
{
    if (bytes == nullptr || width == 0 || height == 0 || length < width * height * 2) return;
    std::ofstream output("C:\\Users\\Nitish\\Documents\\GitHub\\meet-gesture-native\\camera-probe.ppm", std::ios::binary);
    if (!output) return;
    output << "P6\n" << width << " " << height << "\n255\n";
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* row = bytes + y * width * 2;
        for (UINT x = 0; x < width; x += 2)
        {
            const int y0 = row[x * 2];
            const int u = row[x * 2 + 1] - 128;
            const int y1 = row[x * 2 + 2];
            const int v = row[x * 2 + 3] - 128;
            auto writeRgb = [&](int luma) {
                const int r = std::clamp(luma + static_cast<int>(1.402f * v), 0, 255);
                const int g = std::clamp(luma - static_cast<int>(0.344f * u + 0.714f * v), 0, 255);
                const int b = std::clamp(luma + static_cast<int>(1.772f * u), 0, 255);
                const char pixel[3] = { static_cast<char>(r), static_cast<char>(g), static_cast<char>(b) };
                output.write(pixel, 3);
            };
            writeRgb(y0);
            if (x + 1 < width) writeRgb(y1);
        }
    }
}

static HRESULT RegisterMediaSourceClass()
{
    WCHAR modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
    if (length == 0 || length >= ARRAYSIZE(modulePath)) return HRESULT_FROM_WIN32(GetLastError());

    std::wstring dllPath(modulePath, length);
    const size_t slash = dllPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return E_FAIL;
    dllPath.resize(slash + 1);
    dllPath += L"VirtualCameraMediaSource.dll";

    HKEY key = nullptr;
    const wchar_t keyPath[] = L"SOFTWARE\\Classes\\CLSID\\{7B89B92E-FE71-42D0-8A41-E137D06EA184}\\InprocServer32";
    LSTATUS status = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        keyPath,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

    status = RegSetValueExW(key, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(dllPath.c_str()),
        static_cast<DWORD>((dllPath.size() + 1) * sizeof(wchar_t)));
    if (status == ERROR_SUCCESS)
    {
        const wchar_t threadingModel[] = L"Both";
        status = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(threadingModel), sizeof(threadingModel));
    }
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(status);
}

static void PrintVirtualCameraProperties(IMFActivate** devices, UINT32 deviceCount)
{
    for (UINT32 i = 0; i < deviceCount; ++i)
    {
        WCHAR* link = nullptr;
        UINT32 linkLength = 0;
        if (FAILED(devices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            &link,
            &linkLength))) continue;
        if (wcsstr(link, L"vcamdevapi") == nullptr)
        {
            CoTaskMemFree(link);
            continue;
        }

        std::wcout << L"Virtual link: " << link << L"\n";
        const DEVPROPKEY* keys[] = { &kSourceId, &kFriendlyName, &kLifetime, &kAccess };
        const wchar_t* labels[] = { L"source", L"name", L"lifetime", L"access" };
        for (int keyIndex = 0; keyIndex < 4; ++keyIndex)
        {
            DEVPROPTYPE type = 0;
            ULONG size = 0;
            CONFIGRET cr = CM_Get_Device_Interface_PropertyW(link, keys[keyIndex], &type, nullptr, &size, 0);
            if (cr != CR_BUFFER_SMALL)
            {
                std::wcout << L"  " << labels[keyIndex] << L": unavailable\n";
                continue;
            }
            std::vector<BYTE> data(size);
            cr = CM_Get_Device_Interface_PropertyW(link, keys[keyIndex], &type, data.data(), &size, 0);
            if (cr != CR_SUCCESS)
            {
                std::wcout << L"  " << labels[keyIndex] << L": unavailable\n";
                continue;
            }
            if (type == DEVPROP_TYPE_STRING)
            {
                std::wcout << L"  " << labels[keyIndex] << L": " << reinterpret_cast<wchar_t*>(data.data()) << L"\n";
            }
            else if (type == DEVPROP_TYPE_INT32)
            {
                std::wcout << L"  " << labels[keyIndex] << L": " << *reinterpret_cast<LONG*>(data.data()) << L"\n";
            }
        }
        CoTaskMemFree(link);
    }
}

static HRESULT RemoveMeetGestureCamera()
{
    IMFVirtualCamera* camera = nullptr;
    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_CurrentUser,
        L"Meet Gesture FX Camera",
        kMediaSourceClsid,
        nullptr,
        0,
        &camera);
    if (SUCCEEDED(hr)) hr = camera->Remove();
    if (camera)
    {
        camera->Shutdown();
        camera->Release();
    }
    return hr;
}

static void PrintPhysicalMediaTypes(IMFActivate** devices, UINT32 deviceCount)
{
    if (deviceCount == 0) return;
    IMFMediaSource* source = nullptr;
    HRESULT hr = devices[0]->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr))
    {
        PrintHr(L"Activate physical camera", hr);
        return;
    }

    IMFPresentationDescriptor* presentation = nullptr;
    hr = source->CreatePresentationDescriptor(&presentation);
    if (SUCCEEDED(hr))
    {
        DWORD streamCount = 0;
        presentation->GetStreamDescriptorCount(&streamCount);
        for (DWORD stream = 0; stream < streamCount; ++stream)
        {
            BOOL selected = FALSE;
            IMFStreamDescriptor* descriptor = nullptr;
            if (FAILED(presentation->GetStreamDescriptorByIndex(stream, &selected, &descriptor))) continue;
            IMFMediaTypeHandler* handler = nullptr;
            if (SUCCEEDED(descriptor->GetMediaTypeHandler(&handler)))
            {
                DWORD typeCount = 0;
                handler->GetMediaTypeCount(&typeCount);
                std::wcout << L"Physical stream " << stream << L" has " << typeCount << L" types\n";
                for (DWORD typeIndex = 0; typeIndex < typeCount; ++typeIndex)
                {
                    IMFMediaType* type = nullptr;
                    if (FAILED(handler->GetMediaTypeByIndex(typeIndex, &type))) continue;
                    GUID subtype = GUID_NULL;
                    UINT32 width = 0;
                    UINT32 height = 0;
                    UINT32 numerator = 0;
                    UINT32 denominator = 0;
                    type->GetGUID(MF_MT_SUBTYPE, &subtype);
                    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
                    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator);
                    std::wcout << L"  " << typeIndex << L": " << width << L"x" << height
                        << L" subtype=" << std::hex << subtype.Data1 << std::dec
                        << L" fps=" << numerator << L"/" << denominator << L"\n";
                    type->Release();
                }
                handler->Release();
            }
            descriptor->Release();
        }
        presentation->Release();
    }
    if (FAILED(hr)) PrintHr(L"Read physical camera types", hr);
    source->Shutdown();
    source->Release();
}

static void PrintBackgroundSegmentationSupport(IMFActivate** devices, UINT32 deviceCount)
{
    if (deviceCount == 0) return;

    IMFMediaSource* source = nullptr;
    HRESULT hr = devices[0]->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr))
    {
        PrintHr(L"Activate physical camera for segmentation query", hr);
        return;
    }

    IKsControl* ksControl = nullptr;
    hr = source->QueryInterface(IID_PPV_ARGS(&ksControl));
    if (FAILED(hr))
    {
        PrintHr(L"Query IKsControl", hr);
        source->Shutdown();
        source->Release();
        return;
    }

    KSPROPERTY property{
        KSPROPERTYSETID_ExtendedCameraControl,
        KSPROPERTY_CAMERACONTROL_EXTENDED_BACKGROUNDSEGMENTATION,
        KSPROPERTY_TYPE_GET };
    BYTE payload[4096] = {};
    ULONG returned = 0;
    hr = ksControl->KsProperty(&property, sizeof(property), payload, sizeof(payload), &returned);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA))
    {
        const auto* header = reinterpret_cast<const KSCAMERA_EXTENDEDPROP_HEADER*>(payload);
        std::wcout << L"Background segmentation query succeeded; bytes=" << returned
            << L" capability=0x" << std::hex << header->Capability
            << L" flags=0x" << header->Flags << std::dec << L"\n";
        std::wcout << L"Mask metadata supported="
            << (((header->Capability & KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_MASK) != 0) ? L"yes" : L"no")
            << L"\n";
    }
    else
    {
        PrintHr(L"Background segmentation query", hr);
    }

    ksControl->Release();
    source->Shutdown();
    source->Release();
}

static void PrintWindowsStudioFaceMetadataSupport(IMFActivate** devices, UINT32 deviceCount)
{
    IMFActivate* selected = nullptr;
    for (UINT32 i = 0; i < deviceCount; ++i)
    {
        WCHAR* link = nullptr;
        UINT32 linkLength = 0;
        if (FAILED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                &link,
                &linkLength)))
        {
            continue;
        }
        const bool isVirtual = wcsstr(link, L"vcamdevapi") != nullptr;
        CoTaskMemFree(link);
        if (!isVirtual)
        {
            selected = devices[i];
            selected->AddRef();
            break;
        }
    }

    if (selected == nullptr)
    {
        std::wcerr << L"No physical camera was found for Windows Studio query.\n";
        return;
    }

    IMFMediaSource* source = nullptr;
    HRESULT hr = selected->ActivateObject(IID_PPV_ARGS(&source));
    selected->Release();
    if (FAILED(hr))
    {
        PrintHr(L"Activate physical camera for Windows Studio query", hr);
        return;
    }

    IKsControl* ksControl = nullptr;
    hr = source->QueryInterface(IID_PPV_ARGS(&ksControl));
    if (FAILED(hr))
    {
        PrintHr(L"Query IKsControl for Windows Studio query", hr);
        source->Shutdown();
        source->Release();
        return;
    }

    std::vector<BYTE> supportedPayload(4096, 0);
    auto* supportedHeader = reinterpret_cast<KSCAMERA_EXTENDEDPROP_HEADER*>(supportedPayload.data());
    supportedHeader->Version = 1;
    supportedHeader->PinId = KSCAMERA_EXTENDEDPROP_FILTERSCOPE;
    supportedHeader->Size = static_cast<ULONG>(supportedPayload.size());
    KSPROPERTY supportedProperty{
        kWindowsStudioCameraControlPropertySet,
        kWindowsStudioSupportedProperty,
        KSPROPERTY_TYPE_GET };
    ULONG returned = 0;
    hr = ksControl->KsProperty(
        &supportedProperty,
        sizeof(supportedProperty),
        supportedPayload.data(),
        static_cast<ULONG>(supportedPayload.size()),
        &returned);
    if (FAILED(hr))
    {
        PrintHr(L"Windows Studio supported-controls query", hr);
        ksControl->Release();
        source->Shutdown();
        source->Release();
        return;
    }

    const ULONG payloadSize = std::min<ULONG>(returned, static_cast<ULONG>(supportedPayload.size()));
    const ULONG propertyBytes = payloadSize > sizeof(KSCAMERA_EXTENDEDPROP_HEADER)
        ? payloadSize - sizeof(KSCAMERA_EXTENDEDPROP_HEADER)
        : 0;
    const ULONG propertyCount = propertyBytes / sizeof(KSPROPERTY);
    bool faceMetadataListed = false;
    for (ULONG i = 0; i < propertyCount; ++i)
    {
        const auto* entry = reinterpret_cast<const KSPROPERTY*>(
            supportedPayload.data() + sizeof(KSCAMERA_EXTENDEDPROP_HEADER) + i * sizeof(KSPROPERTY));
        if (IsEqualGUID(entry->Set, kWindowsStudioCameraControlPropertySet)
            && entry->Id == kWindowsStudioFaceMetadataProperty)
        {
            faceMetadataListed = true;
        }
    }
    std::wcout << L"Windows Studio face metadata control listed="
        << (faceMetadataListed ? L"yes" : L"no") << L"\n";

    struct FaceMetadataPayload
    {
        KSCAMERA_EXTENDEDPROP_HEADER header;
        KSCAMERA_EXTENDEDPROP_VALUE value;
    } facePayload = {};
    facePayload.header.Version = 1;
    facePayload.header.PinId = KSCAMERA_EXTENDEDPROP_FILTERSCOPE;
    facePayload.header.Size = sizeof(facePayload);
    KSPROPERTY faceProperty{
        kWindowsStudioCameraControlPropertySet,
        kWindowsStudioFaceMetadataProperty,
        KSPROPERTY_TYPE_GET };
    returned = 0;
    hr = ksControl->KsProperty(
        &faceProperty,
        sizeof(faceProperty),
        &facePayload,
        sizeof(facePayload),
        &returned);
    if (SUCCEEDED(hr))
    {
        std::wcout << L"Windows Studio face metadata capability=0x"
            << std::hex << facePayload.header.Capability << std::dec << L"\n";
        std::wcout << L"Windows Studio landmarks supported="
            << ((facePayload.header.Capability & kWindowsStudioFaceLandmarksFlag) != 0 ? L"yes" : L"no")
            << L"\n";
    }
    else
    {
        PrintHr(L"Windows Studio face metadata capability query", hr);
    }

    ksControl->Release();
    source->Shutdown();
    source->Release();
}

static int ProbeVirtualCamera(IMFActivate** devices, UINT32 deviceCount)
{
    IMFActivate* selectedDevice = nullptr;
    for (UINT32 i = 0; i < deviceCount; ++i)
    {
        WCHAR* name = nullptr;
        UINT32 nameLength = 0;
        WCHAR* link = nullptr;
        UINT32 linkLength = 0;
        HRESULT nameHr = devices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
            &name,
            &nameLength);
        HRESULT hr = devices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            &link,
            &linkLength);
        const bool isTarget = SUCCEEDED(nameHr) && name != nullptr
            && wcsstr(name, L"Meet Gesture FX Camera") != nullptr;
        if (name) CoTaskMemFree(name);
        if (FAILED(hr)) continue;

        const bool isVirtual = wcsstr(link, L"vcamdevapi") != nullptr;
        CoTaskMemFree(link);
        if (!isVirtual || !isTarget) continue;

        selectedDevice = devices[i];
        selectedDevice->AddRef();
        break;
    }

    if (selectedDevice == nullptr)
    {
        std::wcerr << L"Meet Gesture FX Camera was not found.\n";
        return 1;
    }

        IMFMediaSource* source = nullptr;
        HRESULT hr = selectedDevice->ActivateObject(IID_PPV_ARGS(&source));
        selectedDevice->Release();
        if (FAILED(hr))
        {
            PrintHr(L"Activate virtual camera", hr);
            return 1;
        }

        IMFSourceReader* reader = nullptr;
        hr = MFCreateSourceReaderFromMediaSource(source, nullptr, &reader);
        if (SUCCEEDED(hr))
        {
            UINT32 frameWidth = 0;
            UINT32 frameHeight = 0;
            IMFMediaType* mediaType = nullptr;
            hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &mediaType);
            if (SUCCEEDED(hr))
            {
                GUID subtype = GUID_NULL;
                mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
                MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &frameWidth, &frameHeight);
                std::wcout << L"Virtual camera stream: " << frameWidth << L"x" << frameHeight
                    << L" subtype=" << std::hex << subtype.Data1 << std::dec << L"\n";
                reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType);
                mediaType->Release();
            }

            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            for (int attempt = 0; attempt < 60 && sample == nullptr && SUCCEEDED(hr); ++attempt)
            {
                flags = 0;
                hr = reader->ReadSample(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                    0,
                    &streamIndex,
                    &flags,
                    &timestamp,
                    &sample);
                if (sample == nullptr && flags != 0)
                {
                    std::wcout << L"ReadSample flags=0x" << std::hex << flags << std::dec << L"\n";
                }
            }
            if (SUCCEEDED(hr) && sample != nullptr)
            {
                IMFMediaBuffer* buffer = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
                {
                    BYTE* bytes = nullptr;
                    DWORD maxLength = 0;
                    DWORD currentLength = 0;
                    if (SUCCEEDED(buffer->Lock(&bytes, &maxLength, &currentLength)))
                    {
                        std::wcout << L"First frame received: " << currentLength
                            << L" bytes, first byte=" << static_cast<unsigned>(bytes[0]) << L"\n";
                        UINT blackLuma = 0;
                        UINT redEye = 0;
                        for (DWORD offset = 0; offset + 3 < currentLength; offset += 4)
                        {
                            const BYTE y0 = bytes[offset];
                            const BYTE u = bytes[offset + 1];
                            const BYTE y1 = bytes[offset + 2];
                            const BYTE v = bytes[offset + 3];
                            if (y0 <= 25) ++blackLuma;
                            if (y1 <= 25) ++blackLuma;
                            if (y0 >= 55 && y0 <= 105 && u >= 55 && u <= 115 && v >= 210) ++redEye;
                            if (y1 >= 55 && y1 <= 105 && u >= 55 && u <= 115 && v >= 210) ++redEye;
                        }
                        std::wcout << L"Diagnostic pixels: black-luma=" << blackLuma
                            << L" red-eye-like=" << redEye << L"\n";
                        WriteYuy2Ppm(bytes, currentLength, frameWidth, frameHeight);
                        buffer->Unlock();
                    }
                    buffer->Release();
                }
                sample->Release();
            }
            else if (SUCCEEDED(hr))
            {
                std::wcerr << L"No sample arrived after 60 reads.\n";
            }
            else if (FAILED(hr))
            {
                PrintHr(L"Read first virtual-camera frame", hr);
            }
        }
        if (reader) reader->Release();
        source->Shutdown();
        source->Release();
        return FAILED(hr) ? 1 : 0;
    }

int wmain(int argc, wchar_t** argv)
{
    AppendLog(L"start");
    if (argc > 1 && _wcsicmp(argv[1], L"--register-source") == 0)
    {
        const HRESULT registerResult = RegisterMediaSourceClass();
        if (FAILED(registerResult)) PrintHr(L"Register media source class", registerResult);
        return FAILED(registerResult) ? 1 : 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        PrintHr(L"CoInitializeEx", hr);
        return 1;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        PrintHr(L"MFStartup", hr);
        CoUninitialize();
        return 1;
    }

    AppendLog(L"MFStartup succeeded");

    if (argc > 1 && _wcsicmp(argv[1], L"--remove") == 0)
    {
        const HRESULT removeResult = RemoveMeetGestureCamera();
        if (FAILED(removeResult)) PrintHr(L"Remove virtual camera", removeResult);
        MFShutdown();
        CoUninitialize();
        return FAILED(removeResult) ? 1 : 0;
    }

    IMFActivate** devices = nullptr;
    UINT32 deviceCount = 0;
    IMFAttributes* enumAttributes = nullptr;
    hr = MFCreateAttributes(&enumAttributes, 1);
    if (SUCCEEDED(hr))
    {
        hr = enumAttributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }
    if (SUCCEEDED(hr))
    {
        hr = MFEnumDeviceSources(enumAttributes, &devices, &deviceCount);
    }
    if (FAILED(hr) || deviceCount == 0)
    {
        if (FAILED(hr)) PrintHr(L"MFEnumDeviceSources", hr);
        else std::wcerr << L"No physical camera was found.\n";
        if (enumAttributes) enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--list") == 0)
    {
        for (UINT32 i = 0; i < deviceCount; ++i)
        {
            WCHAR* name = nullptr;
            UINT32 nameLength = 0;
            WCHAR* link = nullptr;
            UINT32 linkLength = 0;
            devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLength);
            devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &linkLength);
            std::wcout << (i + 1) << L": " << (name ? name : L"(unnamed)") << L"\n";
            std::wcout << L"   " << (link ? link : L"(no symbolic link)") << L"\n";
            if (name) CoTaskMemFree(name);
            if (link) CoTaskMemFree(link);
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
        enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--probe") == 0)
    {
        const int result = ProbeVirtualCamera(devices, deviceCount);
        for (UINT32 i = 0; i < deviceCount; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return result;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--info") == 0)
    {
        PrintVirtualCameraProperties(devices, deviceCount);
        for (UINT32 i = 0; i < deviceCount; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--physical-types") == 0)
    {
        PrintPhysicalMediaTypes(devices, deviceCount);
        for (UINT32 i = 0; i < deviceCount; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--segmentation-info") == 0)
    {
        PrintBackgroundSegmentationSupport(devices, deviceCount);
        for (UINT32 i = 0; i < deviceCount; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"--wse-info") == 0)
    {
        PrintWindowsStudioFaceMetadataSupport(devices, deviceCount);
        for (UINT32 i = 0; i < deviceCount; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    UINT32 selected = 0;
    if (argc > 1)
    {
        selected = static_cast<UINT32>(_wtoi(argv[1]));
        if (selected > 0) --selected;
    }
    if (selected >= deviceCount)
    {
        std::wcerr << L"Camera index must be between 1 and " << deviceCount << L".\n";
        for (UINT32 i = 0; i < deviceCount; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    WCHAR* physicalLink = nullptr;
    UINT32 physicalLinkLength = 0;
    WCHAR* physicalName = nullptr;
    UINT32 physicalNameLength = 0;
    hr = devices[selected]->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        &physicalLink,
        &physicalLinkLength);
    if (SUCCEEDED(hr))
    {
        hr = devices[selected]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
            &physicalName,
            &physicalNameLength);
    }
    if (FAILED(hr))
    {
        PrintHr(L"Read physical camera", hr);
        if (physicalLink) CoTaskMemFree(physicalLink);
        if (physicalName) CoTaskMemFree(physicalName);
        for (UINT32 i = 0; i < deviceCount; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        enumAttributes->Release();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    std::wcout << L"Using camera: " << (physicalName ? physicalName : L"(unnamed)") << L"\n";

    IMFVirtualCamera* virtualCamera = nullptr;
    hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_CurrentUser,
        L"Meet Gesture FX Camera",
        kMediaSourceClsid,
        nullptr,
        0,
        &virtualCamera);
    if (FAILED(hr)) PrintHr(L"MFCreateVirtualCamera", hr);
    else AppendLog(L"MFCreateVirtualCamera succeeded");
    if (SUCCEEDED(hr)) hr = virtualCamera->AddDeviceSourceInfo(physicalLink);
    if (FAILED(hr)) PrintHr(L"AddDeviceSourceInfo", hr);
    else AppendLog(L"AddDeviceSourceInfo succeeded");

    IMFAttributes* sourceAttributes = nullptr;
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&sourceAttributes, 2);
    if (FAILED(hr)) PrintHr(L"MFCreateAttributes", hr);
    if (SUCCEEDED(hr)) hr = sourceAttributes->SetString(kVcamDeviceInfo, physicalLink);
    if (FAILED(hr)) PrintHr(L"Set device attribute", hr);
    if (SUCCEEDED(hr)) hr = sourceAttributes->SetUINT32(kVcamKind, 2);
    if (FAILED(hr)) PrintHr(L"Set camera-kind attribute", hr);

    if (SUCCEEDED(hr))
    {
        UINT32 itemCount = 0;
        hr = sourceAttributes->GetCount(&itemCount);
        for (UINT32 i = 0; SUCCEEDED(hr) && i < itemCount; ++i)
        {
            GUID key = GUID_NULL;
            PROPVARIANT value;
            PropVariantInit(&value);
            hr = sourceAttributes->GetItemByIndex(i, &key, &value);
            if (SUCCEEDED(hr)) hr = virtualCamera->SetItem(key, value);
            PropVariantClear(&value);
        }
        if (FAILED(hr)) PrintHr(L"Set activation attribute", hr);
    }

    const LONG lifetime = MFVirtualCameraLifetime_System;
    const LONG access = MFVirtualCameraAccess_CurrentUser;
    if (SUCCEEDED(hr))
    {
        hr = virtualCamera->AddProperty(
            &kSourceId,
            DEVPROP_TYPE_STRING,
            reinterpret_cast<const BYTE*>(kMediaSourceClsid),
            static_cast<ULONG>(sizeof(kMediaSourceClsid)));
        if (FAILED(hr)) PrintHr(L"Add source-id property", hr);
    }
    if (SUCCEEDED(hr))
    {
        hr = virtualCamera->AddProperty(
            &kFriendlyName,
            DEVPROP_TYPE_STRING,
            reinterpret_cast<const BYTE*>(L"Meet Gesture FX Camera"),
            static_cast<ULONG>(sizeof(L"Meet Gesture FX Camera")));
        if (FAILED(hr)) PrintHr(L"Add friendly-name property", hr);
    }
    if (SUCCEEDED(hr)) hr = virtualCamera->AddProperty(&kLifetime, DEVPROP_TYPE_INT32, reinterpret_cast<const BYTE*>(&lifetime), sizeof(lifetime));
    if (FAILED(hr)) PrintHr(L"Add lifetime property", hr);
    if (SUCCEEDED(hr)) hr = virtualCamera->AddProperty(&kAccess, DEVPROP_TYPE_INT32, reinterpret_cast<const BYTE*>(&access), sizeof(access));
    if (FAILED(hr)) PrintHr(L"Add access property", hr);
    if (SUCCEEDED(hr)) hr = virtualCamera->Start(nullptr);
    if (FAILED(hr)) PrintHr(L"Start virtual camera", hr);
    else AppendLog(L"Start virtual camera succeeded");

    if (FAILED(hr))
    {
        PrintHr(L"Register virtual camera", hr);
    }
    else
    {
        WCHAR* virtualLink = nullptr;
        UINT32 virtualLinkLength = 0;
        if (SUCCEEDED(virtualCamera->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            &virtualLink,
            &virtualLinkLength)))
        {
            std::wcout << L"Registered: " << virtualLink << L"\n";
            CoTaskMemFree(virtualLink);
        }
        std::wcout << L"Meet Gesture FX Camera is ready.\n";
    }

    if (virtualCamera) virtualCamera->Shutdown();
    if (sourceAttributes) sourceAttributes->Release();
    if (virtualCamera) virtualCamera->Release();
    CoTaskMemFree(physicalLink);
    CoTaskMemFree(physicalName);
    for (UINT32 i = 0; i < deviceCount; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    enumAttributes->Release();
    MFShutdown();
    CoUninitialize();
    return FAILED(hr) ? 1 : 0;
}
