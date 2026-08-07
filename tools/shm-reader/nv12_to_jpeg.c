/**
 * @file nv12_to_jpeg.c
 * @brief NV12 → JPEG converter. Zero external dependencies (built-in baseline JPEG encoder).
 *
 * Usage:
 *   nv12_to_jpeg -w 1920 -h 1080 < frame.nv12 > frame.jpg
 *   nv12_to_jpeg -w 1920 -h 1080 -i frame.nv12 -o frame.jpg
 *   nv12_to_jpeg -w 1920 -h 1080 --pipe   (continuous length-prefixed mode)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========== Zig-zag scan order ========== */
static const uint8_t zz[64] = {
     0, 1, 5, 6,14,15,27,28,
     2, 4, 7,13,16,26,29,42,
     3, 8,12,17,25,30,41,43,
     9,11,18,24,31,40,44,53,
    10,19,23,32,39,45,52,54,
    20,22,33,38,46,51,55,60,
    21,34,37,47,50,56,59,61,
    35,36,48,49,57,58,62,63
};

/* Natural-order zig-zag: for zig-zag index i, natural_zz[i] = position in 8x8 block */
static const uint8_t natural_zz[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

/* ========== Standard quantization tables (natural / row-major order) ========== */
static const uint8_t std_lum_qt[64] = {
    16,11,10,16, 24, 40, 51, 61,
    12,12,14,19, 26, 58, 60, 55,
    14,13,16,24, 40, 57, 69, 56,
    14,17,22,29, 51, 87, 80, 62,
    18,22,37,56, 68,109,103, 77,
    24,35,55,64, 81,104,113, 92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103, 99
};

static const uint8_t std_chr_qt[64] = {
    17,18,24,47,99,99,99,99,
    18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99
};

/* ========== Huffman tables ========== */
static const uint8_t dc_lum_bits[17] = {0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t dc_lum_val[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t dc_chr_bits[17] = {0,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t dc_chr_val[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t ac_lum_bits[17] = {0,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t ac_lum_val[] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,
    0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,
    0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,
    0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,
    0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,
    0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,
    0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,
    0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,
    0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,
    0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
};

static const uint8_t ac_chr_bits[17] = {0,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t ac_chr_val[] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,
    0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,
    0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,
    0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,
    0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,
    0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,
    0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,
    0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,
    0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,
    0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
};

/* ========== Huffman code entry ========== */
typedef struct { uint16_t code; uint8_t len; } HuffEntry;

/* ========== Bit writer / output buffer ========== */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    uint32_t bitbuf;
    int      bitcnt;
} BitWriter;

static void bw_init(BitWriter *w) {
    w->cap = 65536;
    w->buf = (uint8_t*)malloc(w->cap);
    w->pos = 0;
    w->bitbuf = 0;
    w->bitcnt = 0;
}

static void bw_ensure(BitWriter *w, size_t n) {
    if (w->pos + n > w->cap) {
        size_t nc = w->cap * 2;
        if (nc < w->pos + n) nc = w->pos + n + 65536;
        w->buf = (uint8_t*)realloc(w->buf, nc);
        w->cap = nc;
    }
}

static void bw_byte(BitWriter *w, uint8_t b) {
    bw_ensure(w, 1);
    w->buf[w->pos++] = b;
}

static void bw_word(BitWriter *w, uint16_t v) {
    bw_byte(w, v >> 8);
    bw_byte(w, v & 0xFF);
}

static void bw_bytes(BitWriter *w, const uint8_t *d, size_t n) {
    bw_ensure(w, n);
    memcpy(w->buf + w->pos, d, n);
    w->pos += n;
}

static void bw_put_bits(BitWriter *w, uint16_t code, int len) {
    w->bitbuf = (w->bitbuf << len) | code;
    w->bitcnt += len;
    while (w->bitcnt >= 8) {
        uint8_t b = (uint8_t)(w->bitbuf >> (w->bitcnt - 8));
        bw_byte(w, b);
        if (b == 0xFF) bw_byte(w, 0x00);
        w->bitcnt -= 8;
    }
}

static void bw_flush_bits(BitWriter *w) {
    if (w->bitcnt > 0) {
        bw_put_bits(w, 0x7F, 7);
        w->bitcnt = 0;
    }
}

/* ========== Build Huffman lookup table ========== */
static void build_hufftable(HuffEntry *ht, const uint8_t *bits, const uint8_t *vals) {
    memset(ht, 0, 256 * sizeof(HuffEntry));
    uint16_t code = 0;
    int k = 0;
    for (int i = 1; i <= 16; i++) {
        for (int j = 0; j < bits[i]; j++) {
            ht[vals[k]].code = code;
            ht[vals[k]].len = i;
            k++;
            code++;
        }
        code <<= 1;
    }
}

/* ========== Scale quantization table by quality ========== */
static void scale_qt(uint8_t *dst, const uint8_t *src, int quality) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int s = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (int i = 0; i < 64; i++) {
        int v = (src[i] * s + 50) / 100;
        if (v < 1) v = 1;
        if (v > 255) v = 255;
        dst[i] = (uint8_t)v;  /* natural order (row-major), same as src */
    }
}

/* ========== Float DCT (AAN butterfly, same as stb_image_write) ========== */
static void fdct(float d[64]) {
    int i;
    /* Rows */
    for (i = 0; i < 8; i++) {
        float *v = d + i * 8;
        float a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3];
        float a4 = v[4], a5 = v[5], a6 = v[6], a7 = v[7];

        float s07 = a0 + a7, d07 = a0 - a7;
        float s16 = a1 + a6, d16 = a1 - a6;
        float s25 = a2 + a5, d25 = a2 - a5;
        float s34 = a3 + a4, d34 = a3 - a4;

        /* Even */
        float e0 = s07 + s34, e1 = s16 + s25;
        float e2 = s07 - s34, e3 = s16 - s25;
        v[0] = e0 + e1;
        v[4] = e0 - e1;
        float z1 = (e2 + e3) * 0.707106781f;
        v[2] = e2 + z1;
        v[6] = e2 - z1;

        /* Odd */
        float t10 = d34 + d25, t11 = d25 + d16, t12 = d16 + d07;
        float z5 = (t10 - t12) * 0.382683433f;
        float z2 = t10 * 0.541196100f + z5;
        float z4 = t12 * 1.306562965f + z5;
        float z3 = t11 * 0.707106781f;
        float z11 = d07 + z3, z13 = d07 - z3;
        v[5] = z13 + z2;
        v[3] = z13 - z2;
        v[1] = z11 + z4;
        v[7] = z11 - z4;
    }
    /* Columns */
    for (i = 0; i < 8; i++) {
        float a0 = d[i],      a1 = d[8+i],  a2 = d[16+i], a3 = d[24+i];
        float a4 = d[32+i],   a5 = d[40+i], a6 = d[48+i], a7 = d[56+i];

        float s07 = a0 + a7, d07 = a0 - a7;
        float s16 = a1 + a6, d16 = a1 - a6;
        float s25 = a2 + a5, d25 = a2 - a5;
        float s34 = a3 + a4, d34 = a3 - a4;

        float e0 = s07 + s34, e1 = s16 + s25;
        float e2 = s07 - s34, e3 = s16 - s25;
        d[i]    = e0 + e1;
        d[32+i] = e0 - e1;
        float z1 = (e2 + e3) * 0.707106781f;
        d[16+i] = e2 + z1;
        d[48+i] = e2 - z1;

        float t10 = d34 + d25, t11 = d25 + d16, t12 = d16 + d07;
        float z5 = (t10 - t12) * 0.382683433f;
        float z2 = t10 * 0.541196100f + z5;
        float z4 = t12 * 1.306562965f + z5;
        float z3 = t11 * 0.707106781f;
        float z11 = d07 + z3, z13 = d07 - z3;
        d[40+i] = z13 + z2;
        d[24+i] = z13 - z2;
        d[8+i]  = z11 + z4;
        d[56+i] = z11 - z4;
    }
}

/*
 * AAN post-scale factors.
 * The AAN butterfly DCT output at position (row, col) is scaled by
 *   aanscale[row] * aanscale[col]
 * relative to the true DCT. We fold this into the quantization divisor.
 */
static const float aanscale[8] = {
    1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
    1.0f, 0.785694958f, 0.541196100f, 0.275899379f
};

/* Build combined fdtbl: 1.0 / (qt[i] * aanscale[row]*aanscale[col] * 8.0) */
static void build_fdtbl(float *fdtbl, const uint8_t *qt) {
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int i = r * 8 + c;
            fdtbl[i] = 1.0f / ((float)qt[i] * aanscale[r] * aanscale[c] * 8.0f);
        }
    }
}

/* ========== Encode one 8x8 block ========== */
static int encode_block(BitWriter *w, float fblock[64], const float *fdtbl,
                        int prev_dc, const HuffEntry *dc_ht, const HuffEntry *ac_ht) {
    /* DCT */
    fdct(fblock);

    /* Quantize + zig-zag reorder */
    int coefs[64];
    for (int i = 0; i < 64; i++) {
        /* natural_zz[i] = natural-order position for zig-zag index i */
        int ni = natural_zz[i];
        float v = fblock[ni] * fdtbl[ni];
        coefs[i] = (int)(v + (v < 0 ? -0.5f : 0.5f));  /* round to nearest */
    }

    /* DC coefficient (difference coded) */
    int dc = coefs[0] - prev_dc;
    int adc = dc < 0 ? -dc : dc;
    int nbits = 0, tmp = adc;
    while (tmp) { nbits++; tmp >>= 1; }
    bw_put_bits(w, dc_ht[nbits].code, dc_ht[nbits].len);
    if (nbits) {
        int val = dc >= 0 ? dc : (dc - 1);
        bw_put_bits(w, (uint16_t)(val & ((1 << nbits) - 1)), nbits);
    }

    /* AC coefficients */
    int nzeros = 0;
    for (int i = 1; i < 64; i++) {
        int ac = coefs[i];
        if (ac == 0) {
            nzeros++;
            continue;
        }
        while (nzeros >= 16) {
            bw_put_bits(w, ac_ht[0xF0].code, ac_ht[0xF0].len);
            nzeros -= 16;
        }
        int aac = ac < 0 ? -ac : ac;
        nbits = 0; tmp = aac;
        while (tmp) { nbits++; tmp >>= 1; }
        uint8_t sym = (uint8_t)((nzeros << 4) | nbits);
        bw_put_bits(w, ac_ht[sym].code, ac_ht[sym].len);
        int val = ac >= 0 ? ac : (ac - 1);
        bw_put_bits(w, (uint16_t)(val & ((1 << nbits) - 1)), nbits);
        nzeros = 0;
    }
    if (nzeros > 0) {
        bw_put_bits(w, ac_ht[0].code, ac_ht[0].len);  /* EOB */
    }

    return coefs[0];  /* return this DC for next block's difference */
}

/* ========== Write JPEG headers ========== */

static void write_dht(BitWriter *w, uint8_t cls_id,
                      const uint8_t *bits, const uint8_t *vals) {
    int count = 0;
    for (int i = 1; i <= 16; i++) count += bits[i];
    bw_word(w, 0xFFC4);
    bw_word(w, 3 + 16 + count);
    bw_byte(w, cls_id);
    for (int i = 1; i <= 16; i++) bw_byte(w, bits[i]);
    for (int i = 0; i < count; i++) bw_byte(w, vals[i]);
}

static void write_headers(BitWriter *w, int width, int height,
                          const uint8_t *lum_qt, const uint8_t *chr_qt) {
    /* SOI */
    bw_word(w, 0xFFD8);

    /* APP0 JFIF */
    bw_word(w, 0xFFE0);
    bw_word(w, 16);
    bw_bytes(w, (const uint8_t*)"JFIF\0", 5);
    bw_word(w, 0x0101);
    bw_byte(w, 0);
    bw_word(w, 1);
    bw_word(w, 1);
    bw_byte(w, 0);
    bw_byte(w, 0);

    /* DQT luminance (table 0, zig-zag order in marker) */
    bw_word(w, 0xFFDB);
    bw_word(w, 67);
    bw_byte(w, 0);
    for (int i = 0; i < 64; i++)
        bw_byte(w, lum_qt[natural_zz[i]]);  /* read natural-order qt in zig-zag sequence */

    /* DQT chrominance (table 1) */
    bw_word(w, 0xFFDB);
    bw_word(w, 67);
    bw_byte(w, 1);
    for (int i = 0; i < 64; i++)
        bw_byte(w, chr_qt[natural_zz[i]]);

    /* SOF0 baseline YCbCr 4:2:0 */
    bw_word(w, 0xFFC0);
    bw_word(w, 17);
    bw_byte(w, 8);         /* precision */
    bw_word(w, height);
    bw_word(w, width);
    bw_byte(w, 3);         /* 3 components */
    bw_byte(w, 1); bw_byte(w, 0x22); bw_byte(w, 0);   /* Y:  2x2 subsampling, qt0 */
    bw_byte(w, 2); bw_byte(w, 0x11); bw_byte(w, 1);   /* Cb: 1x1, qt1 */
    bw_byte(w, 3); bw_byte(w, 0x11); bw_byte(w, 1);   /* Cr: 1x1, qt1 */

    /* DHT */
    write_dht(w, 0x00, dc_lum_bits, dc_lum_val);
    write_dht(w, 0x10, ac_lum_bits, ac_lum_val);
    write_dht(w, 0x01, dc_chr_bits, dc_chr_val);
    write_dht(w, 0x11, ac_chr_bits, ac_chr_val);

    /* SOS */
    bw_word(w, 0xFFDA);
    bw_word(w, 12);
    bw_byte(w, 3);
    bw_byte(w, 1); bw_byte(w, 0x00);   /* Y:  DC=0, AC=0 */
    bw_byte(w, 2); bw_byte(w, 0x11);   /* Cb: DC=1, AC=1 */
    bw_byte(w, 3); bw_byte(w, 0x11);   /* Cr: DC=1, AC=1 */
    bw_byte(w, 0);                      /* Ss */
    bw_byte(w, 63);                     /* Se */
    bw_byte(w, 0);                      /* Ah/Al */
}

/* ========== Main encode function ========== */

void nv12_encode_jpeg(const uint8_t *nv12, int w, int h, int quality,
                      uint8_t **out_buf, size_t *out_len) {
    uint8_t lum_qt[64], chr_qt[64];
    float fdtbl_y[64], fdtbl_c[64];
    HuffEntry dc_lum_ht[256], dc_chr_ht[256], ac_lum_ht[256], ac_chr_ht[256];

    scale_qt(lum_qt, std_lum_qt, quality);
    scale_qt(chr_qt, std_chr_qt, quality);
    build_fdtbl(fdtbl_y, lum_qt);
    build_fdtbl(fdtbl_c, chr_qt);
    build_hufftable(dc_lum_ht, dc_lum_bits, dc_lum_val);
    build_hufftable(dc_chr_ht, dc_chr_bits, dc_chr_val);
    build_hufftable(ac_lum_ht, ac_lum_bits, ac_lum_val);
    build_hufftable(ac_chr_ht, ac_chr_bits, ac_chr_val);

    BitWriter bw;
    bw_init(&bw);
    write_headers(&bw, w, h, lum_qt, chr_qt);

    const uint8_t *Y  = nv12;
    const uint8_t *UV = nv12 + w * h;

    int prev_dc_y = 0, prev_dc_cb = 0, prev_dc_cr = 0;
    int mcu_w = (w + 15) / 16;
    int mcu_h = (h + 15) / 16;

    float fblock[64];

    for (int my = 0; my < mcu_h; my++) {
        for (int mx = 0; mx < mcu_w; mx++) {

            /* 4 Y blocks (2x2 arrangement in the 16x16 MCU) */
            for (int by = 0; by < 2; by++) {
                for (int bx = 0; bx < 2; bx++) {
                    for (int r = 0; r < 8; r++) {
                        for (int c = 0; c < 8; c++) {
                            int py = my * 16 + by * 8 + r;
                            int px = mx * 16 + bx * 8 + c;
                            if (py >= h) py = h - 1;
                            if (px >= w) px = w - 1;
                            fblock[r * 8 + c] = (float)Y[py * w + px] - 128.0f;
                        }
                    }
                    prev_dc_y = encode_block(&bw, fblock, fdtbl_y,
                                              prev_dc_y, dc_lum_ht, ac_lum_ht);
                }
            }

            /* Cb block (from NV12 interleaved UV plane) */
            for (int r = 0; r < 8; r++) {
                for (int c = 0; c < 8; c++) {
                    int py = my * 8 + r;
                    int px = mx * 8 + c;
                    if (py >= h / 2) py = h / 2 - 1;
                    if (px >= w / 2) px = w / 2 - 1;
                    fblock[r * 8 + c] = (float)UV[py * w + px * 2] - 128.0f;
                }
            }
            prev_dc_cb = encode_block(&bw, fblock, fdtbl_c,
                                       prev_dc_cb, dc_chr_ht, ac_chr_ht);

            /* Cr block */
            for (int r = 0; r < 8; r++) {
                for (int c = 0; c < 8; c++) {
                    int py = my * 8 + r;
                    int px = mx * 8 + c;
                    if (py >= h / 2) py = h / 2 - 1;
                    if (px >= w / 2) px = w / 2 - 1;
                    fblock[r * 8 + c] = (float)UV[py * w + px * 2 + 1] - 128.0f;
                }
            }
            prev_dc_cr = encode_block(&bw, fblock, fdtbl_c,
                                       prev_dc_cr, dc_chr_ht, ac_chr_ht);
        }
    }

    bw_flush_bits(&bw);
    bw_word(&bw, 0xFFD9);  /* EOI */

    *out_buf = bw.buf;
    *out_len = bw.pos;
}

/* ========== CLI ========== */

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -w WIDTH -h HEIGHT [options]\n", prog);
    fprintf(stderr, "  -w WIDTH     Frame width (must be even)\n");
    fprintf(stderr, "  -h HEIGHT    Frame height (must be even)\n");
    fprintf(stderr, "  -q QUALITY   JPEG quality 1-100 (default: 85)\n");
    fprintf(stderr, "  -i FILE      Input NV12 (default: stdin)\n");
    fprintf(stderr, "  -o FILE      Output JPEG (default: stdout)\n");
    fprintf(stderr, "  --pipe       Continuous mode: 4-byte LE length prefix per JPEG\n");
}

static size_t read_full(FILE *f, uint8_t *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        size_t r = fread(buf + total, 1, n - total, f);
        if (r == 0) break;
        total += r;
    }
    return total;
}

int main(int argc, char *argv[]) {
    int width = 0, height = 0, quality = 85, pipe_mode = 0;
    const char *in_path = NULL, *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            width = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            height = atoi(argv[++i]);
        else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc)
            quality = atoi(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            in_path = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out_path = argv[++i];
        else if (strcmp(argv[i], "--pipe") == 0)
            pipe_mode = 1;
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        }
    }

    if (width <= 0 || height <= 0 || width % 2 || height % 2) {
        fprintf(stderr, "Error: width/height must be positive even numbers\n");
        usage(argv[0]); return 1;
    }

    size_t frame_size = (size_t)width * height * 3 / 2;
    uint8_t *frame = (uint8_t*)malloc(frame_size);
    if (!frame) { perror("malloc"); return 1; }

    FILE *fin = in_path ? fopen(in_path, "rb") : stdin;
    FILE *fout = (out_path && !pipe_mode) ? fopen(out_path, "wb") : stdout;
    if (!fin) { perror(in_path); return 1; }
    if (!fout) { perror(out_path); return 1; }

    if (pipe_mode) {
        while (1) {
            if (read_full(fin, frame, frame_size) < frame_size) break;
            uint8_t *jpeg = NULL;
            size_t jpeg_len = 0;
            nv12_encode_jpeg(frame, width, height, quality, &jpeg, &jpeg_len);
            uint32_t len32 = (uint32_t)jpeg_len;
            fwrite(&len32, 4, 1, fout);
            fwrite(jpeg, 1, jpeg_len, fout);
            fflush(fout);
            free(jpeg);
        }
    } else {
        size_t n = read_full(fin, frame, frame_size);
        if (n < frame_size)
            fprintf(stderr, "Warning: short read %zu/%zu\n", n, frame_size);
        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        nv12_encode_jpeg(frame, width, height, quality, &jpeg, &jpeg_len);
        fwrite(jpeg, 1, jpeg_len, fout);
        free(jpeg);
        fprintf(stderr, "Encoded %dx%d -> %zu bytes (q=%d)\n",
                width, height, jpeg_len, quality);
    }

    free(frame);
    if (in_path) fclose(fin);
    if (out_path && !pipe_mode) fclose(fout);
    return 0;
}
