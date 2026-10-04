#ifndef _APPSANDBOXVAD_TOPOLOGY_H_
#define _APPSANDBOXVAD_TOPOLOGY_H_

NTSTATUS VadSpeakerFilterHandler(_In_ PPCPROPERTY_REQUEST PropertyRequest);

NTSTATUS VadSpeakerTopoHandler(_In_ PPCPROPERTY_REQUEST PropertyRequest);

NTSTATUS VadMicFilterHandler(_In_ PPCPROPERTY_REQUEST PropertyRequest);

// AppSandbox mic feed (KSPROPSETID_AsbMic) on the mic topology filter.
NTSTATUS VadMicFeedHandler(_In_ PPCPROPERTY_REQUEST PropertyRequest);

// Mic ring buffer (wavstream.cpp): fed by VadMicFeedHandler, drained by
// running capture streams.
VOID  VadMicRingWrite(_In_reads_bytes_(Length) const BYTE *Data, _In_ ULONG Length);
VOID  VadMicRingRead(_Out_writes_bytes_(Length) BYTE *Dest, _In_ ULONG Length);
VOID  VadMicRingReset(VOID);
ULONG VadMicRunningStreams(VOID);

#endif // _APPSANDBOXVAD_TOPOLOGY_H_
