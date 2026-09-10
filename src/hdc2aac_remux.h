#ifndef HDC2AAC_REMUX_H
#define HDC2AAC_REMUX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_FRAME_BYTES 4096

typedef struct hdc2aac_remuxer hdc2aac_remuxer_t;

hdc2aac_remuxer_t *hdc2aac_remuxer_create(void);
void hdc2aac_remuxer_destroy(hdc2aac_remuxer_t *remuxer);

/*
 * Remux an HDC packet (raw HDC payload) into a standard AAC-LC ADTS frame.
 *
 * Parameters:
 *   remuxer: pointer created by hdc2aac_remuxer_create()
 *   hdc_data: pointer to raw HDC frame bytes (from NRSC5_EVENT_HDC)
 *   hdc_len: length of hdc_data in bytes
 *   out_buf: buffer to receive the 7-byte ADTS header + AAC payload
 *   out_buf_max: capacity of out_buf (recommended >= 4096 bytes)
 *
 * Returns:
 *   Total bytes written to out_buf (ADTS header + AAC payload), or 0 on error.
 */
size_t hdc2aac_remux_frame(hdc2aac_remuxer_t *remuxer,
                           const uint8_t *hdc_data, size_t hdc_len,
                           uint8_t *out_buf, size_t out_buf_max);

#ifdef __cplusplus
}
#endif

#endif /* HDC2AAC_REMUX_H */
