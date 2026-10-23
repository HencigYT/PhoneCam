#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <wrl.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "framereader.h"
#include "../common/shared.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ChainInterfaces;

class MediaSource;

class MediaStream : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                        ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
                                        IKsControl>
{
public:
    ~MediaStream();
    HRESULT RuntimeClassInitialize(MediaSource* source, int camera);

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    IFACEMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength, ULONG* returned) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID data, ULONG dataLength, ULONG* returned) override;
    IFACEMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID data, ULONG dataLength, ULONG* returned) override;

    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();
    IMFStreamDescriptor* Descriptor() const { return _descriptor.Get(); }

private:
    void StopWorker();
    void Worker();
    void Deliver(IUnknown* token);

    std::mutex _lock;
    std::condition_variable _cv;
    std::deque<ComPtr<IUnknown>> _tokens;
    std::thread _thread;
    std::atomic<bool> _running{ false };
    bool _shutdown = false;
    MF_STREAM_STATE _state = MF_STREAM_STATE_STOPPED;

    MediaSource* _parent = nullptr; // owner; cleared on shutdown
    ComPtr<IMFMediaEventQueue> _queue;
    ComPtr<IMFStreamDescriptor> _descriptor;

    UINT32 _width = 1920, _height = 1080, _fps = 60;
    bool _yuy2 = false;
    LONGLONG _lastDeliver = 0;
    FrameReader _reader;
};

class MediaSource : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                        ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                                        IMFGetService, IKsControl, IMFSampleAllocatorControl>
{
public:
    ~MediaSource();
    HRESULT RuntimeClassInitialize(IMFAttributes* attributes, int camera);

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    IFACEMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    IFACEMETHODIMP Pause() override;
    IFACEMETHODIMP Shutdown() override;
    IFACEMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    IFACEMETHODIMP Stop() override;

    // IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    IFACEMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    IFACEMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    IFACEMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength, ULONG* returned) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID data, ULONG dataLength, ULONG* returned) override;
    IFACEMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID data, ULONG dataLength, ULONG* returned) override;

    // IMFSampleAllocatorControl
    IFACEMETHODIMP SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) override;
    IFACEMETHODIMP GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

private:
    std::recursive_mutex _lock;
    bool _mfStarted = false;
    bool _shutdown = false;
    bool _streamAnnounced = false;
    ComPtr<IMFAttributes> _attributes;
    ComPtr<IMFMediaEventQueue> _queue;
    ComPtr<IMFPresentationDescriptor> _presentation;
    ComPtr<MediaStream> _stream;
};

// The classes registered for the cameras (pcam::kCameraClsids). The frame server
// creates one and calls ActivateObject to get the media source.
class ActivatorBase
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>>
{
protected:
    // The WRL class factory does not call RuntimeClassInitialize, so the store is made here.
    explicit ActivatorBase(int camera) : _camera(camera) { MFCreateAttributes(&_attrs, 4); }

public:
    HRESULT RuntimeClassInitialize();

    // IMFActivate
    IFACEMETHODIMP ActivateObject(REFIID riid, void** object) override;
    IFACEMETHODIMP ShutdownObject() override;
    IFACEMETHODIMP DetachObject() override;

    // IMFAttributes, forwarded to an attribute store
    IFACEMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return _attrs->GetItem(k, v); }
    IFACEMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return _attrs->GetItemType(k, t); }
    IFACEMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return _attrs->CompareItem(k, v, r); }
    IFACEMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return _attrs->Compare(a, t, r); }
    IFACEMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return _attrs->GetUINT32(k, v); }
    IFACEMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return _attrs->GetUINT64(k, v); }
    IFACEMETHODIMP GetDouble(REFGUID k, double* v) override { return _attrs->GetDouble(k, v); }
    IFACEMETHODIMP GetGUID(REFGUID k, GUID* v) override { return _attrs->GetGUID(k, v); }
    IFACEMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return _attrs->GetStringLength(k, l); }
    IFACEMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 s, UINT32* l) override { return _attrs->GetString(k, v, s, l); }
    IFACEMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override { return _attrs->GetAllocatedString(k, v, l); }
    IFACEMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return _attrs->GetBlobSize(k, s); }
    IFACEMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 s, UINT32* l) override { return _attrs->GetBlob(k, b, s, l); }
    IFACEMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return _attrs->GetAllocatedBlob(k, b, s); }
    IFACEMETHODIMP GetUnknown(REFGUID k, REFIID riid, LPVOID* v) override { return _attrs->GetUnknown(k, riid, v); }
    IFACEMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return _attrs->SetItem(k, v); }
    IFACEMETHODIMP DeleteItem(REFGUID k) override { return _attrs->DeleteItem(k); }
    IFACEMETHODIMP DeleteAllItems() override { return _attrs->DeleteAllItems(); }
    IFACEMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return _attrs->SetUINT32(k, v); }
    IFACEMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return _attrs->SetUINT64(k, v); }
    IFACEMETHODIMP SetDouble(REFGUID k, double v) override { return _attrs->SetDouble(k, v); }
    IFACEMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return _attrs->SetGUID(k, v); }
    IFACEMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return _attrs->SetString(k, v); }
    IFACEMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 s) override { return _attrs->SetBlob(k, b, s); }
    IFACEMETHODIMP SetUnknown(REFGUID k, IUnknown* v) override { return _attrs->SetUnknown(k, v); }
    IFACEMETHODIMP LockStore() override { return _attrs->LockStore(); }
    IFACEMETHODIMP UnlockStore() override { return _attrs->UnlockStore(); }
    IFACEMETHODIMP GetCount(UINT32* c) override { return _attrs->GetCount(c); }
    IFACEMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return _attrs->GetItemByIndex(i, k, v); }
    IFACEMETHODIMP CopyAllItems(IMFAttributes* d) override { return _attrs->CopyAllItems(d); }

private:
    int _camera;
    ComPtr<IMFAttributes> _attrs;
    ComPtr<MediaSource> _source;
};

class __declspec(uuid("7B1E3C52-9A4D-4F6B-8E21-5C3D9F0A6B17")) Activator1 : public ActivatorBase
{
public:
    Activator1() : ActivatorBase(0) {}
};

class __declspec(uuid("7B1E3C52-9A4D-4F6B-8E21-5C3D9F0A6B18")) Activator2 : public ActivatorBase
{
public:
    Activator2() : ActivatorBase(1) {}
};

class __declspec(uuid("7B1E3C52-9A4D-4F6B-8E21-5C3D9F0A6B19")) Activator3 : public ActivatorBase
{
public:
    Activator3() : ActivatorBase(2) {}
};
