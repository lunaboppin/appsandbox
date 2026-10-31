/*
 * vm_web_encoder.c -- Media Foundation H.264 encoder (see vm_web_encoder.h).
 *
 * mfplat.dll is loaded at run time: Windows N editions don't have it, and the rest
 * of the app must still start there.
 */

#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <stdio.h>
#include "vm_web_encoder.h"
#include "ui.h"

#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

/* From codecapi.h, whose GUID macros only work from C++. */
static const GUID ASB_AVLowLatencyMode =
    { 0x9c27891a, 0xed7a, 0x40e1, { 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee } };
static const GUID ASB_AVEncCommonRateControlMode =
    { 0x1c0608e9, 0x370c, 0x4710, { 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23 } };
static const GUID ASB_AVEncCommonMeanBitRate =
    { 0xf7222374, 0x2144, 0x4815, { 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52 } };
static const GUID ASB_AVEncVideoForceKeyFrame =
    { 0x398c1b98, 0x8353, 0x475a, { 0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45 } };
static const GUID ASB_AVEncMPVGOPSize =
    { 0x95f31b26, 0x95a4, 0x41aa, { 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 } };
static const GUID ASB_AVEncMPVDefaultBPictureCount =
    { 0x8d390aac, 0xdc5c, 0x4200, { 0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2 } };
static const GUID ASB_IID_ICodecAPI =
    { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };

#define RATE_CONTROL_CBR 0   /* eAVEncCommonRateControlMode_CBR */
#define H264_PROFILE_HIGH 100

typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PFN_MFTEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
                                        const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *);
typedef HRESULT (WINAPI *PFN_MFCreateSample)(IMFSample **);
typedef HRESULT (WINAPI *PFN_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer **);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(IMFMediaType **);

static PFN_MFTEnumEx            p_MFTEnumEx;
static PFN_MFCreateSample       p_MFCreateSample;
static PFN_MFCreateMemoryBuffer p_MFCreateMemoryBuffer;
static PFN_MFCreateMediaType    p_MFCreateMediaType;

static BOOL mf_load(void)
{
    static LONG state;   /* 0 = not tried, 1 = loaded, 2 = unavailable */
    if (state == 0) {
        HMODULE dll = LoadLibraryW(L"mfplat.dll");
        LONG result = 2;
        if (dll) {
            PFN_MFStartup startup = (PFN_MFStartup)GetProcAddress(dll, "MFStartup");
            p_MFTEnumEx = (PFN_MFTEnumEx)GetProcAddress(dll, "MFTEnumEx");
            p_MFCreateSample = (PFN_MFCreateSample)GetProcAddress(dll, "MFCreateSample");
            p_MFCreateMemoryBuffer = (PFN_MFCreateMemoryBuffer)GetProcAddress(dll, "MFCreateMemoryBuffer");
            p_MFCreateMediaType = (PFN_MFCreateMediaType)GetProcAddress(dll, "MFCreateMediaType");
            if (startup && p_MFTEnumEx && p_MFCreateSample && p_MFCreateMemoryBuffer &&
                p_MFCreateMediaType && SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE)))
                result = 1;
        }
        InterlockedExchange(&state, result);
    }
    return state == 1;
}

struct WebEncoder {
    IMFTransform *mft;
    IMFMediaEventGenerator *events;   /* NULL: synchronous (software) transform */
    ICodecAPI *codec;
    UINT width, height, fps;
    BOOL hardware;
    LONG need_input;
    LONGLONG next_time;
    BOOL provides_samples;
    DWORD output_size;
    BYTE *seq_header;
    UINT32 seq_header_size;
    BYTE *out_buf;
    DWORD out_cap;
    WebEncoderOutput output;
    void *user;
};

static void set_codec_uint(ICodecAPI *c, const GUID *g, UINT32 value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    ICodecAPI_SetValue(c, g, &v);
}

static void set_codec_bool(ICodecAPI *c, const GUID *g, BOOL value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    ICodecAPI_SetValue(c, g, &v);
}

static void read_seq_header(WebEncoder *e)
{
    IMFMediaType *type = NULL;
    UINT32 size = 0;
    if (SUCCEEDED(IMFTransform_GetOutputCurrentType(e->mft, 0, &type))) {
        if (SUCCEEDED(IMFMediaType_GetBlobSize(type, &MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
            BYTE *blob = (BYTE *)malloc(size);
            if (blob && SUCCEEDED(IMFMediaType_GetBlob(type, &MF_MT_MPEG_SEQUENCE_HEADER, blob, size, NULL))) {
                free(e->seq_header);
                e->seq_header = blob;
                e->seq_header_size = size;
            } else {
                free(blob);
            }
        }
        IMFMediaType_Release(type);
    }
}

static BOOL configure(WebEncoder *e, UINT bits)
{
    IMFMediaType *out = NULL, *in = NULL;
    MFT_OUTPUT_STREAM_INFO info;
    HRESULT hr;

    if (SUCCEEDED(IMFTransform_QueryInterface(e->mft, &ASB_IID_ICodecAPI, (void **)&e->codec))) {
        set_codec_bool(e->codec, &ASB_AVLowLatencyMode, TRUE);
        set_codec_uint(e->codec, &ASB_AVEncCommonRateControlMode, RATE_CONTROL_CBR);
        set_codec_uint(e->codec, &ASB_AVEncCommonMeanBitRate, bits);
        set_codec_uint(e->codec, &ASB_AVEncMPVDefaultBPictureCount, 0);
        set_codec_uint(e->codec, &ASB_AVEncMPVGOPSize, e->fps * 10);
    }

    if (FAILED(p_MFCreateMediaType(&out))) return FALSE;
    IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(out, &MF_MT_AVG_BITRATE, bits);
    IMFMediaType_SetUINT64(out, &MF_MT_FRAME_SIZE, ((UINT64)e->width << 32) | e->height);
    IMFMediaType_SetUINT64(out, &MF_MT_FRAME_RATE, ((UINT64)e->fps << 32) | 1);
    IMFMediaType_SetUINT64(out, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    IMFMediaType_SetUINT32(out, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(out, &MF_MT_MPEG2_PROFILE, H264_PROFILE_HIGH);
    hr = IMFTransform_SetOutputType(e->mft, 0, out, 0);
    IMFMediaType_Release(out);
    if (FAILED(hr)) return FALSE;

    if (FAILED(p_MFCreateMediaType(&in))) return FALSE;
    IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_SetUINT64(in, &MF_MT_FRAME_SIZE, ((UINT64)e->width << 32) | e->height);
    IMFMediaType_SetUINT64(in, &MF_MT_FRAME_RATE, ((UINT64)e->fps << 32) | 1);
    IMFMediaType_SetUINT64(in, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(in, &MF_MT_DEFAULT_STRIDE, e->width);
    hr = IMFTransform_SetInputType(e->mft, 0, in, 0);
    IMFMediaType_Release(in);
    if (FAILED(hr)) return FALSE;

    if (FAILED(IMFTransform_GetOutputStreamInfo(e->mft, 0, &info))) return FALSE;
    e->provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                           MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    e->output_size = info.cbSize ? info.cbSize : e->width * e->height * 2;
    read_seq_header(e);

    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return TRUE;
}

/* First transform of the kind that accepts this size. */
static BOOL try_kind(WebEncoder *e, UINT32 flags, UINT bits)
{
    MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out_info = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate **list = NULL;
    UINT32 count = 0, i;
    BOOL ok = FALSE;

    if (FAILED(p_MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags | MFT_ENUM_FLAG_SORTANDFILTER,
                           &in_info, &out_info, &list, &count)))
        return FALSE;
    for (i = 0; i < count; i++) {
        if (!ok) {
            IMFAttributes *attrs = NULL;
            e->events = NULL;
            e->codec = NULL;
            if (SUCCEEDED(IMFActivate_ActivateObject(list[i], &IID_IMFTransform, (void **)&e->mft))) {
                UINT32 is_async = 0;
                if (SUCCEEDED(IMFTransform_GetAttributes(e->mft, &attrs))) {
                    IMFAttributes_GetUINT32(attrs, &MF_TRANSFORM_ASYNC, &is_async);
                    if (is_async) {
                        IMFAttributes_SetUINT32(attrs, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                        IMFTransform_QueryInterface(e->mft, &IID_IMFMediaEventGenerator, (void **)&e->events);
                    }
                    IMFAttributes_Release(attrs);
                }
                ok = configure(e, bits);
                if (!ok) {
                    if (e->events) { IMFMediaEventGenerator_Release(e->events); e->events = NULL; }
                    if (e->codec) { ICodecAPI_Release(e->codec); e->codec = NULL; }
                    IMFTransform_Release(e->mft);
                    e->mft = NULL;
                }
            }
        }
        IMFActivate_Release(list[i]);
    }
    CoTaskMemFree(list);
    return ok;
}

WebEncoder *web_encoder_create(UINT width, UINT height, UINT fps, UINT bits_per_second,
                               WebEncoderOutput output, void *user)
{
    WebEncoder *e;
    if (!mf_load()) {
        ui_log(L"Browser access: Media Foundation isn't installed (a Windows N edition?), so video can't be encoded.");
        return NULL;
    }
    e = (WebEncoder *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->width = width;
    e->height = height;
    e->fps = fps;
    e->output = output;
    e->user = user;
    e->hardware = try_kind(e, MFT_ENUM_FLAG_HARDWARE, bits_per_second);
    if (!e->hardware && !try_kind(e, MFT_ENUM_FLAG_SYNCMFT, bits_per_second)) {
        free(e);
        return NULL;
    }
    return e;
}

void web_encoder_destroy(WebEncoder *e)
{
    if (!e) return;
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    if (e->events) {
        IMFShutdown *shutdown = NULL;
        IMFMediaEventGenerator_Release(e->events);
        if (SUCCEEDED(IMFTransform_QueryInterface(e->mft, &IID_IMFShutdown, (void **)&shutdown))) {
            IMFShutdown_Shutdown(shutdown);
            IMFShutdown_Release(shutdown);
        }
    }
    if (e->codec) ICodecAPI_Release(e->codec);
    IMFTransform_Release(e->mft);
    free(e->seq_header);
    free(e->out_buf);
    free(e);
}

BOOL web_encoder_is_hardware(const WebEncoder *e) { return e && e->hardware; }

void web_encoder_force_key(WebEncoder *e)
{
    if (e && e->codec) set_codec_uint(e->codec, &ASB_AVEncVideoForceKeyFrame, 1);
}

void web_encoder_set_bitrate(WebEncoder *e, UINT bits_per_second)
{
    if (e && e->codec) set_codec_uint(e->codec, &ASB_AVEncCommonMeanBitRate, bits_per_second);
}

/* Which NAL unit types an access unit holds: bit n for type n. */
static UINT32 nal_types(const BYTE *p, DWORD size)
{
    UINT32 types = 0;
    DWORD i;
    for (i = 0; i + 3 < size; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            types |= 1u << (p[i + 3] & 31);
            i += 3;
        }
    }
    return types;
}

static void deliver(WebEncoder *e, IMFSample *sample)
{
    IMFMediaBuffer *buffer = NULL;
    BYTE *data;
    DWORD len = 0;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buffer))) return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, &len))) {
        UINT32 types = nal_types(data, len);
        BOOL key = (types & (1u << 5)) != 0;
        /* A decoder joining at a key frame needs SPS and PPS with it. */
        if (key && !(types & (1u << 7)) && e->seq_header) {
            DWORD total = e->seq_header_size + len;
            if (total > e->out_cap) {
                BYTE *bigger = (BYTE *)realloc(e->out_buf, total);
                if (bigger) { e->out_buf = bigger; e->out_cap = total; }
            }
            if (total <= e->out_cap) {
                memcpy(e->out_buf, e->seq_header, e->seq_header_size);
                memcpy(e->out_buf + e->seq_header_size, data, len);
                e->output(e->user, e->out_buf, total, TRUE);
            }
        } else if (len) {
            e->output(e->user, data, len, key);
        }
        IMFMediaBuffer_Unlock(buffer);
    }
    IMFMediaBuffer_Release(buffer);
}

/* Fetch one finished frame. S_OK when one came, MF_E_TRANSFORM_NEED_MORE_INPUT when
   there is none; a format change is handled here and asks for a retry. */
static HRESULT fetch_output(WebEncoder *e)
{
    MFT_OUTPUT_DATA_BUFFER out;
    DWORD status = 0;
    IMFSample *own = NULL;
    HRESULT hr;

    ZeroMemory(&out, sizeof(out));
    if (!e->provides_samples) {
        IMFMediaBuffer *buffer = NULL;
        if (FAILED(p_MFCreateSample(&own))) return E_OUTOFMEMORY;
        if (FAILED(p_MFCreateMemoryBuffer(e->output_size, &buffer))) {
            IMFSample_Release(own);
            return E_OUTOFMEMORY;
        }
        IMFSample_AddBuffer(own, buffer);
        IMFMediaBuffer_Release(buffer);
        out.pSample = own;
    }
    hr = IMFTransform_ProcessOutput(e->mft, 0, 1, &out, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        IMFMediaType *type = NULL;
        if (SUCCEEDED(IMFTransform_GetOutputAvailableType(e->mft, 0, 0, &type))) {
            IMFTransform_SetOutputType(e->mft, 0, type, 0);
            IMFMediaType_Release(type);
        }
        read_seq_header(e);
        if (own) IMFSample_Release(own);
        if (out.pEvents) IMFCollection_Release(out.pEvents);
        return S_FALSE;
    }
    if (SUCCEEDED(hr) && out.pSample) deliver(e, out.pSample);
    if (out.pSample && out.pSample != own) IMFSample_Release(out.pSample);
    if (own) IMFSample_Release(own);
    if (out.pEvents) IMFCollection_Release(out.pEvents);
    return hr;
}

BOOL web_encoder_pump(WebEncoder *e)
{
    if (!e) return FALSE;
    if (!e->events) return TRUE;
    for (;;) {
        IMFMediaEvent *ev = NULL;
        MediaEventType type = MEUnknown;
        HRESULT hr = IMFMediaEventGenerator_GetEvent(e->events, MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) return TRUE;
        if (FAILED(hr)) return FALSE;
        IMFMediaEvent_GetType(ev, &type);
        IMFMediaEvent_Release(ev);
        if (type == METransformNeedInput) {
            e->need_input++;
        } else if (type == METransformHaveOutput) {
            hr = fetch_output(e);
            if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) return FALSE;
        } else if (type == MEError) {
            return FALSE;
        }
    }
}

BOOL web_encoder_ready(const WebEncoder *e)
{
    return e && (!e->events || e->need_input > 0);
}

BOOL web_encoder_submit(WebEncoder *e, const BYTE *nv12)
{
    IMFSample *sample = NULL;
    IMFMediaBuffer *buffer = NULL;
    BYTE *dest;
    const DWORD size = e->width * e->height * 3 / 2;
    const LONGLONG duration = 10000000LL / e->fps;
    HRESULT hr;

    if (FAILED(p_MFCreateMemoryBuffer(size, &buffer))) return FALSE;
    if (FAILED(p_MFCreateSample(&sample))) { IMFMediaBuffer_Release(buffer); return FALSE; }
    if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &dest, NULL, NULL))) {
        memcpy(dest, nv12, size);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_SetCurrentLength(buffer, size);
    }
    IMFSample_AddBuffer(sample, buffer);
    IMFSample_SetSampleTime(sample, e->next_time);
    IMFSample_SetSampleDuration(sample, duration);
    e->next_time += duration;
    hr = IMFTransform_ProcessInput(e->mft, 0, sample, 0);
    IMFMediaBuffer_Release(buffer);
    IMFSample_Release(sample);
    if (e->events) {
        if (e->need_input > 0) e->need_input--;
        return SUCCEEDED(hr);
    }
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return FALSE;
    for (;;) {
        hr = fetch_output(e);
        if (hr == S_FALSE) continue;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return TRUE;
        if (FAILED(hr)) return FALSE;
    }
}
