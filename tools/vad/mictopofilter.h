#ifndef _APPSANDBOXVAD_MICTOPOFILTER_H_
#define _APPSANDBOXVAD_MICTOPOFILTER_H_

//=============================================================================
// Microphone topology filter
//
//   Mic jack (pin 0) ---------------------------> Bridge to wave (pin 1)
//
// No volume/mute nodes: Windows then applies the capture volume in software,
// and the mic does not share mixer state with the speaker topology.
//=============================================================================

static
KSDATARANGE MicTopoPinDataRangesBridge[] =
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
PKSDATARANGE MicTopoPinDataRangePointersBridge[] =
{
    &MicTopoPinDataRangesBridge[0]
};

static
PCPIN_DESCRIPTOR MicTopoMiniportPins[] =
{
    // KSPIN_TOPO_MIC_ELEMENTS
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
            SIZEOF_ARRAY(MicTopoPinDataRangePointersBridge),
            MicTopoPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE,
            NULL,
            0
        }
    },
    // KSPIN_TOPO_MIC_BRIDGE
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
            SIZEOF_ARRAY(MicTopoPinDataRangePointersBridge),
            MicTopoPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    }
};

//=============================================================================
// Jack description (always connected, integrated)
//=============================================================================

static
KSJACK_DESCRIPTION MicJackDesc =
{
    KSAUDIO_SPEAKER_STEREO,
    JACKDESC_RGB(0xD6, 0x4A, 0x8C),    // pink-ish, the usual mic colour
    eConnTypeUnknown,
    eGeoLocFront,
    eGenLocPrimaryBox,
    ePortConnIntegratedDevice,
    TRUE                                // IsConnected = always
};

static
PKSJACK_DESCRIPTION MicJackDescriptions[] =
{
    &MicJackDesc,
    NULL
};

//=============================================================================
// Topology connections: Mic jack -> bridge
//=============================================================================

static
PCCONNECTION_DESCRIPTOR MicTopoConnections[] =
{
    { PCFILTER_NODE, KSPIN_TOPO_MIC_ELEMENTS, PCFILTER_NODE, KSPIN_TOPO_MIC_BRIDGE }
};

//=============================================================================
// Topology filter properties: jack description + the AppSandbox mic feed
//=============================================================================

static
PCPROPERTY_ITEM MicTopoFilterProperties[] =
{
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadMicFilterHandler
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadMicFilterHandler
    },
    {
        &KSPROPSETID_AsbMic,
        KSPROPERTY_ASBMIC_DATA,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadMicFeedHandler
    },
    {
        &KSPROPSETID_AsbMic,
        KSPROPERTY_ASBMIC_STATE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadMicFeedHandler
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicTopoFilter, MicTopoFilterProperties);

static
PCFILTER_DESCRIPTOR MicTopoFilterDescriptor =
{
    0,                                              // Version
    &AutomationMicTopoFilter,                       // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(MicTopoMiniportPins),              // PinCount
    MicTopoMiniportPins,                            // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    0,                                              // NodeCount
    NULL,                                           // Nodes
    SIZEOF_ARRAY(MicTopoConnections),               // ConnectionCount
    MicTopoConnections,                             // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories
};

#endif // _APPSANDBOXVAD_MICTOPOFILTER_H_
