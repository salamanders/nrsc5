#ifndef RECORDER_H
#define RECORDER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct song_recorder song_recorder_t;

song_recorder_t *recorder_create(const char *base_dir, double split_delay_sec, unsigned int program, double preroll_sec);
void recorder_destroy(song_recorder_t *rec, int clean_shutdown);

void recorder_on_hdc(song_recorder_t *rec, unsigned int program, const uint8_t *data, size_t len, uint32_t flags);
void recorder_on_id3(song_recorder_t *rec, unsigned int program, const char *title, const char *artist, const char *album, int xhdr_lot);
void recorder_on_lot(song_recorder_t *rec, unsigned int lot_id, const char *mime, const char *name, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RECORDER_H */
