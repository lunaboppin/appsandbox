#ifndef VM_WEB_ENCODER_H
#define VM_WEB_ENCODER_H

#include <windows.h>

/*
 * H.264 for browser access, through Media Foundation: the host GPU's encoder if it
 * has one (NVENC, AMF and Quick Sync all appear as hardware transforms), otherwise
 * Windows' software encoder. Takes NV12 in system memory; hands out Annex B access
 * units with no frame reordering.
 */

typedef struct WebEncoder WebEncoder;
typedef void (*WebEncoderOutput)(void *user, const BYTE *data, DWORD size, BOOL key_frame);

WebEncoder *web_encoder_create(UINT width, UINT height, UINT fps, UINT bits_per_second,
                               WebEncoderOutput output, void *user);
void web_encoder_destroy(WebEncoder *e);

/* TRUE when the frames are encoded by a GPU. */
BOOL web_encoder_is_hardware(const WebEncoder *e);

/* Collect finished frames and see whether a new one can go in. Must be called often
   from the one thread that uses the encoder. Returns FALSE if the encoder failed. */
BOOL web_encoder_pump(WebEncoder *e);
BOOL web_encoder_ready(const WebEncoder *e);

/* nv12 is width * height * 3 / 2 bytes. Returns FALSE if the encoder failed. */
BOOL web_encoder_submit(WebEncoder *e, const BYTE *nv12);

void web_encoder_force_key(WebEncoder *e);
void web_encoder_set_bitrate(WebEncoder *e, UINT bits_per_second);

#endif /* VM_WEB_ENCODER_H */
