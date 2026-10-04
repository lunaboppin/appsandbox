#ifndef _APPSANDBOXVAD_MICWAVEFILTER_H_
#define _APPSANDBOXVAD_MICWAVEFILTER_H_

//=============================================================================
// Microphone wave (WaveRT capture) filter
//
//   Bridge from topology (pin 0) -----------------> Host capture stream (pin 1)
//
// One format only (48 kHz, 16-bit, stereo): the data comes straight from the
// AppSandbox mic feed ring buffer, which carries exactly that.
//=============================================================================

#define MICIN_MAX_INPUT_STREAMS     1

static
KSDATAFORMAT_WAVEFORMATEXTENSIBLE MicInPinSupportedFormats[] =
{
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                MICIN_CHANNELS,
                MICIN_SAMPLE_RATE,
                MICIN_BYTES_PER_SEC,
                MICIN_BLOCK_ALIGN,
                MICIN_BITS_PER_SAMPLE,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            MICIN_BITS_PER_SAMPLE,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    }
};

static
MODE_AND_DEFAULT_FORMAT MicInPinSupportedModes[] =
{
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &MicInPinSupportedFormats[0].DataFormat
    }
};

// Indexed by wave pin ID.
static
PIN_DEVICE_FORMATS_AND_MODES MicInPinDeviceFormatsAndModes[] =
{
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0
    },
    {
        SystemCapturePin,
        MicInPinSupportedFormats,
        SIZEOF_ARRAY(MicInPinSupportedFormats),
        MicInPinSupportedModes,
        SIZEOF_ARRAY(MicInPinSupportedModes)
    }
};

static
KSDATARANGE_AUDIO MicInWaveDataRangesStream[] =
{
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        MICIN_CHANNELS,         // MaximumChannels
        MICIN_BITS_PER_SAMPLE,  // MinimumBitsPerSample
        MICIN_BITS_PER_SAMPLE,  // MaximumBitsPerSample
        MICIN_SAMPLE_RATE,      // MinimumSampleFrequency
        MICIN_SAMPLE_RATE       // MaximumSampleFrequency
    }
};

static
PKSDATARANGE MicInWaveDataRangePointersStream[] =
{
    PKSDATARANGE(&MicInWaveDataRangesStream[0]),
    PKSDATARANGE(&VadDataRangeAttributeList),
};

static
KSDATARANGE MicInWaveDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static
PKSDATARANGE MicInWaveDataRangePointersBridge[] =
{
    &MicInWaveDataRangesBridge[0]
};

static
PCPIN_DESCRIPTOR MicInWaveMiniportPins[] =
{
    // KSPIN_WAVE_CAPTURE_BRIDGE
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicInWaveDataRangePointersBridge),
            MicInWaveDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // KSPIN_WAVE_CAPTURE_SOURCE_HOST
    {
        MICIN_MAX_INPUT_STREAMS,
        MICIN_MAX_INPUT_STREAMS,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicInWaveDataRangePointersStream),
            MicInWaveDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    },
};

static
PCCONNECTION_DESCRIPTOR MicInWaveConnections[] =
{
    { PCFILTER_NODE, KSPIN_WAVE_CAPTURE_BRIDGE, PCFILTER_NODE, KSPIN_WAVE_CAPTURE_SOURCE_HOST }
};

static
PCPROPERTY_ITEM MicInWaveFilterProperties[] =
{
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadWaveFilterHandler
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadWaveFilterHandler
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicInWaveFilter, MicInWaveFilterProperties);

static
PCFILTER_DESCRIPTOR MicInWaveFilterDescriptor =
{
    0,                                              // Version
    &AutomationMicInWaveFilter,                     // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(MicInWaveMiniportPins),            // PinCount
    MicInWaveMiniportPins,                          // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    0,                                              // NodeCount
    NULL,                                           // Nodes
    SIZEOF_ARRAY(MicInWaveConnections),             // ConnectionCount
    MicInWaveConnections,                           // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories
};

#endif // _APPSANDBOXVAD_MICWAVEFILTER_H_
