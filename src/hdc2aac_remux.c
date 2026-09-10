/*
 * hdc2aac_remux.c - In-memory lossless remuxer from HDC frames to AAC-LC ADTS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hdc2aac_remux.h"
#include "hcb_trees.h"

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
#define NUM_SWB_LONG    47
#define NUM_SWB_SHORT   15
#define FRAME_LEN       1024
#define SHORT_LEN       128

static const uint16_t swb_offset_long[NUM_SWB_LONG + 1] = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68,
    76, 84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220,
    240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552, 600, 652, 704,
    768, 832, 896, 960, 1024
};

static const uint16_t swb_offset_short[NUM_SWB_SHORT + 1] = {
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128
};

#define MAX_FRAME_BYTES 4096
#define MAX_SECTIONS    60
#define MAX_SFB_LIMIT   64

typedef struct {
    const uint8_t *buf;
    size_t nbytes;
    size_t pos;
} bitreader_t;

static inline int br_overrun(const bitreader_t *br)
{
    return br->pos > br->nbytes * 8;
}

static inline int br_bit(bitreader_t *br)
{
    size_t byte = br->pos >> 3;
    if (byte >= br->nbytes) {
        br->pos++;
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

typedef struct {
    uint8_t buf[MAX_FRAME_BYTES];
    size_t nbits;
    int overflow;
} bitwriter_t;

static void wb_reset(bitwriter_t *bw)
{
    memset(bw->buf, 0, sizeof(bw->buf));
    bw->nbits = 0;
    bw->overflow = 0;
}

static void wb_bit(bitwriter_t *bw, int b)
{
    size_t byte;
    if (bw->nbits >= sizeof(bw->buf) * 8) {
        bw->overflow = 1;
        return;
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

static int parse_section_data(bitreader_t *br, ics_t *ics)
{
    int g, sect_bits, sect_lim, esc;

    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE) {
        sect_bits = 3;
        sect_lim = 8 * 15;
    } else {
        sect_bits = 5;
        sect_lim = MAX_SFB_LIMIT - 13;
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

static int consume_sf(bitreader_t *br)
{
    int nsign, nesc;
    return huffman_walk(hdc_cb_sf, br, &nsign, &nesc);
}

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

static int parse_scale_factors(bitreader_t *br, ics_t *ics)
{
    int g, sfb, noise_pcm_flag = 1;

    for (g = 0; g < ics->num_window_groups; g++) {
        for (sfb = 0; sfb < ics->max_sfb; sfb++) {
            switch (ics->sfb_cb[g][sfb]) {
            case ZERO_HCB:
                break;
            case NOISE_HCB:
                if (noise_pcm_flag) {
                    noise_pcm_flag = 0;
                    br_bits(br, 9);
                } else if (consume_sf(br) < 0) {
                    return -1;
                }
                break;
            case INTENSITY_HCB:
            case INTENSITY_HCB2:
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

static int parse_spectral_data(bitreader_t *br, ics_t *ics)
{
    int g, i, k;

    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->num_sec[g]; i++) {
            int sect_cb = ics->sect_cb[g][i];
            int inc = (sect_cb >= FIRST_PAIR_HCB) ? 2 : 4;

            switch (sect_cb) {
            case ZERO_HCB:
            case NOISE_HCB:
            case INTENSITY_HCB:
            case INTENSITY_HCB2:
                break;
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
    }
    return 0;
}

typedef struct {
    size_t from, to;
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
        int n_filt, coef_res = 0, f;
        if (ics->window_sequence != EIGHT_SHORT_SEQUENCE) {
            n_filt = 1;
            *longskip = 1;
        } else {
            n_filt = (int)br_bits(br, n_filt_bits);
        }
        if (n_filt > 0) {
            coef_res = br_bit(br);
        }

        for (f = 0; f < n_filt; f++) {
            int order;
            br_bits(br, length_bits);
            order = (int)br_bits(br, order_bits);
            if (order > 0) {
                int compress, coef_bits, j;
                br_bit(br);
                compress = (int)br_bit(br);
                coef_bits = 3 + coef_res - compress;
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

typedef struct {
    size_t gg_pos;
    size_t secsf_from, secsf_to;
    size_t spec_from, spec_to;
} channel_span_t;

typedef struct {
    int stereo;
    ics_t ics;
    int ms_present;
    size_t ms_from, ms_len;
    struct {
        int present;
        int longskip;
        tns_span_t span;
    } tns[2];
    channel_span_t ch[2];
    int has_fil;
} parsed_frame_t;

static int parse_channel(bitreader_t *br, ics_t *ics, channel_span_t *ch)
{
    ch->gg_pos = br->pos;
    br_bits(br, 8);

    ch->secsf_from = br->pos;
    if (parse_section_data(br, ics) < 0) return -1;
    if (parse_scale_factors(br, ics) < 0) return -1;
    ch->secsf_to = br->pos;

    ch->spec_from = br->pos;
    if (parse_spectral_data(br, ics) < 0) return -1;
    ch->spec_to = br->pos;
    return 0;
}

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
        return -1;
    }

    if (br_bit(&br) != 0) return -1;
    p->ics.window_shape   = (int)br_bit(&br);
    p->ics.window_sequence = (int)br_bits(&br, 2);

    if (p->ics.window_sequence == EIGHT_SHORT_SEQUENCE) {
        p->ics.max_sfb = (int)br_bits(&br, 4);
        p->ics.sfg     = (int)br_bits(&br, 7);
    } else {
        p->ics.max_sfb = (int)br_bits(&br, 6);
    }

    if (window_grouping(&p->ics) < 0) return -1;

    if (p->stereo) {
        p->ms_present = (int)br_bits(&br, 2);
        if (p->ms_present == 3) return -1;
        if (p->ms_present == 1) {
            size_t bits = (size_t)p->ics.num_window_groups * p->ics.max_sfb;
            size_t i;
            p->ms_from = br.pos;
            p->ms_len = bits;
            for (i = 0; i < bits; i++)
                br_bit(&br);
        }
    }

    num_ch = p->stereo ? 2 : 1;
    for (c = 0; c < num_ch; c++) {
        p->tns[c].present = (int)br_bit(&br);
        if (p->tns[c].present &&
            parse_tns_hdc(&br, &p->ics, &p->tns[c].span,
                          &p->tns[c].longskip) < 0) {
            return -1;
        }
    }

    for (c = 0; c < num_ch; c++) {
        if (parse_channel(&br, &p->ics, &p->ch[c]) < 0) {
            return -1;
        }
    }

    if (!br_overrun(&br) && br_peek(&br, LEN_SE_ID) == ID_FIL) {
        br_bits(&br, LEN_SE_ID);
        if (br_bit(&br)) {
            p->has_fil = 1;
        }
    }

    if (br_overrun(&br)) return -1;
    return 0;
}

static void emit_ics_info(bitwriter_t *bw, const ics_t *ics)
{
    wb_bit(bw, 0);
    wb_bits(bw, (uint32_t)ics->window_sequence, 2);
    wb_bit(bw, ics->window_shape);
    if (ics->window_sequence == EIGHT_SHORT_SEQUENCE) {
        wb_bits(bw, (uint32_t)ics->max_sfb, 4);
        wb_bits(bw, (uint32_t)ics->sfg, 7);
    } else {
        wb_bits(bw, (uint32_t)ics->max_sfb, 6);
        wb_bit(bw, 0);
    }
}

static void emit_channel_body(bitwriter_t *bw, const bitreader_t *src,
                              const parsed_frame_t *p, int c,
                              int include_ics)
{
    const channel_span_t *ch = &p->ch[c];

    wb_copy(bw, src, ch->gg_pos, 8);

    if (include_ics)
        emit_ics_info(bw, &p->ics);

    wb_copy(bw, src, ch->secsf_from, ch->secsf_to - ch->secsf_from);

    wb_bits(bw, 0, 1);
    wb_bits(bw, (uint32_t)p->tns[c].present, 1);
    if (p->tns[c].present) {
        if (p->tns[c].longskip)
            wb_bits(bw, 1, 2);
        wb_copy(bw, src, p->tns[c].span.from,
                p->tns[c].span.to - p->tns[c].span.from);
    }
    wb_bits(bw, 0, 1);
    wb_copy(bw, src, ch->spec_from, ch->spec_to - ch->spec_from);
}

static size_t emit_raw_block(bitwriter_t *bw, const bitreader_t *src,
                             const parsed_frame_t *p)
{
    wb_reset(bw);

    if (p->stereo) {
        wb_bits(bw, ID_CPE, LEN_SE_ID);
        wb_bits(bw, 0, LEN_TAG);
        wb_bit(bw, 1);
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
    if (bw->overflow) return 0;
    return bw->nbits / 8;
}

static void write_adts_header(uint8_t *h, unsigned len_total, int stereo)
{
    unsigned chan = stereo ? 2 : 1;
    unsigned fullness = 0x7FF;

    h[0] = 0xFF;
    h[1] = 0xF1;
    h[2] = (uint8_t)((1u << 6) | (SF_INDEX << 2) | (0 << 1) | ((chan >> 2) & 1));
    h[3] = (uint8_t)(((chan & 3u) << 6) | ((len_total >> 11) & 0x03));
    h[4] = (uint8_t)((len_total >> 3) & 0xFF);
    h[5] = (uint8_t)(((len_total & 0x07) << 5) | (fullness >> 6));
    h[6] = (uint8_t)(((fullness & 0x3F) << 2) | 0);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */

struct hdc2aac_remuxer {
    bitwriter_t bw;
    parsed_frame_t pf;
};

hdc2aac_remuxer_t *hdc2aac_remuxer_create(void)
{
    hdc2aac_remuxer_t *r = calloc(1, sizeof(*r));
    return r;
}

void hdc2aac_remuxer_destroy(hdc2aac_remuxer_t *remuxer)
{
    free(remuxer);
}

size_t hdc2aac_remux_frame(hdc2aac_remuxer_t *remuxer,
                           const uint8_t *hdc_data, size_t hdc_len,
                           uint8_t *out_buf, size_t out_buf_max)
{
    bitreader_t src;
    size_t raw_len;

    if (!remuxer || !hdc_data || hdc_len == 0 || !out_buf)
        return 0;

    if (parse_hdc_frame(hdc_data, hdc_len, &remuxer->pf) < 0)
        return 0;

    src.buf = hdc_data;
    src.nbytes = hdc_len;
    src.pos = 0;

    raw_len = emit_raw_block(&remuxer->bw, &src, &remuxer->pf);
    if (raw_len == 0 || raw_len + 7 > out_buf_max)
        return 0;

    write_adts_header(out_buf, (unsigned)(raw_len + 7), remuxer->pf.stereo);
    memcpy(out_buf + 7, remuxer->bw.buf, raw_len);

    return raw_len + 7;
}
