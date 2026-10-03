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
#include <pthread.h>
#include <dirent.h>

#include "recorder.h"
#include "hdc2aac_remux.h"
#include "log.h"

/* ~21.533 packets/second in HD Radio (2048 samples / 44100 Hz per frame) */
#define DEFAULT_MIN_SONG_PACKETS 1292  /* 60 seconds */
#define LOT_CACHE_SIZE 8
#define MAX_PATH_LEN 1024
#define DEFAULT_PREROLL_SEC 15.0
#define DEFAULT_POSTROLL_SEC 15.0

typedef struct {
    unsigned int lot_id;
    char ext[8];        /* ".jpg" or ".png" */
    uint8_t *data;
    size_t size;
} lot_cache_entry_t;

typedef struct {
    uint8_t data[MAX_FRAME_BYTES + 7];
    size_t len;
} preroll_frame_t;

typedef struct active_track {
    FILE *tmp_fp;
    char tmp_path[MAX_PATH_LEN * 2];
    char title[256];
    char artist[256];
    char album[256];
    int xhdr_lot;
    uint8_t *art_data;
    size_t art_size;
    char art_ext[8];

    unsigned long packets_preroll;
    unsigned long packets_body;
    unsigned long packets_postroll;

    int is_postroll;
    size_t postroll_frames_remaining;

    struct active_track *next;
} active_track_t;

struct song_recorder {
    char base_dir[MAX_PATH_LEN];
    double split_delay_sec;
    double preroll_sec;
    double postroll_sec;
    unsigned int program;
    unsigned long min_song_packets;

    hdc2aac_remuxer_t *remuxer;

    /* Continuous pre-roll ring buffer */
    preroll_frame_t *preroll;
    size_t preroll_head;
    size_t preroll_count;
    size_t preroll_capacity;

    int has_seen_first_transition;
    int record_initial;

    /* Active tracks linked list */
    active_track_t *tracks;

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
/* Raw Broadcast Destination Path: [song]_001.m4a -> [song]_002.m4a    */
/* (The unsuffixed [song].m4a is reserved for trimmed master tracks)  */

static void resolve_destination_paths(char *out_path, size_t maxlen,
                                      const char *dir, const char *title)
{
    int i;

    /* Raw broadcast captures always start at _001.m4a.
     * The plain unsuffixed [title].m4a name is reserved exclusively for
     * the finalized, high-confidence trimmed master track. */
    for (i = 1; i <= 999; i++) {
        snprintf(out_path, maxlen, "%s/%s_%03d.m4a", dir, title, i);
        if (access(out_path, F_OK) != 0) {
            return;
        }
    }

    snprintf(out_path, maxlen, "%s/%s_%lu.m4a", dir, title, (unsigned long)time(NULL));
}

/* ------------------------------------------------------------------ */
/* Track Lookup Helpers                                               */

static active_track_t *get_primary_track(song_recorder_t *rec)
{
    active_track_t *t = rec->tracks;
    while (t) {
        if (!t->is_postroll) return t;
        t = t->next;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Background Worker for Non-Blocking Remuxing                       */

typedef struct {
    char input_aac_path[MAX_PATH_LEN * 2];
    char tmp_art_path[MAX_PATH_LEN * 4];
    int has_art;
    char final_audio_path[MAX_PATH_LEN * 4];
    char title[256];
    char artist[256];
    char album[256];
    double duration_sec;
    unsigned long packets;
    double offset_front_sec;
    double offset_end_sec;
    double meta_duration_sec;
    unsigned long packets_preroll;
    unsigned long packets_body;
    unsigned long packets_postroll;
    char base_dir[MAX_PATH_LEN];
    char clean_artist[256];
    char clean_title[256];
} remux_job_t;

static void *remux_worker_thread(void *arg)
{
    remux_job_t *job = (remux_job_t *)arg;
    if (!job) return NULL;

    char meta_title[512], meta_artist[512], meta_album[512];
    char meta_comment[256], meta_off_front[64], meta_off_end[64];
    char *argv[48];
    int argc = 0;

    argv[argc++] = "ffmpeg";
    argv[argc++] = "-y";
    argv[argc++] = "-v"; argv[argc++] = "error";
    argv[argc++] = "-i"; argv[argc++] = job->input_aac_path;
    if (job->has_art) {
        argv[argc++] = "-i"; argv[argc++] = job->tmp_art_path;
        argv[argc++] = "-map"; argv[argc++] = "0:a";
        argv[argc++] = "-map"; argv[argc++] = "1:v";
        argv[argc++] = "-c:a"; argv[argc++] = "copy";
        argv[argc++] = "-c:v"; argv[argc++] = "copy";
        argv[argc++] = "-disposition:v:0"; argv[argc++] = "attached_pic";
    } else {
        argv[argc++] = "-c:a"; argv[argc++] = "copy";
    }
    if (job->title[0]) {
        snprintf(meta_title, sizeof(meta_title), "title=%s", job->title);
        argv[argc++] = "-metadata"; argv[argc++] = meta_title;
    }
    if (job->artist[0]) {
        snprintf(meta_artist, sizeof(meta_artist), "artist=%s", job->artist);
        argv[argc++] = "-metadata"; argv[argc++] = meta_artist;
    }
    if (job->album[0]) {
        snprintf(meta_album, sizeof(meta_album), "album=%s", job->album);
        argv[argc++] = "-metadata"; argv[argc++] = meta_album;
    }

    /* Embedded boundary offsets inside M4A metadata */
    snprintf(meta_comment, sizeof(meta_comment),
             "comment=offset_front=%.3f;offset_end=%.3f;meta_duration=%.3f;total_duration=%.3f",
             job->offset_front_sec, job->offset_end_sec,
             job->meta_duration_sec, job->duration_sec);
    argv[argc++] = "-metadata"; argv[argc++] = meta_comment;

    snprintf(meta_off_front, sizeof(meta_off_front), "nrsc5_offset_front=%.3f", job->offset_front_sec);
    argv[argc++] = "-metadata"; argv[argc++] = meta_off_front;

    snprintf(meta_off_end, sizeof(meta_off_end), "nrsc5_offset_end=%.3f", job->offset_end_sec);
    argv[argc++] = "-metadata"; argv[argc++] = meta_off_end;

    argv[argc++] = job->final_audio_path;
    argv[argc] = NULL;

    /* Losslessly remux to M4A without blocking the main SDR loop */
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

    /* Verify size and auto-unlink on failure or broken stub */
    struct stat st;
    int is_valid = (remux_ok && stat(job->final_audio_path, &st) == 0 && st.st_size > 1024);

    if (!is_valid) {
        unlink(job->final_audio_path);
        log_error("[RECORDER] Failed to finalize \"%s\": ffmpeg remux error or empty output (unlinked)", job->title);
    } else {
        /* Write companion info text file alongside the M4A */
        char txt_path[MAX_PATH_LEN * 4];
        strncpy(txt_path, job->final_audio_path, sizeof(txt_path) - 1);
        txt_path[sizeof(txt_path) - 1] = '\0';
        char *dot = strrchr(txt_path, '.');
        if (dot) strcpy(dot, ".txt");
        else strncat(txt_path, ".txt", sizeof(txt_path) - strlen(txt_path) - 1);

        FILE *txt_fp = fopen(txt_path, "w");
        if (txt_fp) {
            fprintf(txt_fp, "title: %s\n", job->title);
            fprintf(txt_fp, "artist: %s\n", job->artist);
            fprintf(txt_fp, "album: %s\n", job->album[0] ? job->album : "");
            fprintf(txt_fp, "offset_front_sec: %.3f\n", job->offset_front_sec);
            fprintf(txt_fp, "offset_end_sec: %.3f\n", job->offset_end_sec);
            fprintf(txt_fp, "meta_duration_sec: %.3f\n", job->meta_duration_sec);
            fprintf(txt_fp, "total_duration_sec: %.3f\n", job->duration_sec);
            fprintf(txt_fp, "preroll_packets: %lu\n", job->packets_preroll);
            fprintf(txt_fp, "song_packets: %lu\n", job->packets_body);
            fprintf(txt_fp, "postroll_packets: %lu\n", job->packets_postroll);
            fprintf(txt_fp, "total_packets: %lu\n", job->packets);
            fclose(txt_fp);
        }

        unsigned int mins = (unsigned int)(job->duration_sec / 60);
        unsigned int secs = (unsigned int)(job->duration_sec) % 60;
        log_info("[RECORDER] Saved: \"%s\" by \"%s\"%s%s%s (%u:%02u, %lu packets%s, offsets: front=%.2fs, end=%.2fs) -> %s",
                 job->title, job->artist,
                 job->album[0] ? " (Album: \"" : "",
                 job->album[0] ? job->album : "",
                 job->album[0] ? "\")" : "",
                 mins, secs, job->packets,
                 job->has_art ? ", embedded art" : "",
                 job->offset_front_sec, job->offset_end_sec,
                 job->final_audio_path);

        /* Automatically check if enough copies exist to mint a high-confidence trimmed master */
        pid_t tpid = fork();
        if (tpid == 0) {
            char *trim_argv[] = {
                "trimmer.py",
                "--apply",
                "--recordings-dir", job->base_dir,
                "--artist", job->clean_artist,
                "--title", job->clean_title,
                NULL
            };
            execv("/home/benjamin/nrsc5/scripts/trimmer.py", trim_argv);
            execvp("trimmer.py", trim_argv);
            _exit(127);
        } else if (tpid > 0) {
            int tstatus = 0;
            waitpid(tpid, &tstatus, 0);
        }
    }

    /* Clean up temporary staging files */
    if (job->has_art) {
        unlink(job->tmp_art_path);
    }
    unlink(job->input_aac_path);

    free(job);
    return NULL;
}

static unsigned long g_song_seq = 0;

/* ------------------------------------------------------------------ */
/* Song Lifecycle: Finalize & Open                                    */

static void finalize_track(song_recorder_t *rec, active_track_t *track)
{
    if (!track->tmp_fp) return;

    fclose(track->tmp_fp);
    track->tmp_fp = NULL;

    unsigned long total_packets = track->packets_preroll + track->packets_body + track->packets_postroll;
    double duration_sec = total_packets * 0.0464399;
    double offset_front_sec = track->packets_preroll * 0.0464399;
    double offset_end_sec = track->packets_postroll * 0.0464399;
    double meta_duration_sec = track->packets_body * 0.0464399;

    int has_valid_metadata = (track->title[0] != '\0' &&
                              track->artist[0] != '\0' &&
                              strcmp(track->title, "Unknown") != 0);

    if (total_packets >= rec->min_song_packets && has_valid_metadata) {
        char artist_dir[MAX_PATH_LEN * 2];
        char final_audio[MAX_PATH_LEN * 4];
        char clean_artist[256];
        char clean_title[256];

        sanitize_filename(clean_artist, track->artist, sizeof(clean_artist));
        sanitize_filename(clean_title, track->title, sizeof(clean_title));

        snprintf(artist_dir, sizeof(artist_dir), "%s/%s", rec->base_dir, clean_artist);
        mkdir_p(artist_dir);

        resolve_destination_paths(final_audio, sizeof(final_audio),
                                  artist_dir, clean_title);

        /* Stage cover art for ffmpeg embedding if available */
        char tmp_art_path[MAX_PATH_LEN * 4];
        int has_art = 0;
        if (track->art_data && track->art_size > 0) {
            snprintf(tmp_art_path, sizeof(tmp_art_path), "%s/.tmp_art_%d_%lu_%lu%s",
                     artist_dir, (int)getpid(), (unsigned long)time(NULL), ++g_song_seq, track->art_ext);
            FILE *art_fp = fopen(tmp_art_path, "wb");
            if (art_fp) {
                fwrite(track->art_data, 1, track->art_size, art_fp);
                fclose(art_fp);
                has_art = 1;
            }
        }

        /* Rename active temporary recording to an isolated staging file for background worker */
        char staging_aac[MAX_PATH_LEN * 2];
        snprintf(staging_aac, sizeof(staging_aac), "%s/.staging_%d_%lu_%lu.aac",
                 rec->base_dir, (int)getpid(), (unsigned long)time(NULL), ++g_song_seq);
        if (rename(track->tmp_path, staging_aac) != 0) {
            snprintf(staging_aac, sizeof(staging_aac), "%s", track->tmp_path);
        }

        remux_job_t *job = calloc(1, sizeof(*job));
        if (job) {
            snprintf(job->input_aac_path, sizeof(job->input_aac_path), "%s", staging_aac);
            snprintf(job->final_audio_path, sizeof(job->final_audio_path), "%s", final_audio);
            job->has_art = has_art;
            if (has_art) {
                snprintf(job->tmp_art_path, sizeof(job->tmp_art_path), "%s", tmp_art_path);
            }
            snprintf(job->title, sizeof(job->title), "%s", track->title);
            snprintf(job->artist, sizeof(job->artist), "%s", track->artist);
            snprintf(job->album, sizeof(job->album), "%s", track->album);
            job->duration_sec = duration_sec;
            job->packets = total_packets;
            job->offset_front_sec = offset_front_sec;
            job->offset_end_sec = offset_end_sec;
            job->meta_duration_sec = meta_duration_sec;
            job->packets_preroll = track->packets_preroll;
            job->packets_body = track->packets_body;
            job->packets_postroll = track->packets_postroll;
            snprintf(job->base_dir, sizeof(job->base_dir), "%s", rec->base_dir);
            snprintf(job->clean_artist, sizeof(job->clean_artist), "%s", clean_artist);
            snprintf(job->clean_title, sizeof(job->clean_title), "%s", clean_title);

            /* Launch detached background packaging worker */
            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            if (pthread_create(&tid, &attr, remux_worker_thread, job) != 0) {
                log_error("[RECORDER] Failed to spawn background remux thread for \"%s\", executing synchronously", track->title);
                remux_worker_thread(job);
            }
            pthread_attr_destroy(&attr);
            rec->total_songs_saved++;
        } else {
            log_error("[RECORDER] Out of memory allocating remux job for \"%s\"", track->title);
            if (has_art) unlink(tmp_art_path);
            unlink(staging_aac);
        }
    } else {
        unlink(track->tmp_path);
        log_info("[RECORDER] Discarded non-song/interstitial: \"%s - %s\" (%.1fs < %lus)",
                 track->artist, track->title, duration_sec,
                 (unsigned long)(rec->min_song_packets * 0.0464399));
        rec->total_interstitials_discarded++;
    }

    if (track->art_data) {
        free(track->art_data);
        track->art_data = NULL;
        track->art_size = 0;
    }
}

static void start_new_song(song_recorder_t *rec, const char *title, const char *artist, const char *album, int xhdr_lot)
{
    /* If an active non-postroll track is running, transition it to post-roll */
    active_track_t *cur = get_primary_track(rec);
    if (cur) {
        if (rec->postroll_sec > 0.0) {
            cur->is_postroll = 1;
            cur->postroll_frames_remaining = (size_t)(rec->postroll_sec * 21.533 + 0.5);
            log_info("[RECORDER] Track \"%s\" entered post-roll (%.2fs, %zu frames)",
                     cur->title, rec->postroll_sec, cur->postroll_frames_remaining);
        } else {
            finalize_track(rec, cur);
        }
    }

    /* Allocate new active track */
    active_track_t *new_track = calloc(1, sizeof(active_track_t));
    if (!new_track) {
        log_error("[RECORDER] Failed to allocate active track for \"%s\"", title ? title : "");
        return;
    }

    strncpy(new_track->title, title ? title : "", sizeof(new_track->title) - 1);
    strncpy(new_track->artist, artist ? artist : "", sizeof(new_track->artist) - 1);
    strncpy(new_track->album, album ? album : "", sizeof(new_track->album) - 1);
    new_track->xhdr_lot = xhdr_lot;

    /* Check LOT cache for matching cover art */
    if (xhdr_lot >= 0) {
        for (int i = 0; i < LOT_CACHE_SIZE; i++) {
            if (rec->lot_cache[i].data && (int)rec->lot_cache[i].lot_id == xhdr_lot) {
                new_track->art_data = malloc(rec->lot_cache[i].size);
                if (new_track->art_data) {
                    memcpy(new_track->art_data, rec->lot_cache[i].data, rec->lot_cache[i].size);
                    new_track->art_size = rec->lot_cache[i].size;
                    strncpy(new_track->art_ext, rec->lot_cache[i].ext, sizeof(new_track->art_ext) - 1);
                }
                break;
            }
        }
    }

    snprintf(new_track->tmp_path, sizeof(new_track->tmp_path), "%s/.tmp_rec_%d_%lu_%lu.aac",
             rec->base_dir, (int)getpid(), (unsigned long)time(NULL), ++g_song_seq);
    new_track->tmp_fp = fopen(new_track->tmp_path, "wb");
    if (!new_track->tmp_fp) {
        log_error("[RECORDER] Failed to create staging file %s: %s", new_track->tmp_path, strerror(errno));
        if (new_track->art_data) free(new_track->art_data);
        free(new_track);
        return;
    }

    /* Prepend continuous pre-roll buffer */
    if (rec->preroll && rec->preroll_count > 0 && rec->preroll_capacity > 0) {
        size_t start_idx = (rec->preroll_count < rec->preroll_capacity) ? 0 : rec->preroll_head;
        for (size_t i = 0; i < rec->preroll_count; i++) {
            size_t idx = (start_idx + i) % rec->preroll_capacity;
            preroll_frame_t *frame = &rec->preroll[idx];
            if (frame->len > 0) {
                fwrite(frame->data, 1, frame->len, new_track->tmp_fp);
                new_track->packets_preroll++;
            }
        }
        log_info("[RECORDER] Prepended %lu pre-roll frames (%.2fs) to \"%s\"",
                 new_track->packets_preroll, new_track->packets_preroll * 0.0464399, new_track->title);
    }

    /* Insert at head of tracks list */
    new_track->next = rec->tracks;
    rec->tracks = new_track;

    log_info("[RECORDER] Now recording: \"%s\" by \"%s\"%s%s%s (XHDR LOT: %d)",
             new_track->title, new_track->artist,
             new_track->album[0] ? " (Album: \"" : "",
             new_track->album[0] ? new_track->album : "",
             new_track->album[0] ? "\")" : "",
             new_track->xhdr_lot);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */

song_recorder_t *recorder_create(const char *base_dir, double split_delay_sec, unsigned int program, double preroll_sec, double postroll_sec)
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

    rec->preroll_sec = (preroll_sec < 0.0) ? DEFAULT_PREROLL_SEC : preroll_sec;
    const char *preroll_env = getenv("NRSC5_RECORDER_PREROLL");
    if (preroll_env) {
        rec->preroll_sec = atof(preroll_env);
    }
    if (rec->preroll_sec < 0.0) rec->preroll_sec = 0.0;

    rec->postroll_sec = (postroll_sec < 0.0) ? DEFAULT_POSTROLL_SEC : postroll_sec;
    const char *postroll_env = getenv("NRSC5_RECORDER_POSTROLL");
    if (postroll_env) {
        rec->postroll_sec = atof(postroll_env);
    }
    if (rec->postroll_sec < 0.0) rec->postroll_sec = 0.0;

    /* ~21.533 frames per second (2048 samples / 44100 Hz per frame) */
    rec->preroll_capacity = (size_t)(rec->preroll_sec * 21.533 + 0.5);
    if (rec->preroll_capacity > 0) {
        rec->preroll = calloc(rec->preroll_capacity, sizeof(preroll_frame_t));
    }
    rec->preroll_head = 0;
    rec->preroll_count = 0;

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

    /* Clean up any dangling temporary or staging files from prior unclean shutdowns */
    DIR *d = opendir(rec->base_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, ".tmp_", 5) == 0 || strncmp(de->d_name, ".staging_", 9) == 0) {
                char orphan[MAX_PATH_LEN * 2];
                snprintf(orphan, sizeof(orphan), "%s/%s", rec->base_dir, de->d_name);
                unlink(orphan);
            }
        }
        closedir(d);
    }

    rec->remuxer = hdc2aac_remuxer_create();
    if (!rec->remuxer) {
        if (rec->preroll) free(rec->preroll);
        free(rec);
        return NULL;
    }

    log_info("[RECORDER] Initialized: target folder \"%s\", program %u, min duration %lus, pre-roll %.2fs (%zu frames), post-roll %.2fs",
             rec->base_dir, rec->program, (unsigned long)(rec->min_song_packets * 0.0464399),
             rec->preroll_sec, rec->preroll_capacity, rec->postroll_sec);

    return rec;
}

void recorder_destroy(song_recorder_t *rec, int clean_shutdown)
{
    if (!rec) return;

    active_track_t *t = rec->tracks;
    while (t) {
        active_track_t *next = t->next;
        if (t->tmp_fp) {
            if (clean_shutdown) {
                finalize_track(rec, t);
            } else {
                fclose(t->tmp_fp);
                t->tmp_fp = NULL;
                unlink(t->tmp_path);
                log_info("[RECORDER] Discarded incomplete track on exit: \"%s\"", t->title);
                if (t->art_data) free(t->art_data);
            }
        }
        free(t);
        t = next;
    }
    rec->tracks = NULL;

    if (rec->preroll) {
        free(rec->preroll);
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

    aac_len = hdc2aac_remux_frame(rec->remuxer, data, len, aac_buf, sizeof(aac_buf));
    if (aac_len == 0)
        return;

    /* Feed frame to all currently active tracks */
    active_track_t **curr_ptr = &rec->tracks;
    while (*curr_ptr) {
        active_track_t *track = *curr_ptr;
        if (track->tmp_fp) {
            fwrite(aac_buf, 1, aac_len, track->tmp_fp);
            if (track->is_postroll) {
                track->packets_postroll++;
                if (track->postroll_frames_remaining > 0) {
                    track->postroll_frames_remaining--;
                }
                if (track->postroll_frames_remaining == 0) {
                    /* Finalize track and unlink from active list */
                    finalize_track(rec, track);
                    *curr_ptr = track->next;
                    free(track);
                    continue;
                }
            } else {
                track->packets_body++;
            }
        }
        curr_ptr = &(*curr_ptr)->next;
    }

    /* Store frame into rolling pre-roll ring buffer */
    if (rec->preroll && rec->preroll_capacity > 0) {
        preroll_frame_t *slot = &rec->preroll[rec->preroll_head];
        memcpy(slot->data, aac_buf, aac_len);
        slot->len = aac_len;
        rec->preroll_head = (rec->preroll_head + 1) % rec->preroll_capacity;
        if (rec->preroll_count < rec->preroll_capacity) {
            rec->preroll_count++;
        }
    }
}

void recorder_on_id3(song_recorder_t *rec, unsigned int program, const char *title, const char *artist, const char *album, int xhdr_lot)
{
    if (!rec || rec->program != program)
        return;

    const char *new_title = title ? title : "";
    const char *new_artist = artist ? artist : "";
    const char *new_album = album ? album : "";

    active_track_t *cur = get_primary_track(rec);

    /* Ignore identical repeated ID3 tags, but update album / LOT ID if newly provided */
    if (rec->has_seen_first_transition && cur &&
        strcmp(cur->title, new_title) == 0 &&
        strcmp(cur->artist, new_artist) == 0) {

        if (cur->album[0] == '\0' && new_album[0] != '\0') {
            strncpy(cur->album, new_album, sizeof(cur->album) - 1);
            cur->album[sizeof(cur->album) - 1] = '\0';
            log_info("[RECORDER] Updated album for \"%s\": \"%s\"", cur->title, cur->album);
        }

        if (cur->xhdr_lot < 0 && xhdr_lot >= 0) {
            cur->xhdr_lot = xhdr_lot;
            /* Check if art is already in cache */
            if (!cur->art_data) {
                for (int i = 0; i < LOT_CACHE_SIZE; i++) {
                    if (rec->lot_cache[i].data && (int)rec->lot_cache[i].lot_id == xhdr_lot) {
                        cur->art_data = malloc(rec->lot_cache[i].size);
                        if (cur->art_data) {
                            memcpy(cur->art_data, rec->lot_cache[i].data, rec->lot_cache[i].size);
                            cur->art_size = rec->lot_cache[i].size;
                            strncpy(cur->art_ext, rec->lot_cache[i].ext, sizeof(cur->art_ext) - 1);
                            cur->art_ext[sizeof(cur->art_ext) - 1] = '\0';
                            log_info("[RECORDER] Matched cached cover art for \"%s\" (LOT %u, %zu bytes %s)",
                                     cur->title, xhdr_lot, cur->art_size, cur->art_ext);
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
            log_info("[RECORDER] In-progress track detected: \"%s\" by \"%s\" (discarding partial track until next song)",
                     new_title, new_artist);
        }
    } else {
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

    /* If matching any active track, associate immediately */
    for (active_track_t *t = rec->tracks; t; t = t->next) {
        if (t->tmp_fp && (int)lot_id == t->xhdr_lot && !t->art_data) {
            t->art_data = malloc(size);
            if (t->art_data) {
                memcpy(t->art_data, data, size);
                t->art_size = size;
                strncpy(t->art_ext, ext, sizeof(t->art_ext) - 1);
                t->art_ext[sizeof(t->art_ext) - 1] = '\0';
                log_info("[RECORDER] Received cover art for \"%s\" (LOT %u, %zu bytes %s)",
                         t->title, lot_id, size, ext);
            }
        }
    }
}
