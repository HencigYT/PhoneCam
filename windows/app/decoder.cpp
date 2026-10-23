#include "decoder.h"
#include "lang.h"
#include "h264patch.h"
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <algorithm>

static ComPtr<IMFTransform> CreateDecoderMft(const GUID& subtype)
{
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, subtype };
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_NV12 };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    ComPtr<IMFTransform> mft;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                         &activates, &count)))
        return mft;
    for (UINT32 i = 0; i < count; i++)
    {
        if (!mft)
            activates[i]->ActivateObject(IID_PPV_ARGS(&mft));
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return mft;
}

int Decoder::SupportedCodecs()
{
    int mask = 0;
    if (CreateDecoderMft(MFVideoFormat_H264))
        mask |= 1;
    if (CreateDecoderMft(MFVideoFormat_HEVC))
        mask |= 2;
    return mask;
}

bool Decoder::Init(int codec, int width, int height, int fps, std::wstring& error)
{
    _mft.Reset();
    _manager.Reset();
    _device.Reset();
    _outSample.Reset();
    _staging.Reset();
    _hardware = false;
    _codec = codec;
    _width = width & ~1;
    _height = height & ~1;

    const GUID& subtype = codec == CodecHEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    _mft = CreateDecoderMft(subtype);
    if (!_mft)
    {
        error = codec == CodecHEVC
                    ? lang::tr(L"Nincs HEVC dekóder (telepítsd a \"HEVC Video Extensions\"-t vagy válts H.264-re a telefonon)",
                               L"No HEVC decoder (install \"HEVC Video Extensions\" or switch to H.264 on the phone)")
                    : lang::tr(L"Nincs H.264 dekóder a rendszerben", L"No H.264 decoder on this system");
        return false;
    }

    ComPtr<IMFAttributes> attrs;
    UINT32 d3dAware = 0;
    if (SUCCEEDED(_mft->GetAttributes(&attrs)))
    {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
    }
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(_mft.As(&codecApi)))
    {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    if (d3dAware && GetEnvironmentVariableW(L"PHONECAM_SOFTWARE_DECODE", nullptr, 0) == 0)
    {
        static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                        ARRAYSIZE(levels), D3D11_SDK_VERSION, &_device, nullptr, nullptr)))
        {
            ComPtr<ID3D10Multithread> mt;
            if (SUCCEEDED(_device.As(&mt)))
                mt->SetMultithreadProtected(TRUE);
            UINT token = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &_manager)) &&
                SUCCEEDED(_manager->ResetDevice(_device.Get(), token)) &&
                SUCCEEDED(_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)_manager.Get())))
            {
                _hardware = true;
            }
            else
            {
                _manager.Reset();
                _device.Reset();
            }
        }
    }

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps > 0 ? fps : 30, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    HRESULT hr = _mft->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr))
    {
        wchar_t buf[128];
        swprintf_s(buf, lang::tr(L"A dekóder nem fogadja el a formátumot (%dx%d, 0x%08X)", L"The decoder rejects the format (%dx%d, 0x%08X)"), width, height, (unsigned)hr);
        error = buf;
        _mft.Reset();
        return false;
    }
    if (!ConfigureOutput())
    {
        error = lang::tr(L"A dekóder nem ad NV12 kimenetet", L"The decoder has no NV12 output");
        _mft.Reset();
        return false;
    }
    _mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    _mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool Decoder::ConfigureOutput()
{
    for (DWORD i = 0;; i++)
    {
        ComPtr<IMFMediaType> type;
        if (FAILED(_mft->GetOutputAvailableType(0, i, &type)))
            return false;
        GUID sub;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12)
            continue;
        if (FAILED(_mft->SetOutputType(0, type.Get(), 0)))
            return false;

        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &_outWidth, &_outHeight);
        UINT32 stride = 0;
        _stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : (LONG)_outWidth;
        _fullRange = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) ==
                     MFNominalRange_0_255;

        MFT_OUTPUT_STREAM_INFO info{};
        _mft->GetOutputStreamInfo(0, &info);
        _providesSamples =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        _outBufferSize = info.cbSize;
        _outSample.Reset();
        return true;
    }
}

bool Decoder::Decode(const uint8_t* data, size_t size, int64_t ptsUs, bool keyFrame, const FrameCallback& onFrame)
{
    if (!_mft)
        return false;
    // Key frames carry the SPS; declare "no reordering" so frames come out immediately.
    if (keyFrame && _codec == CodecH264 && PatchH264NoReorder(data, size, _patched))
    {
        data = _patched.data();
        size = _patched.size();
    }

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer((DWORD)size, &buffer)))
        return false;
    BYTE* p = nullptr;
    buffer->Lock(&p, nullptr, nullptr);
    memcpy(p, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)size);

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(ptsUs * 10);
    if (keyFrame)
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

    HRESULT hr = _mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING)
    {
        if (!Drain(onFrame))
            return false;
        hr = _mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr))
    {
        _lastError = hr;
        return false;
    }
    return Drain(onFrame);
}

bool Decoder::Drain(const FrameCallback& onFrame)
{
    for (int guard = 0; guard < 16; guard++)
    {
        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = 0;
        if (!_providesSamples)
        {
            // A fresh sample every time: the software decoder fails (E_FAIL) on
            // key frames when handed back a sample it already filled.
            _outSample.Reset();
            ComPtr<IMFMediaBuffer> buf;
            DWORD size = std::max<DWORD>(_outBufferSize, _outWidth * _outHeight * 3 / 2);
            if (FAILED(MFCreateMemoryBuffer(size, &buf)) || FAILED(MFCreateSample(&_outSample)))
                return false;
            _outSample->AddBuffer(buf.Get());
            out.pSample = _outSample.Get();
        }
        DWORD status = 0;
        HRESULT hr = _mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents)
            out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            if (_providesSamples && out.pSample)
                out.pSample->Release();
            if (!ConfigureOutput())
                return false;
            continue;
        }
        if (FAILED(hr))
        {
            if (_providesSamples && out.pSample)
                out.pSample->Release();
            _lastError = hr;
            return false;
        }
        if (_providesSamples)
        {
            ComPtr<IMFSample> owned;
            owned.Attach(out.pSample);
            if (owned)
                Emit(owned.Get(), onFrame);
        }
        else
        {
            Emit(_outSample.Get(), onFrame);
        }
    }
    return true;
}

void Decoder::Emit(IMFSample* sample, const FrameCallback& onFrame)
{
    LONGLONG time = 0;
    sample->GetSampleTime(&time);
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)))
        return;

    int width = std::min<int>(_width, (int)_outWidth) & ~1;
    int height = std::min<int>(_height, (int)_outHeight) & ~1;

    // GPU output: copy the decoded slice into a reusable staging texture and map
    // it (IMF2DBuffer::Lock2D on DXGI buffers is far slower).
    ComPtr<IMFDXGIBuffer> dxgi;
    ComPtr<ID3D11Texture2D> tex;
    UINT subresource = 0;
    if (_device && SUCCEEDED(buffer.As(&dxgi)) && SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&tex))) &&
        SUCCEEDED(dxgi->GetSubresourceIndex(&subresource)))
    {
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        if (!_staging || desc.Width != _stagingDesc.Width || desc.Height != _stagingDesc.Height ||
            desc.Format != _stagingDesc.Format)
        {
            _staging.Reset();
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.ArraySize = 1;
            sd.MipLevels = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            if (FAILED(_device->CreateTexture2D(&sd, nullptr, &_staging)))
                return;
            _stagingDesc = desc;
        }
        ComPtr<ID3D11DeviceContext> ctx;
        _device->GetImmediateContext(&ctx);
        ctx->CopySubresourceRegion(_staging.Get(), 0, 0, 0, 0, tex.Get(), subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(_staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        {
            const uint8_t* y = (const uint8_t*)map.pData;
            int pitch = (int)map.RowPitch;
            onFrame(y, pitch, y + (size_t)pitch * desc.Height, pitch, width, height, time / 10, _fullRange);
            ctx->Unmap(_staging.Get(), 0);
        }
        return;
    }

    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d)))
    {
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch)))
        {
            onFrame(scan0, pitch, scan0 + (size_t)pitch * _outHeight, pitch, width, height, time / 10, _fullRange);
            buffer2d->Unlock2D();
        }
        return;
    }
    BYTE* p = nullptr;
    if (SUCCEEDED(buffer->Lock(&p, nullptr, nullptr)))
    {
        onFrame(p, _stride, p + (size_t)_stride * _outHeight, _stride, width, height, time / 10, _fullRange);
        buffer->Unlock();
    }
}
