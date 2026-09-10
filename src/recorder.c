/*
 * recorder.c - Stream recorder with DIRECT RECODE and automated song splitting
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

#include "recorder.h"
#include "hdc2aac_remux.h"
#include "log.h"

/* ~21.533 packets/second in HD Radio (2048 samples / 44100 Hz per frame) */
#define DEFAULT_MIN_SONG_PACKETS 1292  /* 60 seconds */
#define LOT_CACHE_SIZE 8
#define MAX_PATH_LEN 1024

typedef struct {
    unsigned int lot_id;
    char ext[8];        /* ".jpg" or ".png" */
    uint8_t *data;
    size_t size;
} lot_cache_entry_t;

struct song_recorder {
    char base_dir[MAX_PATH_LEN];
    double split_delay_sec;
    unsigned int program;
    unsigned long min_song_packets;

    hdc2aac_remuxer_t *remuxer;

    int has_seen_first_transition;
    int record_initial;
    char current_artist[256];
    char current_title[256];
    char current_album[256];
    int current_xhdr_lot;
    unsigned long current_packets;
    time_t current_start_time;

    char tmp_path[MAX_PATH_LEN * 2];
    FILE *tmp_fp;

    /* Cover art for current track */
    uint8_t *current_art_data;
    size_t current_art_size;
    char current_art_ext[8];

    /* Recent LOT image cache */
    lot_cache_entry_t lot_cache[LOT_CACHE_SIZE];
    int lot_cache_idx;

    /* Metrics */
    unsigned long total_songs_saved;
    unsigned long total_interstitials_discarded;
};

/* ------------------------------------------------------------------ */
/* Helpers: Directory creation & sanitization                         */

static int mkdir_p(const char *path)
{
    char tmp[MAX_PATH_LEN];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static void sanitize_filename(char *dst, const char *src, size_t maxlen)
{
    size_t d = 0;
    if (!src) {
        strncpy(dst, "Unknown", maxlen);
        dst[maxlen - 1] = '\0';
        return;
    }

    /* Trim leading whitespace */
    while (*src == ' ' || *src == '\t') src++;

    while (*src && d + 1 < maxlen) {
        unsigned char c = (unsigned char)*src++;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < 32) {
            dst[d++] = '_';
        } else {
            dst[d++] = (char)c;
        }
    }

    /* Trim trailing spaces and dots */
    while (d > 0 && (dst[d - 1] == ' ' || dst[d - 1] == '.' || dst[d - 1] == '\t'))
        d--;
    dst[d] = '\0';

    if (d == 0) {
        strncpy(dst, "Unknown", maxlen);
        dst[maxlen - 1] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* ID3v2.3 Tag Header Injection for .aac files                        */

static void write_id3v2_tag(FILE *fp, const char *title, const char *artist,
                            const char *album, const uint8_t *art_data,
                            size_t art_size, const char *art_ext)
{
    size_t tlen = (title && title[0]) ? strlen(title) : 0;
    size_t alen = (artist && artist[0]) ? strlen(artist) : 0;
    size_t blen = (album && album[0]) ? strlen(album) : 0;

    size_t tit2_size = 0;
    size_t tpe1_size = 0;
    size_t talb_size = 0;
    size_t apic_size = 0;

    const char *mime = "image/jpeg";
    if (art_ext && strcmp(art_ext, ".png") == 0) {
        mime = "image/png";
    }
    size_t mime_len = strlen(mime);

    size_t total_payload = 0;

    if (tlen > 0) {
        tit2_size = 1 + tlen;
        total_payload += 10 + tit2_size;
    }
    if (alen > 0) {
        tpe1_size = 1 + alen;
        total_payload += 10 + tpe1_size;
    }
    if (blen > 0) {
        talb_size = 1 + blen;
        total_payload += 10 + talb_size;
    }
    if (art_data && art_size > 0) {
        /* encoding(1) + mime(mime_len + 1) + pic_type(1) + desc_null(1) + art_size */
        apic_size = 1 + (mime_len + 1) + 1 + 1 + art_size;
        total_payload += 10 + apic_size;
    }

    if (total_payload == 0)
        return;

    /* ID3v2 10-byte header */
    uint8_t hdr[10];
    hdr[0] = 'I'; hdr[1] = 'D'; hdr[2] = '3';
    hdr[3] = 3;   /* version 2.3 */
    hdr[4] = 0;   /* revision */
    hdr[5] = 0;   /* flags */
    hdr[6] = (uint8_t)((total_payload >> 21) & 0x7F);
    hdr[7] = (uint8_t)((total_payload >> 14) & 0x7F);
    hdr[8] = (uint8_t)((total_payload >> 7) & 0x7F);
    hdr[9] = (uint8_t)(total_payload & 0x7F);
    fwrite(hdr, 1, 10, fp);

    uint8_t fhdr[10];

    /* TIT2 (Title) frame */
    if (tit2_size > 0) {
        memcpy(fhdr, "TIT2", 4);
        fhdr[4] = (uint8_t)((tit2_size >> 24) & 0xFF);
        fhdr[5] = (uint8_t)((tit2_size >> 16) & 0xFF);
        fhdr[6] = (uint8_t)((tit2_size >> 8) & 0xFF);
        fhdr[7] = (uint8_t)(tit2_size & 0xFF);
        fhdr[8] = 0; fhdr[9] = 0;
        fwrite(fhdr, 1, 10, fp);
        fputc(0, fp);
        fwrite(title, 1, tlen, fp);
    }

    /* TPE1 (Artist) frame */
    if (tpe1_size > 0) {
        memcpy(fhdr, "TPE1", 4);
        fhdr[4] = (uint8_t)((tpe1_size >> 24) & 0xFF);
        fhdr[5] = (uint8_t)((tpe1_size >> 16) & 0xFF);
        fhdr[6] = (uint8_t)((tpe1_size >> 8) & 0xFF);
        fhdr[7] = (uint8_t)(tpe1_size & 0xFF);
        fhdr[8] = 0; fhdr[9] = 0;
        fwrite(fhdr, 1, 10, fp);
        fputc(0, fp);
        fwrite(artist, 1, alen, fp);
    }

    /* TALB (Album) frame */
    if (talb_size > 0) {
        memcpy(fhdr, "TALB", 4);
        fhdr[4] = (uint8_t)((talb_size >> 24) & 0xFF);
        fhdr[5] = (uint8_t)((talb_size >> 16) & 0xFF);
        fhdr[6] = (uint8_t)((talb_size >> 8) & 0xFF);
        fhdr[7] = (uint8_t)(talb_size & 0xFF);
        fhdr[8] = 0; fhdr[9] = 0;
        fwrite(fhdr, 1, 10, fp);
        fputc(0, fp);
        fwrite(album, 1, blen, fp);
    }

    /* APIC (Attached Picture) frame */
    if (apic_size > 0) {
        memcpy(fhdr, "APIC", 4);
        fhdr[4] = (uint8_t)((apic_size >> 24) & 0xFF);
        fhdr[5] = (uint8_t)((apic_size >> 16) & 0xFF);
        fhdr[6] = (uint8_t)((apic_size >> 8) & 0xFF);
        fhdr[7] = (uint8_t)(apic_size & 0xFF);
        fhdr[8] = 0; fhdr[9] = 0;
        fwrite(fhdr, 1, 10, fp);
        fputc(0, fp);                      /* Text encoding: 0 = ISO-8859-1 */
        fwrite(mime, 1, mime_len + 1, fp); /* MIME type with null terminator */
        fputc(0x03, fp);                   /* Picture type: 0x03 = Cover (front) */
        fputc(0, fp);                      /* Description: empty string null terminator */
        fwrite(art_data, 1, art_size, fp); /* Image binary data */
    }
}

/* ------------------------------------------------------------------ */
/* Collision Resolution: [song].aac -> [song]_001.aac                 */

static void resolve_destination_paths(char *out_aac, char *out_art_stem, size_t maxlen,
                                      const char *dir, const char *title)
{
    int i;

    snprintf(out_art_stem, maxlen, "%s/%s", dir, title);
    snprintf(out_aac, maxlen, "%s.aac", out_art_stem);

    if (access(out_aac, F_OK) != 0) {
        return;
    }

    for (i = 1; i <= 999; i++) {
        snprintf(out_aac, maxlen, "%s/%s_%03d.aac", dir, title, i);
        if (access(out_aac, F_OK) != 0) {
            snprintf(out_art_stem, maxlen, "%s/%s_%03d", dir, title, i);
            return;
        }
    }

    snprintf(out_aac, maxlen, "%s/%s_%lu.aac", dir, title, (unsigned long)time(NULL));
    snprintf(out_art_stem, maxlen, "%s/%s_%lu", dir, title, (unsigned long)time(NULL));
}

/* ------------------------------------------------------------------ */
/* Song Lifecycle: Finalize & Open                                    */

static void finalize_current_song(song_recorder_t *rec)
{
    if (!rec->tmp_fp) return;

    fclose(rec->tmp_fp);
    rec->tmp_fp = NULL;

    double duration_sec = rec->current_packets * 0.0464399;
    int has_valid_metadata = (rec->current_title[0] != '\0' &&
                              rec->current_artist[0] != '\0' &&
                              strcmp(rec->current_title, "Unknown") != 0);

    if (rec->current_packets >= rec->min_song_packets && has_valid_metadata) {
        char artist_dir[MAX_PATH_LEN];
        char final_aac[MAX_PATH_LEN * 2];
        char final_art_stem[MAX_PATH_LEN * 2];
        char clean_artist[256];
        char clean_title[256];

        sanitize_filename(clean_artist, rec->current_artist, sizeof(clean_artist));
        sanitize_filename(clean_title, rec->current_title, sizeof(clean_title));

        snprintf(artist_dir, sizeof(artist_dir), "%s/%s", rec->base_dir, clean_artist);
        mkdir_p(artist_dir);

        resolve_destination_paths(final_aac, final_art_stem, sizeof(final_aac),
                                  artist_dir, clean_title);

        FILE *in_fp = fopen(rec->tmp_path, "rb");
        if (in_fp) {
            FILE *out_fp = fopen(final_aac, "wb");
            if (out_fp) {
                /* Write complete ID3v2 tag (title, artist, album, cover art) */
                write_id3v2_tag(out_fp, rec->current_title, rec->current_artist,
                                rec->current_album, rec->current_art_data,
                                rec->current_art_size, rec->current_art_ext);

                /* Copy audio data */
                char buf[65536];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), in_fp)) > 0) {
                    fwrite(buf, 1, n, out_fp);
                }
                fclose(out_fp);
                fclose(in_fp);
                unlink(rec->tmp_path);

                unsigned int mins = (unsigned int)(duration_sec / 60);
                unsigned int secs = (unsigned int)(duration_sec) % 60;
                log_info("[RECORDER] Saved: \"%s\" by \"%s\"%s%s%s (%u:%02u, %lu packets) -> %s",
                         rec->current_title, rec->current_artist,
                         rec->current_album[0] ? " (Album: \"" : "",
                         rec->current_album[0] ? rec->current_album : "",
                         rec->current_album[0] ? "\")" : "",
                         mins, secs, rec->current_packets, final_aac);
                rec->total_songs_saved++;

                /* Save companion cover art file if available */
                if (rec->current_art_data && rec->current_art_size > 0) {
                    char art_path[MAX_PATH_LEN * 4];
                    snprintf(art_path, sizeof(art_path), "%s%s", final_art_stem, rec->current_art_ext);
                    FILE *art_fp = fopen(art_path, "wb");
                    if (art_fp) {
                        fwrite(rec->current_art_data, 1, rec->current_art_size, art_fp);
                        fclose(art_fp);
                        log_info("[RECORDER] Saved cover art -> %s", art_path);
                    }
                }
            } else {
                log_error("[RECORDER] Failed to open %s for writing: %s", final_aac, strerror(errno));
                fclose(in_fp);
                unlink(rec->tmp_path);
            }
        } else {
            log_error("[RECORDER] Failed to open temp file %s: %s", rec->tmp_path, strerror(errno));
            unlink(rec->tmp_path);
        }
    } else {
        unlink(rec->tmp_path);
        log_info("[RECORDER] Discarded non-song/interstitial: \"%s - %s\" (%.1fs < %lus)",
                 rec->current_artist, rec->current_title, duration_sec,
                 (unsigned long)(rec->min_song_packets * 0.0464399));
        rec->total_interstitials_discarded++;
    }

    /* Reset track cover art */
    free(rec->current_art_data);
    rec->current_art_data = NULL;
    rec->current_art_size = 0;
    rec->current_art_ext[0] = '\0';
    rec->current_packets = 0;
}

static void start_new_song(song_recorder_t *rec, const char *title, const char *artist, const char *album, int xhdr_lot)
{
    free(rec->current_art_data);
    rec->current_art_data = NULL;
    rec->current_art_size = 0;
    rec->current_art_ext[0] = '\0';

    strncpy(rec->current_title, title ? title : "", sizeof(rec->current_title) - 1);
    rec->current_title[sizeof(rec->current_title) - 1] = '\0';

    strncpy(rec->current_artist, artist ? artist : "", sizeof(rec->current_artist) - 1);
    rec->current_artist[sizeof(rec->current_artist) - 1] = '\0';

    strncpy(rec->current_album, album ? album : "", sizeof(rec->current_album) - 1);
    rec->current_album[sizeof(rec->current_album) - 1] = '\0';

    rec->current_xhdr_lot = xhdr_lot;
    rec->current_packets = 0;
    rec->current_start_time = time(NULL);

    /* Check LOT cache for matching cover art */
    if (xhdr_lot >= 0) {
        for (int i = 0; i < LOT_CACHE_SIZE; i++) {
            if (rec->lot_cache[i].data && (int)rec->lot_cache[i].lot_id == xhdr_lot) {
                rec->current_art_data = malloc(rec->lot_cache[i].size);
                if (rec->current_art_data) {
                    memcpy(rec->current_art_data, rec->lot_cache[i].data, rec->lot_cache[i].size);
                    rec->current_art_size = rec->lot_cache[i].size;
                    strncpy(rec->current_art_ext, rec->lot_cache[i].ext, sizeof(rec->current_art_ext) - 1);
                    rec->current_art_ext[sizeof(rec->current_art_ext) - 1] = '\0';
                }
                break;
            }
        }
    }

    snprintf(rec->tmp_path, sizeof(rec->tmp_path), "%s/.tmp_recording.aac", rec->base_dir);
    rec->tmp_fp = fopen(rec->tmp_path, "wb");
    if (!rec->tmp_fp) {
        log_error("[RECORDER] Failed to create staging file %s: %s", rec->tmp_path, strerror(errno));
        return;
    }

    log_info("[RECORDER] Now recording: \"%s\" by \"%s\"%s%s%s (XHDR LOT: %d)",
             rec->current_title, rec->current_artist,
             rec->current_album[0] ? " (Album: \"" : "",
             rec->current_album[0] ? rec->current_album : "",
             rec->current_album[0] ? "\")" : "",
             rec->current_xhdr_lot);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */

song_recorder_t *recorder_create(const char *base_dir, double split_delay_sec, unsigned int program)
{
    song_recorder_t *rec = calloc(1, sizeof(*rec));
    if (!rec) return NULL;

    strncpy(rec->base_dir, base_dir, sizeof(rec->base_dir) - 1);
    rec->split_delay_sec = split_delay_sec;
    rec->program = program;

    /* Check for test override of minimum duration */
    const char *min_pkt_env = getenv("NRSC5_RECORDER_MIN_PACKETS");
    if (min_pkt_env && atoi(min_pkt_env) >= 0) {
        rec->min_song_packets = (unsigned long)atoi(min_pkt_env);
    } else {
        rec->min_song_packets = DEFAULT_MIN_SONG_PACKETS;
    }

    const char *record_init_env = getenv("NRSC5_RECORD_INITIAL");
    if (record_init_env && atoi(record_init_env) != 0) {
        rec->record_initial = 1;
    }

    mkdir_p(rec->base_dir);

    rec->remuxer = hdc2aac_remuxer_create();
    if (!rec->remuxer) {
        free(rec);
        return NULL;
    }

    log_info("[RECORDER] Initialized: target folder \"%s\", program %u, min duration %lus",
             rec->base_dir, rec->program, (unsigned long)(rec->min_song_packets * 0.0464399));

    return rec;
}

void recorder_destroy(song_recorder_t *rec, int clean_shutdown)
{
    if (!rec) return;

    if (rec->tmp_fp) {
        if (clean_shutdown) {
            finalize_current_song(rec);
        } else {
            fclose(rec->tmp_fp);
            rec->tmp_fp = NULL;
            unlink(rec->tmp_path);
            log_info("[RECORDER] Discarded incomplete track on exit");
        }
    }

    if (rec->current_art_data) {
        free(rec->current_art_data);
    }

    for (int i = 0; i < LOT_CACHE_SIZE; i++) {
        if (rec->lot_cache[i].data) {
            free(rec->lot_cache[i].data);
        }
    }

    if (rec->remuxer) {
        hdc2aac_remuxer_destroy(rec->remuxer);
    }

    log_info("[RECORDER] Shutdown complete: %lu songs saved, %lu interstitials filtered",
             rec->total_songs_saved, rec->total_interstitials_discarded);

    free(rec);
}

void recorder_on_hdc(song_recorder_t *rec, unsigned int program, const uint8_t *data, size_t len, uint32_t flags)
{
    uint8_t aac_buf[MAX_FRAME_BYTES + 7];
    size_t aac_len;

    if (!rec || rec->program != program || !data || len == 0)
        return;

    /* Skip corrupted frames */
    if (flags & 1) return;

    /* Wait until the first clean song transition */
    if (!rec->has_seen_first_transition || !rec->tmp_fp)
        return;

    aac_len = hdc2aac_remux_frame(rec->remuxer, data, len, aac_buf, sizeof(aac_buf));
    if (aac_len > 0) {
        fwrite(aac_buf, 1, aac_len, rec->tmp_fp);
        rec->current_packets++;
    }
}

void recorder_on_id3(song_recorder_t *rec, unsigned int program, const char *title, const char *artist, const char *album, int xhdr_lot)
{
    if (!rec || rec->program != program)
        return;

    const char *new_title = title ? title : "";
    const char *new_artist = artist ? artist : "";
    const char *new_album = album ? album : "";

    /* Ignore identical repeated ID3 tags, but update album / LOT ID if newly provided */
    if (rec->has_seen_first_transition &&
        strcmp(rec->current_title, new_title) == 0 &&
        strcmp(rec->current_artist, new_artist) == 0) {

        if (rec->current_album[0] == '\0' && new_album[0] != '\0') {
            strncpy(rec->current_album, new_album, sizeof(rec->current_album) - 1);
            rec->current_album[sizeof(rec->current_album) - 1] = '\0';
            log_info("[RECORDER] Updated album for \"%s\": \"%s\"", rec->current_title, rec->current_album);
        }

        if (rec->current_xhdr_lot < 0 && xhdr_lot >= 0) {
            rec->current_xhdr_lot = xhdr_lot;
            /* Check if art is already in cache */
            if (!rec->current_art_data) {
                for (int i = 0; i < LOT_CACHE_SIZE; i++) {
                    if (rec->lot_cache[i].data && (int)rec->lot_cache[i].lot_id == xhdr_lot) {
                        rec->current_art_data = malloc(rec->lot_cache[i].size);
                        if (rec->current_art_data) {
                            memcpy(rec->current_art_data, rec->lot_cache[i].data, rec->lot_cache[i].size);
                            rec->current_art_size = rec->lot_cache[i].size;
                            strncpy(rec->current_art_ext, rec->lot_cache[i].ext, sizeof(rec->current_art_ext) - 1);
                            rec->current_art_ext[sizeof(rec->current_art_ext) - 1] = '\0';
                            log_info("[RECORDER] Matched cached cover art for \"%s\" (LOT %u, %zu bytes %s)",
                                     rec->current_title, xhdr_lot, rec->current_art_size, rec->current_art_ext);
                        }
                        break;
                    }
                }
            }
        }
        return;
    }

    /* Metadata transition occurred */
    if (!rec->has_seen_first_transition) {
        rec->has_seen_first_transition = 1;
        if (rec->record_initial) {
            start_new_song(rec, new_title, new_artist, new_album, xhdr_lot);
        } else {
            strncpy(rec->current_title, new_title, sizeof(rec->current_title) - 1);
            strncpy(rec->current_artist, new_artist, sizeof(rec->current_artist) - 1);
            strncpy(rec->current_album, new_album, sizeof(rec->current_album) - 1);
            log_info("[RECORDER] In-progress track detected: \"%s\" by \"%s\" (discarding partial track until next song)",
                     rec->current_title, rec->current_artist);
        }
    } else if (!rec->tmp_fp) {
        /* First transition after discarding initial in-progress track */
        start_new_song(rec, new_title, new_artist, new_album, xhdr_lot);
    } else {
        finalize_current_song(rec);
        start_new_song(rec, new_title, new_artist, new_album, xhdr_lot);
    }
}

void recorder_on_lot(song_recorder_t *rec, unsigned int lot_id, const char *mime, const char *name, const uint8_t *data, size_t size)
{
    (void)mime;
    (void)name;
    const char *ext = ".jpg";

    if (!rec || !data || size < 4) return;

    /* Detect PNG vs JPEG */
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        ext = ".png";
    } else if (data[0] == 0xFF && data[1] == 0xD8) {
        ext = ".jpg";
    } else {
        /* Not an image */
        return;
    }

    /* Store in rotating cache */
    lot_cache_entry_t *entry = &rec->lot_cache[rec->lot_cache_idx];
    if (entry->data) free(entry->data);
    entry->data = malloc(size);
    if (entry->data) {
        memcpy(entry->data, data, size);
        entry->size = size;
        entry->lot_id = lot_id;
        strncpy(entry->ext, ext, sizeof(entry->ext));
        rec->lot_cache_idx = (rec->lot_cache_idx + 1) % LOT_CACHE_SIZE;
    }

    /* If matching currently recording song, associate immediately */
    if (rec->tmp_fp && (int)lot_id == rec->current_xhdr_lot) {
        free(rec->current_art_data);
        rec->current_art_data = malloc(size);
        if (rec->current_art_data) {
            memcpy(rec->current_art_data, data, size);
            rec->current_art_size = size;
            strncpy(rec->current_art_ext, ext, sizeof(rec->current_art_ext) - 1);
            rec->current_art_ext[sizeof(rec->current_art_ext) - 1] = '\0';
            log_info("[RECORDER] Received cover art for \"%s\" (LOT %u, %zu bytes %s)",
                     rec->current_title, lot_id, size, ext);
        }
    }
}
