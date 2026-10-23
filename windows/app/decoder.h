#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

enum Codec
{
    CodecH264 = 0,
    CodecHEVC = 1,
};

// Called once per decoded frame with the visible NV12 image.
using FrameCallback = std::function<void(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int width,
                                         int height, int64_t ptsUs, bool fullRange)>;

// Low-latency H.264/HEVC decoder on top of the Media Foundation decoder MFTs,
// using D3D11 (DXVA) acceleration when the GPU supports it.
class Decoder
{
public:
    // Bitmask of codecs this PC can decode: bit0 = H.264, bit1 = HEVC.
    static int SupportedCodecs();

    bool Init(int codec, int width, int height, int fps, std::wstring& error);
    bool Decode(const uint8_t* data, size_t size, int64_t ptsUs, bool keyFrame, const FrameCallback& onFrame);
    bool IsHardware() const { return _hardware; }
    bool IsReady() const { return _mft != nullptr; }
    HRESULT LastError() const { return _lastError; }

private:
    bool ConfigureOutput();
    bool Drain(const FrameCallback& onFrame);
    void Emit(IMFSample* sample, const FrameCallback& onFrame);

    ComPtr<IMFTransform> _mft;
    ComPtr<ID3D11Device> _device;
    ComPtr<IMFDXGIDeviceManager> _manager;
    ComPtr<IMFSample> _outSample;
    ComPtr<ID3D11Texture2D> _staging;
    D3D11_TEXTURE2D_DESC _stagingDesc{};
    bool _hardware = false;
    bool _providesSamples = false;
    DWORD _outBufferSize = 0;
    int _width = 0, _height = 0;      // visible size reported by the phone
    UINT32 _outWidth = 0, _outHeight = 0; // coded size of the decoder output
    LONG _stride = 0;
    bool _fullRange = false;
    HRESULT _lastError = S_OK;
    int _codec = CodecH264;
    std::vector<uint8_t> _patched;
};
