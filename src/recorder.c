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
#include <sys/wait.h>
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

    /* Trim leading whitespace and separators */
    while (*src == ' ' || *src == '\t' || *src == '_' || *src == '.') src++;

    while (*src && d + 1 < maxlen) {
        unsigned char c = (unsigned char)*src++;
        /* Allow only alphanumeric and hyphens as-is; convert spaces, quotes, and all special characters to underscore */
        int is_safe = ((c >= 'a' && c <= 'z') ||
                       (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') ||
                       c == '-');

        if (is_safe) {
            dst[d++] = (char)c;
        } else {
            /* Avoid consecutive underscores */
            if (d > 0 && dst[d - 1] != '_') {
                dst[d++] = '_';
            }
        }
    }

    /* Trim trailing underscores, spaces and dots */
    while (d > 0 && (dst[d - 1] == ' ' || dst[d - 1] == '.' || dst[d - 1] == '\t' || dst[d - 1] == '_'))
        d--;
    dst[d] = '\0';

    if (d == 0) {
        strncpy(dst, "Unknown", maxlen);
        dst[maxlen - 1] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Collision Resolution: [song].m4a -> [song]_001.m4a                 */

static void resolve_destination_paths(char *out_path, size_t maxlen,
                                      const char *dir, const char *title)
{
    int i;

    snprintf(out_path, maxlen, "%s/%s.m4a", dir, title);

    if (access(out_path, F_OK) != 0) {
        return;
    }

    for (i = 1; i <= 999; i++) {
        snprintf(out_path, maxlen, "%s/%s_%03d.m4a", dir, title, i);
        if (access(out_path, F_OK) != 0) {
            return;
        }
    }

    snprintf(out_path, maxlen, "%s/%s_%lu.m4a", dir, title, (unsigned long)time(NULL));
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
        char artist_dir[MAX_PATH_LEN * 2];
        char final_audio[MAX_PATH_LEN * 4];
        char clean_artist[256];
        char clean_title[256];

        sanitize_filename(clean_artist, rec->current_artist, sizeof(clean_artist));
        sanitize_filename(clean_title, rec->current_title, sizeof(clean_title));

        snprintf(artist_dir, sizeof(artist_dir), "%s/%s", rec->base_dir, clean_artist);
        mkdir_p(artist_dir);

        resolve_destination_paths(final_audio, sizeof(final_audio),
                                  artist_dir, clean_title);

        /* Stage cover art for ffmpeg embedding if available */
        char tmp_art_path[MAX_PATH_LEN * 4];
        int has_art = 0;
        if (rec->current_art_data && rec->current_art_size > 0) {
            snprintf(tmp_art_path, sizeof(tmp_art_path), "%s/.tmp_art_%lu%s",
                     artist_dir, (unsigned long)time(NULL), rec->current_art_ext);
            FILE *art_fp = fopen(tmp_art_path, "wb");
            if (art_fp) {
                fwrite(rec->current_art_data, 1, rec->current_art_size, art_fp);
                fclose(art_fp);
                has_art = 1;
            }
        }

        /* Build ffmpeg command with in-memory metadata and cover art */
        char meta_title[512], meta_artist[512], meta_album[512];
        char *argv[32];
        int argc = 0;
        argv[argc++] = "ffmpeg";
        argv[argc++] = "-y";
        argv[argc++] = "-v"; argv[argc++] = "error";
        argv[argc++] = "-i"; argv[argc++] = rec->tmp_path;
        if (has_art) {
            argv[argc++] = "-i"; argv[argc++] = tmp_art_path;
            argv[argc++] = "-map"; argv[argc++] = "0:a";
            argv[argc++] = "-map"; argv[argc++] = "1:v";
            argv[argc++] = "-c:a"; argv[argc++] = "copy";
            argv[argc++] = "-c:v"; argv[argc++] = "copy";
            argv[argc++] = "-disposition:v:0"; argv[argc++] = "attached_pic";
        } else {
            argv[argc++] = "-c:a"; argv[argc++] = "copy";
        }
        if (rec->current_title[0]) {
            snprintf(meta_title, sizeof(meta_title), "title=%s", rec->current_title);
            argv[argc++] = "-metadata"; argv[argc++] = meta_title;
        }
        if (rec->current_artist[0]) {
            snprintf(meta_artist, sizeof(meta_artist), "artist=%s", rec->current_artist);
            argv[argc++] = "-metadata"; argv[argc++] = meta_artist;
        }
        if (rec->current_album[0]) {
            snprintf(meta_album, sizeof(meta_album), "album=%s", rec->current_album);
            argv[argc++] = "-metadata"; argv[argc++] = meta_album;
        }
        argv[argc++] = final_audio;
        argv[argc] = NULL;

        /* Losslessly remux to M4A for universal player / VLC tag support */
        int remux_ok = 0;
        pid_t pid = fork();
        if (pid == 0) {
            execvp("ffmpeg", argv);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                remux_ok = 1;
            }
        }

        /* Clean up temporary staging files */
        if (has_art) {
            unlink(tmp_art_path);
        }
        unlink(rec->tmp_path);

        if (!remux_ok) {
            log_error("[RECORDER] Failed to finalize \"%s\": ffmpeg remux error", rec->current_title);
        } else {
            unsigned int mins = (unsigned int)(duration_sec / 60);
            unsigned int secs = (unsigned int)(duration_sec) % 60;
            log_info("[RECORDER] Saved: \"%s\" by \"%s\"%s%s%s (%u:%02u, %lu packets%s) -> %s",
                     rec->current_title, rec->current_artist,
                     rec->current_album[0] ? " (Album: \"" : "",
                     rec->current_album[0] ? rec->current_album : "",
                     rec->current_album[0] ? "\")" : "",
                     mins, secs, rec->current_packets,
                     has_art ? ", embedded art" : "",
                     final_audio);
            rec->total_songs_saved++;
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
    /* Require ffmpeg for M4A packaging */
    if (system("ffmpeg -version > /dev/null 2>&1") != 0) {
        log_error("[RECORDER] ffmpeg is required for stream recording but was not found in PATH");
        exit(1);
    }

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
