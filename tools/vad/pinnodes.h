#ifndef _APPSANDBOXVAD_PINNODES_H_
#define _APPSANDBOXVAD_PINNODES_H_

// Name GUID for this driver
// {3E5F7A2C-1B8D-4C9E-A0F6-5D4E3C2B1A09}
#define STATIC_NAME_APPSANDBOXVAD \
    0x3e5f7a2c, 0x1b8d, 0x4c9e, 0xa0, 0xf6, 0x5d, 0x4e, 0x3c, 0x2b, 0x1a, 0x09
DEFINE_GUIDSTRUCT("3E5F7A2C-1B8D-4C9E-A0F6-5D4E3C2B1A09", NAME_APPSANDBOXVAD);
#define NAME_APPSANDBOXVAD DEFINE_GUIDNAMED(NAME_APPSANDBOXVAD)

//=============================================================================
// Render pin layout (render3 pattern: system sink + bridge source)
//=============================================================================

// Default pin instances
#define MAX_INPUT_SYSTEM_STREAMS    1

// Wave pins
enum
{
    KSPIN_WAVE_RENDER_SINK_SYSTEM = 0,
    KSPIN_WAVE_RENDER_SOURCE
};

// Wave topology nodes (unused in render3 — direct passthrough)
enum
{
    KSNODE_WAVE_DAC = 0
};

//=============================================================================
// Topology pins
//=============================================================================

enum
{
    KSPIN_TOPO_WAVEOUT_SOURCE = 0,
    KSPIN_TOPO_LINEOUT_DEST,
};

// Topology nodes
enum
{
    KSNODE_TOPO_VOLUME = 0,
    KSNODE_TOPO_MUTE
};

//=============================================================================
// Microphone (capture) pin layout
//=============================================================================

// Wave pins: bridge in from the topology filter, streaming out to the OS
enum
{
    KSPIN_WAVE_CAPTURE_BRIDGE = 0,
    KSPIN_WAVE_CAPTURE_SOURCE_HOST
};

// Topology pins: the (virtual) microphone jack, bridge out to the wave filter
enum
{
    KSPIN_TOPO_MIC_ELEMENTS = 0,
    KSPIN_TOPO_MIC_BRIDGE
};

//=============================================================================
// AppSandbox microphone feed property set (on the mic topology filter)
//
// The guest audio helper pushes the host microphone's PCM (48 kHz, 16-bit,
// stereo) with KSPROPERTY_ASBMIC_DATA (SET), and polls
// KSPROPERTY_ASBMIC_STATE (GET, ULONG = running capture streams) so the host
// only opens its microphone while something in the guest is recording.
// {6A1C3E52-9F0D-4B7E-A431-5C8E2D7F9016}
//=============================================================================
#define STATIC_KSPROPSETID_AsbMic     0x6a1c3e52, 0x9f0d, 0x4b7e, 0xa4, 0x31, 0x5c, 0x8e, 0x2d, 0x7f, 0x90, 0x16
DEFINE_GUIDSTRUCT("6A1C3E52-9F0D-4B7E-A431-5C8E2D7F9016", KSPROPSETID_AsbMic);
#define KSPROPSETID_AsbMic DEFINE_GUIDNAMED(KSPROPSETID_AsbMic)

enum
{
    KSPROPERTY_ASBMIC_DATA = 0,
    KSPROPERTY_ASBMIC_STATE
};

// Mic format: fixed so the feed needs no conversion in the kernel.
#define MICIN_SAMPLE_RATE       48000
#define MICIN_CHANNELS          2
#define MICIN_BITS_PER_SAMPLE   16
#define MICIN_BLOCK_ALIGN       (MICIN_CHANNELS * MICIN_BITS_PER_SAMPLE / 8)
#define MICIN_BYTES_PER_SEC     (MICIN_SAMPLE_RATE * MICIN_BLOCK_ALIGN)
#define MICIN_MAX_FEED_BYTES    (64 * 1024)     // largest single DATA write

//=============================================================================
// Signal processing mode attribute (for data range attributes)
//=============================================================================

static
KSATTRIBUTE VadSignalProcessingModeAttribute =
{
    sizeof(KSATTRIBUTE),
    0,
    STATICGUIDOF(KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE),
};

static
PKSATTRIBUTE VadDataRangeAttributes[] =
{
    &VadSignalProcessingModeAttribute,
};

static
KSATTRIBUTE_LIST VadDataRangeAttributeList =
{
    ARRAYSIZE(VadDataRangeAttributes),
    VadDataRangeAttributes,
};

#endif // _APPSANDBOXVAD_PINNODES_H_
