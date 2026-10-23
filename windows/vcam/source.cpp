#include "source.h"
#include <algorithm>

using Microsoft::WRL::Make;
using Microsoft::WRL::MakeAndInitialize;

#define RETURN_IF_FAILED(expr)          \
    do                                  \
    {                                   \
        HRESULT hr_ = (expr);           \
        if (FAILED(hr_))                \
            return hr_;                 \
    } while (0)

static const HRESULT kNotSupported = HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);

// ---------------------------------------------------------------------------
// Media types

static HRESULT MakeVideoType(const GUID& subtype, UINT32 w, UINT32 h, UINT32 fps, IMFMediaType** out)
{
    ComPtr<IMFMediaType> t;
    RETURN_IF_FAILED(MFCreateMediaType(&t));
    bool yuy2 = subtype == MFVideoFormat_YUY2;
    UINT32 stride = yuy2 ? w * 2 : w;
    UINT32 size = yuy2 ? w * h * 2 : w * h * 3 / 2;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, size);
    t->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::min<UINT64>((UINT64)size * 8 * fps, 0xFFFFFFFFu));
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    *out = t.Detach();
    return S_OK;
}

struct Format
{
    UINT32 w, h, fps;
    bool yuy2;
};

static std::vector<Format> BuildFormatList(FrameReader& reader)
{
    std::vector<Format> list;
    // The phone's current resolution goes first so clients that just take the
    // default get the stream without any scaling.
    uint32_t sw, sh, sfps;
    if (reader.SourceFormat(sw, sh, sfps) && sw % 2 == 0 && sh % 2 == 0)
        list.push_back({ sw, sh, sfps >= 50 ? 60u : 30u, false });

    const Format standard[] = {
        { 1920, 1080, 60, false }, { 1920, 1080, 30, false }, { 1280, 720, 60, false }, { 1280, 720, 30, false },
        { 2560, 1440, 60, false }, { 3840, 2160, 60, false }, { 3840, 2160, 30, false }, { 960, 540, 30, false },
        { 640, 360, 30, false },   { 1920, 1080, 30, true },  { 1280, 720, 30, true },
    };
    for (const Format& f : standard)
    {
        bool dup = std::any_of(list.begin(), list.end(), [&](const Format& o) {
            return o.w == f.w && o.h == f.h && o.fps == f.fps && o.yuy2 == f.yuy2;
        });
        if (!dup)
            list.push_back(f);
    }
    return list;
}

// ---------------------------------------------------------------------------
// MediaStream

MediaStream::~MediaStream()
{
    StopWorker();
}

HRESULT MediaStream::RuntimeClassInitialize(MediaSource* source, int camera)
{
    _reader.SetCamera(camera);
    _parent = source;
    RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

    std::vector<Format> formats = BuildFormatList(_reader);
    std::vector<ComPtr<IMFMediaType>> types;
    std::vector<IMFMediaType*> raw;
    for (const Format& f : formats)
    {
        ComPtr<IMFMediaType> t;
        RETURN_IF_FAILED(MakeVideoType(f.yuy2 ? MFVideoFormat_YUY2 : MFVideoFormat_NV12, f.w, f.h, f.fps, &t));
        raw.push_back(t.Get());
        types.push_back(t);
    }
    RETURN_IF_FAILED(MFCreateStreamDescriptor(0, (DWORD)raw.size(), raw.data(), &_descriptor));

    ComPtr<IMFMediaTypeHandler> handler;
    RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
    RETURN_IF_FAILED(handler->SetCurrentMediaType(raw[0]));

    RETURN_IF_FAILED(_descriptor->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RETURN_IF_FAILED(_descriptor->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0));
    RETURN_IF_FAILED(_descriptor->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(_descriptor->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    return S_OK;
}

IFACEMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    return _queue->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    return _queue->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    return _queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value)
{
    return _queue->QueueEventParamVar(type, extType, status, value);
}

IFACEMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source)
{
    if (!source)
        return E_POINTER;
    std::lock_guard<std::mutex> lk(_lock);
    if (_shutdown || !_parent)
        return MF_E_SHUTDOWN;
    return _parent->QueryInterface(IID_PPV_ARGS(source));
}

IFACEMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor)
{
    if (!descriptor)
        return E_POINTER;
    std::lock_guard<std::mutex> lk(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    return _descriptor.CopyTo(descriptor);
}

IFACEMETHODIMP MediaStream::RequestSample(IUnknown* token)
{
    {
        std::lock_guard<std::mutex> lk(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        if (_state != MF_STREAM_STATE_RUNNING)
            return MF_E_INVALIDREQUEST;
        _tokens.push_back(ComPtr<IUnknown>(token));
    }
    _cv.notify_one();
    return S_OK;
}

IFACEMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state)
{
    {
        std::lock_guard<std::mutex> lk(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        if (state == _state)
            return S_OK;
        if (state == MF_STREAM_STATE_PAUSED)
        {
            if (_state != MF_STREAM_STATE_RUNNING)
                return MF_E_INVALID_STATE_TRANSITION;
            _state = state;
            return S_OK;
        }
    }
    if (state == MF_STREAM_STATE_RUNNING)
        return Start(nullptr);
    if (state == MF_STREAM_STATE_STOPPED)
        return Stop();
    return E_INVALIDARG;
}

IFACEMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state)
{
    if (!state)
        return E_POINTER;
    std::lock_guard<std::mutex> lk(_lock);
    *state = _state;
    return S_OK;
}

IFACEMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) { return kNotSupported; }
IFACEMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) { return kNotSupported; }
IFACEMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) { return kNotSupported; }

HRESULT MediaStream::Start(IMFMediaType* type)
{
    ComPtr<IMFMediaTypeHandler> handler;
    {
        std::lock_guard<std::mutex> lk(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
    }

    ComPtr<IMFMediaType> current(type);
    if (current)
        handler->SetCurrentMediaType(current.Get());
    else
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&current));

    GUID subtype = GUID_NULL;
    UINT32 w = 0, h = 0, num = 0, den = 0;
    current->GetGUID(MF_MT_SUBTYPE, &subtype);
    RETURN_IF_FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h));
    if (FAILED(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &num, &den)) || !num || !den)
    {
        num = 30;
        den = 1;
    }

    StopWorker();
    {
        std::lock_guard<std::mutex> lk(_lock);
        _width = w & ~1u;
        _height = h & ~1u;
        _fps = std::max(1u, num / den);
        _yuy2 = subtype == MFVideoFormat_YUY2;
        _state = MF_STREAM_STATE_RUNNING;
        _running = true;
        _thread = std::thread(&MediaStream::Worker, this);
    }

    PROPVARIANT time;
    PropVariantInit(&time);
    time.vt = VT_I8;
    time.hVal.QuadPart = MFGetSystemTime();
    return _queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &time);
}

HRESULT MediaStream::Stop()
{
    {
        std::lock_guard<std::mutex> lk(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
    }
    StopWorker();
    {
        std::lock_guard<std::mutex> lk(_lock);
        _state = MF_STREAM_STATE_STOPPED;
    }
    return _queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown()
{
    StopWorker();
    std::lock_guard<std::mutex> lk(_lock);
    if (_shutdown)
        return;
    _shutdown = true;
    _state = MF_STREAM_STATE_STOPPED;
    _parent = nullptr;
    _queue->Shutdown();
}

void MediaStream::StopWorker()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lk(_lock);
        _running = false;
        _tokens.clear();
        worker = std::move(_thread);
    }
    _cv.notify_all();
    if (worker.joinable())
    {
        if (worker.get_id() == std::this_thread::get_id())
            worker.detach();
        else
            worker.join();
    }
}

void MediaStream::Worker()
{
    for (;;)
    {
        ComPtr<IUnknown> token;
        {
            std::unique_lock<std::mutex> lk(_lock);
            _cv.wait(lk, [this] { return !_running || !_tokens.empty(); });
            if (!_running)
                return;
            token = _tokens.front();
            _tokens.pop_front();
        }

        // Hand out each phone frame as soon as it lands. Without a phone, repeat
        // the last image (or the placeholder) ~10x per second so clients stay alive.
        ULONGLONG waitStart = GetTickCount64();
        bool pace = _fps + 5 < _reader.SourceFps();
        LONGLONG minInterval = 10000000LL / _fps * 3 / 4;
        while (_running)
        {
            ULONGLONG elapsed = GetTickCount64() - waitStart;
            if (elapsed >= 100 || !_reader.WaitForNewFrame((DWORD)(100 - elapsed)))
                break;
            if (!pace || MFGetSystemTime() - _lastDeliver >= minInterval)
                break;
            _reader.SkipCurrent(); // client wants fewer fps than the phone sends
        }
        if (!_running)
            return;
        Deliver(token.Get());
    }
}

void MediaStream::Deliver(IUnknown* token)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreate2DMediaBuffer(_width, _height, _yuy2 ? FCC('YUY2') : FCC('NV12'), FALSE, &buffer)))
        return;
    ComPtr<IMF2DBuffer2> buffer2d;
    if (FAILED(buffer.As(&buffer2d)))
        return;
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    if (FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &length)))
        return;
    _reader.Render(scan0, pitch, _width, _height, _yuy2);
    buffer2d->Unlock2D();
    DWORD contiguous = 0;
    buffer2d->GetContiguousLength(&contiguous);
    buffer->SetCurrentLength(contiguous);

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample)))
        return;
    sample->AddBuffer(buffer.Get());
    LONGLONG now = MFGetSystemTime();
    sample->SetSampleTime(now);
    sample->SetSampleDuration(10000000LL / _fps);
    if (token)
        sample->SetUnknown(MFSampleExtension_Token, token);

    std::lock_guard<std::mutex> lk(_lock);
    if (_running && !_shutdown && _state == MF_STREAM_STATE_RUNNING)
    {
        _lastDeliver = now;
        _queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }
}

// ---------------------------------------------------------------------------
// MediaSource

MediaSource::~MediaSource()
{
    Shutdown();
    if (_mfStarted)
        MFShutdown();
}

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* attributes, int camera)
{
    RETURN_IF_FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    _mfStarted = true;
    RETURN_IF_FAILED(MFCreateAttributes(&_attributes, 4));
    if (attributes)
        attributes->CopyAllItems(_attributes.Get());
    RETURN_IF_FAILED(MFCreateEventQueue(&_queue));
    RETURN_IF_FAILED(MakeAndInitialize<MediaStream>(&_stream, this, camera));
    IMFStreamDescriptor* descriptor = _stream->Descriptor();
    RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, &descriptor, &_presentation));
    RETURN_IF_FAILED(_presentation->SelectStream(0));
    return S_OK;
}

IFACEMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    return _queue->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    return _queue->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    return _queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value)
{
    return _queue->QueueEventParamVar(type, extType, status, value);
}

IFACEMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor)
{
    if (!descriptor)
        return E_POINTER;
    std::lock_guard<std::recursive_mutex> lk(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    return _presentation->Clone(descriptor);
}

IFACEMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics)
{
    if (!characteristics)
        return E_POINTER;
    std::lock_guard<std::recursive_mutex> lk(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

IFACEMETHODIMP MediaSource::Pause()
{
    return MF_E_INVALID_STATE_TRANSITION;
}

IFACEMETHODIMP MediaSource::Shutdown()
{
    std::lock_guard<std::recursive_mutex> lk(_lock);
    if (_shutdown)
        return S_OK;
    _shutdown = true;
    if (_stream)
        _stream->Shutdown();
    if (_queue)
        _queue->Shutdown();
    _stream.Reset();
    return S_OK;
}

IFACEMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                  const PROPVARIANT* startPosition)
{
    if (!descriptor || !startPosition)
        return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL)
        return MF_E_UNSUPPORTED_TIME_FORMAT;

    std::lock_guard<std::recursive_mutex> lk(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;

    DWORD count = 0;
    RETURN_IF_FAILED(descriptor->GetStreamDescriptorCount(&count));
    HRESULT result = S_OK;
    for (DWORD i = 0; i < count; i++)
    {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        RETURN_IF_FAILED(descriptor->GetStreamDescriptorByIndex(i, &selected, &sd));
        DWORD id = 0;
        sd->GetStreamIdentifier(&id);
        if (id != 0)
            continue;

        if (selected)
        {
            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&handler)))
                handler->GetCurrentMediaType(&type);

            ComPtr<IMFMediaStream> stream;
            _stream.As(&stream);
            _queue->QueueEventParamUnk(_streamAnnounced ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                       stream.Get());
            _streamAnnounced = true;
            result = _stream->Start(type.Get());
        }
        else
        {
            _stream->Stop();
        }
    }
    _queue->QueueEventParamVar(MESourceStarted, GUID_NULL, result, startPosition);
    return result;
}

IFACEMETHODIMP MediaSource::Stop()
{
    std::lock_guard<std::recursive_mutex> lk(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    _stream->Stop();
    return _queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

IFACEMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes)
{
    if (!attributes)
        return E_POINTER;
    std::lock_guard<std::recursive_mutex> lk(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    return _attributes.CopyTo(attributes);
}

IFACEMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes)
{
    if (!attributes)
        return E_POINTER;
    std::lock_guard<std::recursive_mutex> lk(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    if (streamId != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    return _stream->Descriptor()->QueryInterface(IID_PPV_ARGS(attributes));
}

IFACEMETHODIMP MediaSource::SetD3DManager(IUnknown*)
{
    return S_OK; // samples are produced in system memory
}

IFACEMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID*)
{
    return MF_E_UNSUPPORTED_SERVICE;
}

IFACEMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) { return kNotSupported; }
IFACEMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) { return kNotSupported; }
IFACEMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) { return kNotSupported; }

IFACEMETHODIMP MediaSource::SetDefaultAllocator(DWORD, IUnknown*)
{
    return S_OK;
}

IFACEMETHODIMP MediaSource::GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage)
{
    if (!inputStreamId || !usage)
        return E_POINTER;
    *inputStreamId = outputStreamId;
    *usage = MFSampleAllocatorUsage_UsesCustomAllocator;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Activator

HRESULT ActivatorBase::RuntimeClassInitialize()
{
    return _attrs ? S_OK : MFCreateAttributes(&_attrs, 4);
}

IFACEMETHODIMP ActivatorBase::ActivateObject(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (!_source)
        RETURN_IF_FAILED(MakeAndInitialize<MediaSource>(&_source, _attrs.Get(), _camera));
    return _source->QueryInterface(riid, object);
}

IFACEMETHODIMP ActivatorBase::ShutdownObject()
{
    if (_source)
    {
        _source->Shutdown();
        _source.Reset();
    }
    return S_OK;
}

IFACEMETHODIMP ActivatorBase::DetachObject()
{
    _source.Reset();
    return S_OK;
}
