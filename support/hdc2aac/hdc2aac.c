/*
 * hdc2aac.c - losslessly remux iBiquity HDC audio frames into a standard
 *             AAC-LC ADTS stream that ordinary players (VLC, ffmpeg, ...)
 *             can decode.
 *
 * HDC (the codec used inside HD Radio) is a syntactic derivative of AAC-LC.
 * Its scale-factor sections and Huffman-coded spectral data are identical to
 * AAC; only the framing differs (element ids, field order, TNS header
 * quirks).  This tool parses an HDC frame, transplants the untouched core
 * payload into a standards-compliant AAC element, and wraps it in ADTS.
 *
 *   - No decode/re-encode happens anywhere: every coefficient bit of the
 *     core layer is copied verbatim.
 *   - The SBR extension (high-band reconstruction) present in some HDC
 *     frames is dropped in this version; the output is therefore band-
 *     limited to the ~10 kHz AAC core.  (SBR re-mapping is future work.)
 *
 * Input:  the "--dump-hdc" stream produced by nrsc5: a series of HDC
 *         packets, each prefixed with a 7-byte ADTS header (which lies
 *         about the payload being plain AAC - that is what we fix).
 * Output: an .aac ADTS file, AAC-LC, 22050 Hz, mono or stereo.
 *
 * Build:  cc -O2 -o hdc2aac hdc2aac.c
 *
 * Copyright: MIT-style; see repository LICENSE.  hcb_trees.h is derived
 * from the Huffman tables of faad2 (GPLv2) - treat the combination of
 * this directory as GPLv2 out of caution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "hcb_trees.h"

/* ------------------------------------------------------------------ */
/* constants                                                          */

#define LEN_SE_ID        3
#define LEN_TAG          4

#define ID_SCE           0
#define ID_CPE           1
#define ID_FIL           6
#define ID_END           7

#define ONLY_LONG_SEQUENCE   0
#define LONG_START_SEQUENCE  1
#define EIGHT_SHORT_SEQUENCE 2
#define LONG_STOP_SEQUENCE   3

#define ZERO_HCB       0
#define FIRST_PAIR_HCB 5
#define ESC_HCB        11
#define NOISE_HCB      13
#define INTENSITY_HCB2 14
#define INTENSITY_HCB  15

/* sample-rate index 7 = 22050 Hz (the only rate HD Radio uses) */
#define SF_INDEX        7
#define NUM_SWB_LONG    47      /* num_swb_1024_window[7] */
#define NUM_SWB_SHORT   15      /* num_swb_128_window[7]  */
#define FRAME_LEN       1024
#define SHORT_LEN       128     /* FRAME_LEN / 8 */

/* swb_offset_1024_24 / swb_offset_128_24 (faad2 specrec.c, 22050 Hz).
 * The final element is the implicit band edge (= frame length). */
static const uint16_t swb_offset_long[NUM_SWB_LONG + 1] = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68,
    76, 84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220,
    240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552, 600, 652, 704,
    768, 832, 896, 960, 1024
};

static const uint16_t swb_offset_short[NUM_SWB_SHORT + 1] = {
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128
};

/* hard limits */
#define MAX_FRAME_BYTES 4096
#define MAX_SECTIONS    60
#define MAX_SFB_LIMIT   64

/* ------------------------------------------------------------------ */
/* diagnostics                                                        */

static int verbose = 0;
static unsigned long stat_frames_ok = 0, stat_frames_bad = 0,
                     stat_sbr_dropped = 0;

static void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "hdc2aac: warning: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void info(const char *fmt, ...)
{
    va_list ap;
    if (!verbose) return;
    va_start(ap, fmt);
    fprintf(stderr, "hdc2aac: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* MSB-first bit reader over a byte buffer                            */

typedef struct {
    const uint8_t *buf;
    size_t nbytes;
    size_t pos;                 /* bit position */
} bitreader_t;

static int br_overrun(const bitreader_t *br)
{
    return br->pos > br->nbytes * 8;
}

static int br_bit(bitreader_t *br)
{
    size_t byte = br->pos >> 3;
    if (byte >= br->nbytes) {
        br->pos++;              /* keep counting so overrun is detectable */
        return 0;
    }
    return (br->buf[byte] >> (7 - (br->pos++ & 7))) & 1;
}

static uint32_t br_bits(bitreader_t *br, int n)
{
    uint32_t v = 0;
    while (n--) v = (v << 1) | (uint32_t)br_bit(br);
    return v;
}

static uint32_t br_peek(const bitreader_t *br, int n)
{
    uint32_t v = 0;
    size_t p = br->pos;
    while (n--) {
        size_t byte = p >> 3;
        int bit = 0;
        if (byte < br->nbytes)
            bit = (br->buf[byte] >> (7 - (p & 7))) & 1;
        v = (v << 1) | (uint32_t)bit;
        p++;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* bit writer                                                         */

typedef struct {
    uint8_t buf[MAX_FRAME_BYTES];
    size_t nbits;
} bitwriter_t;

static void wb_reset(bitwriter_t *bw)
{
    memset(bw->buf, 0, sizeof(bw->buf));
    bw->nbits = 0;
}

static void wb_bit(bitwriter_t *bw, int b)
{
    size_t byte;
    if (bw->nbits >= sizeof(bw->buf) * 8) {
        fprintf(stderr, "hdc2aac: internal error: output overflow\n");
        exit(1);
    }
    byte = bw->nbits >> 3;
    if (b) bw->buf[byte] |= (uint8_t)(0x80u >> (bw->nbits & 7));
    bw->nbits++;
}

static void wb_bits(bitwriter_t *bw, uint32_t v, int n)
{
    while (n--) wb_bit(bw, (int)((v >> n) & 1));
}

static void wb_align(bitwriter_t *bw)
{
    while (bw->nbits & 7) wb_bit(bw, 0);
}

/* Copy nbits starting at srcbit of src into bw (verbatim). */
static void wb_copy(bitwriter_t *bw, const bitreader_t *src,
                    size_t srcbit, size_t nbits)
{
    bitreader_t tmp = *src;
    tmp.pos = srcbit;
    while (nbits--) {
        int b = br_bit(&tmp);
        wb_bit(bw, b);
    }
}

/* ------------------------------------------------------------------ */
/* individual-channel-stream state (mirrors faad's ic_stream)          */

typedef struct {
    int window_sequence, window_shape, max_sfb, sfg;
    int num_windows, num_window_groups;
    int group_len[8];
    int num_swb;
    int sect_sfb_offset[8][MAX_SFB_LIMIT + 1];
    int num_sec[8];
    int sect_cb[8][MAX_SECTIONS];
    int sect_start[8][MAX_SECTIONS];
    int sect_end[8][MAX_SECTIONS];
    int sfb_cb[8][MAX_SFB_LIMIT + 1];
} ics_t;

/* faad2 specrec.c: window_grouping_info(), restricted to 22050 Hz */
static int window_grouping(ics_t *ics)
{
    int i, g;

    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE) {
        ics->num_windows = 8;
        ics->num_window_groups = 1;
        ics->group_len[0] = 1;
        ics->num_swb = NUM_SWB_SHORT;

        if (ics->max_sfb > ics->num_swb)
            return -1;

        for (i = 0; i < ics->num_swb; i++)
            ics->sect_sfb_offset[0][i] = swb_offset_short[i];

        /* grouping: bit (6-i) of sfg set -> same group continues */
        {
            int sfg = ics->sfg;
            for (i = 0; i < 7; i++) {
                if (!((sfg >> (6 - i)) & 1)) {
                    ics->num_window_groups++;
                    ics->group_len[ics->num_window_groups - 1] = 1;
                } else {
                    ics->group_len[ics->num_window_groups - 1]++;
                }
            }
        }

        /* sect_sfb_offset per group */
        for (g = 0; g < ics->num_window_groups; g++) {
            int width, sect_sfb = 0, offset = 0;
            for (i = 0; i < ics->num_swb; i++) {
                if (i + 1 == ics->num_swb)
                    width = SHORT_LEN - swb_offset_short[i];
                else
                    width = swb_offset_short[i + 1] - swb_offset_short[i];
                width *= ics->group_len[g];
                ics->sect_sfb_offset[g][sect_sfb++] = offset;
                offset += width;
            }
            ics->sect_sfb_offset[g][sect_sfb] = offset;
        }
    } else {
        ics->num_windows = 1;
        ics->num_window_groups = 1;
        ics->group_len[0] = 1;
        ics->num_swb = NUM_SWB_LONG;

        if (ics->max_sfb > ics->num_swb)
            return -1;

        for (i = 0; i <= ics->num_swb; i++)
            ics->sect_sfb_offset[0][i] = swb_offset_long[i];
    }
    return 0;
}

/* faad2 syntax.c: section_data() */
static int parse_section_data(bitreader_t *br, ics_t *ics)
{
    int g, sect_bits, sect_lim, esc;

    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE) {
        sect_bits = 3;
        sect_lim = 8 * 15;
    } else {
        sect_bits = 5;
        sect_lim = MAX_SFB_LIMIT - 13;   /* faad's MAX_SFB is 51 */
    }
    esc = (1 << sect_bits) - 1;

    for (g = 0; g < ics->num_window_groups; g++) {
        int k = 0, i = 0;
        while (k < ics->max_sfb) {
            int cb, len_incr, len = 0;
            if (i >= MAX_SECTIONS || i >= sect_lim)
                return -1;
            cb = (int)br_bits(br, 4);
            if (cb == 12)
                return -1;
            len_incr = (int)br_bits(br, sect_bits);
            while (len_incr == esc) {
                len += len_incr;
                if (len > sect_lim)
                    return -1;
                len_incr = (int)br_bits(br, sect_bits);
            }
            len += len_incr;
            if (len > sect_lim || k + len > ics->max_sfb)
                return -1;
            {
                int sfb;
                for (sfb = k; sfb < k + len; sfb++)
                    ics->sfb_cb[g][sfb] = cb;
            }
            ics->sect_cb[g][i] = cb;
            ics->sect_start[g][i] = k;
            ics->sect_end[g][i] = k + len;
            k += len;
            i++;
        }
        if (k != ics->max_sfb)
            return -1;
        ics->num_sec[g] = i;
    }
    return 0;
}

/* One huffman walk through a generated tree; reports how many sign bits
 * and escape sequences follow the codeword. */
static int huffman_walk(const hdc_hcb_node *tree, bitreader_t *br,
                        int *nsign, int *nesc)
{
    int idx = 0, guard = 0;
    while (!tree[idx].leaf) {
        int b = br_bit(br);
        int nxt = b ? tree[idx].b : tree[idx].a;
        if (++guard > 64)
            return -1;
        if (nxt < 0)
            return -1;
        idx = nxt;
    }
    *nsign = tree[idx].a;
    *nesc  = tree[idx].b;
    return 0;
}

/* Consume one scale-factor codeword (the sf tree's leaf payload is a
 * dummy; only the bit consumption matters here). */
static int consume_sf(bitreader_t *br)
{
    int nsign, nesc;
    return huffman_walk(hdc_cb_sf, br, &nsign, &nesc);
}

/* Consume one spectral codeword of codebook cb (incl. signs & escapes). */
static int consume_codeword(int cb, bitreader_t *br)
{
    const hdc_hcb_node *tree;
    int nsign, nesc, k;

    if (cb < 1 || cb > ESC_HCB)
        return -1;
    tree = hdc_cbs[cb];
    if (huffman_walk(tree, br, &nsign, &nesc) < 0)
        return -1;
    while (nsign--)
        br_bit(br);
    for (k = 0; k < nesc; k++) {
        /* escape: unary-coded size then that many magnitude bits
         * (faad2 huffman.c: huffman_getescape) */
        int i;
        for (i = 4; i < 16; i++)
            if (!br_bit(br))
                break;
        if (i >= 16)
            return -1;
        br_bits(br, i);
    }
    return 0;
}

/* faad2 syntax.c: decode_scale_factors() - consumption only */
static int parse_scale_factors(bitreader_t *br, ics_t *ics)
{
    int g, sfb, noise_pcm_flag = 1;

    for (g = 0; g < ics->num_window_groups; g++) {
        for (sfb = 0; sfb < ics->max_sfb; sfb++) {
            switch (ics->sfb_cb[g][sfb]) {
            case ZERO_HCB:
                break;
            case NOISE_HCB:
                /* PNS energy: first occurrence pcm(9), then huffman */
                if (noise_pcm_flag) {
                    noise_pcm_flag = 0;
                    br_bits(br, 9);
                } else if (consume_sf(br) < 0) {
                    return -1;
                }
                break;
            case INTENSITY_HCB:
            case INTENSITY_HCB2:
                /* intensity positions use the same huffman code */
                if (consume_sf(br) < 0)
                    return -1;
                break;
            default:
                if (consume_sf(br) < 0)
                    return -1;
                break;
            }
        }
    }
    return 0;
}

/* faad2 syntax.c: spectral_data() - consumption only */
static int parse_spectral_data(bitreader_t *br, ics_t *ics)
{
    int g, i, k, groups = 0;

    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->num_sec[g]; i++) {
            int sect_cb = ics->sect_cb[g][i];
            int inc = (sect_cb >= FIRST_PAIR_HCB) ? 2 : 4;

            switch (sect_cb) {
            case ZERO_HCB:
            case NOISE_HCB:
            case INTENSITY_HCB:
            case INTENSITY_HCB2:
                break;          /* no coefficients transmitted */
            default:
                for (k = ics->sect_sfb_offset[g][ics->sect_start[g][i]];
                     k < ics->sect_sfb_offset[g][ics->sect_end[g][i]];
                     k += inc) {
                    if (consume_codeword(sect_cb, br) < 0)
                        return -1;
                    if (br_overrun(br))
                        return -1;
                }
                break;
            }
        }
        groups += ics->group_len[g];
    }
    (void)groups;
    return 0;
}

/* HDC variant of tns_data(): identical to AAC except that for long
 * windows the n_filt field is not transmitted and forced to 1.
 * Records the span of bits to re-emit verbatim (excluding n_filt when
 * longskip is set). */
typedef struct {
    size_t from, to;            /* span in source bits */
} tns_span_t;

static int parse_tns_hdc(bitreader_t *br, const ics_t *ics, tns_span_t *span,
                         int *longskip)
{
    int w, n_filt_bits = 2, length_bits = 6, order_bits = 5;
    int start = (int)br->pos;

    *longskip = 0;
    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE) {
        n_filt_bits = 1;
        length_bits = 4;
        order_bits = 3;
    }

    for (w = 0; w < ics->num_windows; w++) {
        int n_filt, coef_res_bits = 0, f;
        if (ics->window_sequence != EIGHT_SHORT_SEQUENCE) {
            /* HDC: n_filt implicitly 1, no bits in stream */
            n_filt = 1;
            *longskip = 1;
        } else {
            n_filt = (int)br_bits(br, n_filt_bits);
        }
        if (n_filt > 0) {
            coef_res_bits = 1;          /* coef_res read once per window */
            br_bit(br);                 /* coef_res (copied verbatim via span) */
        }

        for (f = 0; f < n_filt; f++) {
            int order;
            br_bits(br, length_bits);           /* length */
            order = (int)br_bits(br, order_bits);
            if (order > 0) {
                int compress, coef_bits, j;
                br_bit(br);                      /* direction */
                compress = (int)br_bit(br);      /* coef compression */
                coef_bits = 3 + coef_res_bits - compress;
                for (j = 0; j < order; j++)
                    br_bits(br, coef_bits);
            }
        }
        if (br_overrun(br))
            return -1;
    }
    span->from = start;
    span->to = br->pos;
    return 0;
}

/* ------------------------------------------------------------------ */
/* frame parsing                                                      */

typedef struct {
    size_t gg_pos;              /* bit position of global_gain */
    size_t secsf_from, secsf_to;/* section_data + scale_factor_data span */
    size_t spec_from, spec_to;  /* spectral_data span */
} channel_span_t;

typedef struct {
    int stereo;
    ics_t ics;

    int ms_present;             /* ms_mask_present value (stereo only) */
    size_t ms_from, ms_len;     /* ms_used bits, verbatim */

    struct {
        int present;
        int longskip;
        tns_span_t span;
    } tns[2];

    channel_span_t ch[2];

    int has_fil;                /* SBR extension present */
} parsed_frame_t;

static int parse_channel(bitreader_t *br, ics_t *ics, channel_span_t *ch)
{
    ch->gg_pos = br->pos;
    br_bits(br, 8);                             /* global_gain */

    ch->secsf_from = br->pos;
    if (parse_section_data(br, ics) < 0)
        return -1;
    if (parse_scale_factors(br, ics) < 0)
        return -1;
    ch->secsf_to = br->pos;

    ch->spec_from = br->pos;
    if (parse_spectral_data(br, ics) < 0)
        return -1;
    ch->spec_to = br->pos;
    return 0;
}

/* Parse one HDC frame payload (as dumped by nrsc5 --dump-hdc). */
static int parse_hdc_frame(const uint8_t *buf, size_t nbytes,
                           parsed_frame_t *p)
{
    bitreader_t br = { buf, nbytes, 0 };
    int block_type, c, num_ch;

    memset(p, 0, sizeof(*p));

    block_type = (int)br_bits(&br, LEN_SE_ID);
    switch (block_type) {
    case 0: case 1: case 5: case 6:
        p->stereo = 0;
        break;
    case 2: case 7:
        p->stereo = 1;
        break;
    default:
        info("unknown HDC block type %d", block_type);
        return -1;
    }

    if (br_bit(&br) != 0) {                     /* ics_reserved_bit */
        info("reserved bit set");
        return -1;
    }
    p->ics.window_shape   = (int)br_bit(&br);
    p->ics.window_sequence = (int)br_bits(&br, 2);

    if (p->ics.window_sequence == EIGHT_SHORT_SEQUENCE) {
        p->ics.max_sfb = (int)br_bits(&br, 4);
        p->ics.sfg     = (int)br_bits(&br, 7);
    } else {
        p->ics.max_sfb = (int)br_bits(&br, 6);
    }

    if (window_grouping(&p->ics) < 0) {
        info("bad max_sfb/window grouping");
        return -1;
    }

    if (p->stereo) {
        p->ms_present = (int)br_bits(&br, 2);
        if (p->ms_present == 3) {
            info("invalid ms_mask_present");
            return -1;
        }
        if (p->ms_present == 1) {
            size_t bits = (size_t)p->ics.num_window_groups * p->ics.max_sfb;
            size_t i;
            p->ms_from = br.pos;
            p->ms_len = bits;
            for (i = 0; i < bits; i++)
                br_bit(&br);
        }
    }

    /* TNS flags and data precede the channel side information in HDC */
    num_ch = p->stereo ? 2 : 1;
    for (c = 0; c < num_ch; c++) {
        p->tns[c].present = (int)br_bit(&br);
        if (p->tns[c].present &&
            parse_tns_hdc(&br, &p->ics, &p->tns[c].span,
                          &p->tns[c].longskip) < 0) {
            info("bad TNS data");
            return -1;
        }
    }

    for (c = 0; c < num_ch; c++) {
        if (parse_channel(&br, &p->ics, &p->ch[c]) < 0) {
            info("bad channel %d data", c);
            return -1;
        }
    }

    /* optional FIL element carrying the SBR extension */
    if (!br_overrun(&br) && br_peek(&br, LEN_SE_ID) == ID_FIL) {
        br_bits(&br, LEN_SE_ID);
        if (br_bit(&br)) {                      /* SBR data follows */
            p->has_fil = 1;
            stat_sbr_dropped++;
        }
    }

    if (br_overrun(&br)) {
        info("frame overran its buffer");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* AAC emission                                                       */

static void emit_ics_info(bitwriter_t *bw, const ics_t *ics)
{
    wb_bit(bw, 0);                              /* ics_reserved_bit */
    wb_bits(bw, (uint32_t)ics->window_sequence, 2);
    wb_bit(bw, ics->window_shape);
    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE) {
        wb_bits(bw, (uint32_t)ics->max_sfb, 4);
        wb_bits(bw, (uint32_t)ics->sfg, 7);
    } else {
        wb_bits(bw, (uint32_t)ics->max_sfb, 6);
        wb_bit(bw, 0);                          /* predictor_data_present */
    }
}

/* global_gain + section/scalefactor + pulse/tns/gain + spectral.
 * When include_ics is set (mono SCE), ics_info is emitted between
 * global_gain and the sections, as required by the AAC grammar. */
static void emit_channel_body(bitwriter_t *bw, const bitreader_t *src,
                              const parsed_frame_t *p, int c,
                              int include_ics)
{
    const channel_span_t *ch = &p->ch[c];

    wb_copy(bw, src, ch->gg_pos, 8);            /* global_gain verbatim */

    if (include_ics)
        emit_ics_info(bw, &p->ics);

    /* section_data + scale_factor_data: identical grammar, copy verbatim */
    wb_copy(bw, src, ch->secsf_from, ch->secsf_to - ch->secsf_from);

    wb_bits(bw, 0, 1);                          /* pulse_data_present */
    wb_bits(bw, (uint32_t)p->tns[c].present, 1);
    if (p->tns[c].present) {
        if (p->tns[c].longskip)
            wb_bits(bw, 1, 2);                  /* n_filt=1, implicit in HDC */
        wb_copy(bw, src, p->tns[c].span.from,
                p->tns[c].span.to - p->tns[c].span.from);
    }
    wb_bits(bw, 0, 1);                          /* gain_control_present */

    /* spectral data: identical Huffman coding, copy verbatim */
    wb_copy(bw, src, ch->spec_from, ch->spec_to - ch->spec_from);
}

/* Build one raw AAC data block; returns payload size in bytes. */
static size_t emit_raw_block(bitwriter_t *bw, const bitreader_t *src,
                             const parsed_frame_t *p)
{
    wb_reset(bw);

    if (p->stereo) {
        wb_bits(bw, ID_CPE, LEN_SE_ID);
        wb_bits(bw, 0, LEN_TAG);                /* element_instance_tag */
        wb_bit(bw, 1);                          /* common_window */
        emit_ics_info(bw, &p->ics);
        wb_bits(bw, (uint32_t)p->ms_present, 2);
        if (p->ms_present == 1)
            wb_copy(bw, src, p->ms_from, p->ms_len);
        emit_channel_body(bw, src, p, 0, 0);
        emit_channel_body(bw, src, p, 1, 0);
    } else {
        wb_bits(bw, ID_SCE, LEN_SE_ID);
        wb_bits(bw, 0, LEN_TAG);
        emit_channel_body(bw, src, p, 0, 1);
    }

    wb_bits(bw, ID_END, LEN_SE_ID);
    wb_align(bw);
    return bw->nbits / 8;
}

/* Write a spec-correct 7-byte ADTS header (no CRC). */
static void write_adts(FILE *fp, unsigned len_total, int stereo)
{
    unsigned chan = stereo ? 2 : 1;
    uint8_t h[7];
    unsigned fullness = 0x7FF;
    /* sync(12)=FFF | ID(1)=0 MPEG-4 | layer(2)=00 | prot_absent(1)=1
     * profile(2)=01 LC | sf_index(4)=0111 (22050 Hz) | private(1)=0
     * chan_cfg(2) | orig(1)=0 | home(1)=0 | cr_id(1)=0 | cr_start(1)=0
     * length(13) | fullness(11)=7FF | frames(2)=01 */
    h[0] = 0xFF;
    h[1] = 0xF1;
    h[2] = (uint8_t)((1u << 6) | (SF_INDEX << 2) | ((chan >> 1) & 1));
    h[3] = (uint8_t)(((chan & 1u) << 7) | ((len_total >> 11) & 0x03));
    h[4] = (uint8_t)((len_total >> 3) & 0xFF);
    h[5] = (uint8_t)(((len_total & 0x07) << 5) | (fullness >> 6));
    h[6] = (uint8_t)(((fullness & 0x3F) << 2) | 1);
    fwrite(h, 1, sizeof(h), fp);
}

/* ------------------------------------------------------------------ */
/* input scanning & main                                              */

static uint8_t *read_file(const char *path, size_t *psize)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long size;

    if (!fp) {
        perror(path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc(size ? (size_t)size : 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (size && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        perror("read");
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *psize = (size_t)size;
    return buf;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-v] <input.hdc> <output.aac>\n"
            "\n"
            "  Transcodes an nrsc5 \"--dump-hdc\" stream into a standard\n"
            "  AAC-LC ADTS file without any decode/re-encode of the audio\n"
            "  payload.  The SBR extension (high band) is dropped in this\n"
            "  version.\n", prog);
}

int main(int argc, char **argv)
{
    const char *inpath, *outpath;
    uint8_t *buf;
    size_t size, pos = 0;
    FILE *out;
    bitwriter_t bw;
    parsed_frame_t pf;
    int last_stereo = -1;
    unsigned long total_frames = 0;

    if (argc >= 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--verbose"))) {
        verbose = 1;
        argc--;
        argv++;
    }
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }
    inpath = argv[1];
    outpath = argv[2];

    buf = read_file(inpath, &size);
    if (!buf)
        return 1;

    out = fopen(outpath, "wb");
    if (!out) {
        perror(outpath);
        free(buf);
        return 1;
    }

    while (pos + 2 < size) {
        unsigned hdr_len, frame_len;
        const uint8_t *payload;
        size_t payload_len, out_len;
        bitreader_t src;

        /* find ADTS sync */
        if (buf[pos] != 0xFF || (buf[pos + 1] & 0xF0) != 0xF0) {
            pos++;
            continue;
        }
        if (pos + 7 > size)
            break;

        hdr_len = (buf[pos + 1] & 1) ? 7 : 9;   /* CRC adds 2 bytes */
        frame_len = ((unsigned)(buf[pos + 3] & 0x03) << 11)
                  | ((unsigned)buf[pos + 4] << 3)
                  | ((unsigned)buf[pos + 5] >> 5);
        if (frame_len < hdr_len || pos + frame_len > size) {
            warn("bad frame length at offset %lu, resyncing",
                 (unsigned long)pos);
            pos++;
            continue;
        }

        payload = buf + pos + hdr_len;
        payload_len = frame_len - hdr_len;
        total_frames++;

        if (parse_hdc_frame(payload, payload_len, &pf) == 0) {
            src.buf = payload;
            src.nbytes = payload_len;
            src.pos = 0;

            if (last_stereo != -1 && last_stereo != pf.stereo)
                warn("channel count changed mid-stream (%s)",
                     pf.stereo ? "mono -> stereo" : "stereo -> mono");
            last_stereo = pf.stereo;

            out_len = emit_raw_block(&bw, &src, &pf);
            write_adts(out, (unsigned)(out_len + 7), pf.stereo);
            fwrite(bw.buf, 1, out_len, out);
            stat_frames_ok++;
            info("frame %lu: %s, win_seq=%d, max_sfb=%d%s",
                 stat_frames_ok, pf.stereo ? "stereo" : "mono",
                 pf.ics.window_sequence, pf.ics.max_sfb,
                 pf.has_fil ? " [SBR dropped]" : "");
        } else {
            /* corrupt packet (e.g. CRC failure over the air): skip it */
            warn("frame %lu at offset %lu failed to parse, skipped",
                 total_frames, (unsigned long)pos);
            stat_frames_bad++;
        }

        pos += frame_len;
    }

    fclose(out);
    free(buf);

    fprintf(stderr,
            "hdc2aac: %lu frames converted, %lu corrupt/skipped, "
            "%lu SBR extensions dropped\n",
            stat_frames_ok, stat_frames_bad, stat_sbr_dropped);

    return stat_frames_ok ? 0 : 1;
}
