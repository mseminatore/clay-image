//------------------------------------------------------------------------
// anim.c -- palettes, GIF and APNG for the image brick
//
//  Plain C, no Clay API: image.c wraps these for Clay.
//
//  Palettes: the frames' own colours when there are few enough (exact), else
//  a palette learnt by NeuQuant, a Kohonen self-organising map over RGB
//  (Anthony Dekker, "Kohonen neural networks for optimal colour
//  quantization", Network: Computation in Neural Systems 5, 1994). This is
//  our own implementation of the algorithm the paper describes. After
//  learning, the palette entries nearest the most used colours are set to
//  exactly those colours, so backgrounds, text and lines stay pure.
//
//  GIF: GIF89a with one global palette for every frame (no flicker between
//  frames), LZW compressed. Each frame after the first is only the
//  rectangle that changed, with unchanged pixels in it transparent, which
//  keeps animations of plots small.
//
//  APNG: each frame is encoded as a PNG by stb_image_write (filters and
//  deflate), and its image data re-wrapped as an APNG frame. Full colour.
//------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// from stb_image_write (implemented in image.c)
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels, ptrdiff_t stride_bytes, int x, int y, int n, int *out_len);

//------------------------------------------------------------------------
// a growing byte buffer
//------------------------------------------------------------------------
typedef struct {
    unsigned char *data;
    size_t len, cap;
    int failed;
} Buf;

static void buf_put(Buf *b, const void *src, size_t n) {
    if (b->failed)
        return;

    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;

        while (cap < b->len + n)
            cap *= 2;

        unsigned char *p = (unsigned char *)realloc(b->data, cap);

        if (!p) {
            b->failed = 1;
            return;
        }

        b->data = p;
        b->cap = cap;
    }

    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void buf_byte(Buf *b, int v) {
    unsigned char c = (unsigned char)v;
    buf_put(b, &c, 1);
}

static void buf_le16(Buf *b, int v) {
    buf_byte(b, v & 255);
    buf_byte(b, (v >> 8) & 255);
}

static void buf_be32(Buf *b, unsigned long v) {
    buf_byte(b, (int)((v >> 24) & 255));
    buf_byte(b, (int)((v >> 16) & 255));
    buf_byte(b, (int)((v >> 8) & 255));
    buf_byte(b, (int)(v & 255));
}

//------------------------------------------------------------------------
// pixels: frames are 1 (gray), 2 (gray, alpha), 3 (RGB) or 4 (RGBA) bytes
// a pixel. A pixel with alpha below 128 is transparent.
//------------------------------------------------------------------------
static void pixel_rgb(const unsigned char *px, int c, unsigned char *rgb, int *opaque) {
    if (c >= 3) {
        rgb[0] = px[0];
        rgb[1] = px[1];
        rgb[2] = px[2];
    } else {
        rgb[0] = rgb[1] = rgb[2] = px[0];
    }

    *opaque = (c == 2) ? px[1] >= 128 : (c == 4 ? px[3] >= 128 : 1);
}

//------------------------------------------------------------------------
// colour counts: an open-addressed table of 24-bit colours. It stops taking
// new colours when full (the counts of those it has stay right).
//------------------------------------------------------------------------
#define COUNT_BITS 18
#define COUNT_SIZE (1 << COUNT_BITS)

typedef struct {
    unsigned int *key;      // colour + 1 (0: empty)
    unsigned int *count;
    int distinct;
    int full;
} Counts;

static int counts_init(Counts *t) {
    t->key = (unsigned int *)calloc(COUNT_SIZE, sizeof(unsigned int));
    t->count = (unsigned int *)calloc(COUNT_SIZE, sizeof(unsigned int));
    t->distinct = 0;
    t->full = 0;
    return t->key && t->count;
}

static void counts_free(Counts *t) {
    free(t->key);
    free(t->count);
}

static void counts_add(Counts *t, unsigned int rgb) {
    unsigned int k = rgb + 1;
    unsigned int h = (rgb * 2654435761u) >> (32 - COUNT_BITS);

    for (int probe = 0; probe < 64; probe++) {
        unsigned int i = (h + (unsigned int)probe) & (COUNT_SIZE - 1);

        if (t->key[i] == k) {
            t->count[i]++;
            return;
        }

        if (t->key[i] == 0) {
            if (t->distinct >= COUNT_SIZE / 2) {
                t->full = 1;
                return;
            }

            t->key[i] = k;
            t->count[i] = 1;
            t->distinct++;
            return;
        }
    }

    t->full = 1;
}

//------------------------------------------------------------------------
// NeuQuant: a one-dimensional self-organising map of n neurons over (b, g,
// r), trained on a sample of the pixels. Integer arithmetic, with the
// constants of Dekker's paper.
//------------------------------------------------------------------------
#define NQ_CYCLES       100
#define NQ_NETBIAS      4                   // colour values are << 4 in the net
#define NQ_INTBIASSHIFT 16
#define NQ_INTBIAS      (1 << NQ_INTBIASSHIFT)
#define NQ_GAMMASHIFT   10
#define NQ_BETASHIFT    10
#define NQ_BETA         (NQ_INTBIAS >> NQ_BETASHIFT)
#define NQ_BETAGAMMA    (NQ_INTBIAS << (NQ_GAMMASHIFT - NQ_BETASHIFT))
#define NQ_RADBIASSHIFT 6
#define NQ_RADIUSBIAS   (1 << NQ_RADBIASSHIFT)
#define NQ_RADIUSDEC    30
#define NQ_ALPHASHIFT   10
#define NQ_INITALPHA    (1 << NQ_ALPHASHIFT)
#define NQ_RADBSHIFT    8
#define NQ_RADBIAS      (1 << NQ_RADBSHIFT)
#define NQ_ALPHARADBSHIFT (NQ_ALPHASHIFT + NQ_RADBSHIFT)
#define NQ_ALPHARADBIAS (1 << NQ_ALPHARADBSHIFT)

typedef struct {
    int n;
    int net[256][3];        // b, g, r << NQ_NETBIAS
    int bias[256];
    int freq[256];
    int radpower[256 >> 3];
} NeuQuant;

// The neuron nearest (b, g, r) once its bias is counted (so neurons that win
// too often lose their edge), and update every neuron's frequency and bias
static int nq_contest(NeuQuant *q, int b, int g, int r) {
    int bestd = 0x7fffffff, bestbiasd = 0x7fffffff;
    int bestpos = 0, bestbiaspos = 0;

    for (int i = 0; i < q->n; i++) {
        int *p = q->net[i];
        int d = abs(p[0] - b) + abs(p[1] - g) + abs(p[2] - r);

        if (d < bestd) {
            bestd = d;
            bestpos = i;
        }

        int biasd = d - (q->bias[i] >> (NQ_INTBIASSHIFT - NQ_NETBIAS));

        if (biasd < bestbiasd) {
            bestbiasd = biasd;
            bestbiaspos = i;
        }

        int betafreq = q->freq[i] >> NQ_BETASHIFT;
        q->freq[i] -= betafreq;
        q->bias[i] += betafreq << NQ_GAMMASHIFT;
    }

    q->freq[bestpos] += NQ_BETA;
    q->bias[bestpos] -= NQ_BETAGAMMA;
    return bestbiaspos;
}

// Move neuron i towards (b, g, r) by alpha / NQ_INITALPHA
static void nq_alter_single(NeuQuant *q, int alpha, int i, int b, int g, int r) {
    int *p = q->net[i];
    p[0] -= (alpha * (p[0] - b)) / NQ_INITALPHA;
    p[1] -= (alpha * (p[1] - g)) / NQ_INITALPHA;
    p[2] -= (alpha * (p[2] - r)) / NQ_INITALPHA;
}

// Move the neurons within rad of i towards (b, g, r), less the further they are
static void nq_alter_neighbours(NeuQuant *q, int rad, int i, int b, int g, int r) {
    int lo = i - rad, hi = i + rad;

    if (lo < -1) lo = -1;
    if (hi > q->n) hi = q->n;

    int j = i + 1, k = i - 1, m = 1;

    while (j < hi || k > lo) {
        int a = q->radpower[m++];

        if (j < hi) {
            int *p = q->net[j++];
            p[0] -= (a * (p[0] - b)) / NQ_ALPHARADBIAS;
            p[1] -= (a * (p[1] - g)) / NQ_ALPHARADBIAS;
            p[2] -= (a * (p[2] - r)) / NQ_ALPHARADBIAS;
        }

        if (k > lo) {
            int *p = q->net[k--];
            p[0] -= (a * (p[0] - b)) / NQ_ALPHARADBIAS;
            p[1] -= (a * (p[1] - g)) / NQ_ALPHARADBIAS;
            p[2] -= (a * (p[2] - r)) / NQ_ALPHARADBIAS;
        }
    }
}

static void nq_radpower(NeuQuant *q, int rad, int alpha) {
    for (int i = 0; i < rad; i++)
        q->radpower[i] = alpha * (((rad * rad - i * i) * NQ_RADBIAS) / (rad * rad));
}

// Learn n colours (2..256) from rgb, a list of count (r, g, b) triples, looking
// at about one in samplefac of them (1: all; 10 is a good balance); the
// palette is written to pal as r, g, b
static void neuquant(const unsigned char *rgb, size_t count, int n, int samplefac, unsigned char *pal) {
    NeuQuant *q = (NeuQuant *)calloc(1, sizeof(NeuQuant));

    if (!q) {
        // a grey ramp, should memory run out
        for (int i = 0; i < n; i++)
            pal[3 * i] = pal[3 * i + 1] = pal[3 * i + 2] = (unsigned char)(i * 255 / (n > 1 ? n - 1 : 1));
        return;
    }

    q->n = n;

    for (int i = 0; i < n; i++) {
        int v = (i << (NQ_NETBIAS + 8)) / n;
        q->net[i][0] = q->net[i][1] = q->net[i][2] = v;
        q->freq[i] = NQ_INTBIAS / n;
        q->bias[i] = 0;
    }

    if (samplefac < 1) samplefac = 1;
    if (samplefac > 30) samplefac = 30;

    int alphadec = 30 + ((samplefac - 1) / 3);
    size_t samples = count / (size_t)samplefac;

    // small pictures need several passes for the map to settle: learn from
    // at least 100,000 samples (about 30 ms)
    if (samples < 100000)
        samples = 100000;

    size_t delta = samples / NQ_CYCLES;

    if (delta < 1)
        delta = 1;

    int alpha = NQ_INITALPHA;
    int radius = (n >> 3) * NQ_RADIUSBIAS;
    int rad = radius >> NQ_RADBIASSHIFT;

    if (rad <= 1)
        rad = 0;

    nq_radpower(q, rad, alpha);

    // step through the pixels by a prime that doesn't divide their number, so
    // the sample spreads over the whole picture
    size_t step;

    if (count % 499) step = 499;
    else if (count % 491) step = 491;
    else if (count % 487) step = 487;
    else step = 503;

    size_t pos = 0;

    for (size_t i = 0; i < samples; ) {
        const unsigned char *p = rgb + 3 * pos;
        int b = p[2] << NQ_NETBIAS, g = p[1] << NQ_NETBIAS, r = p[0] << NQ_NETBIAS;
        int j = nq_contest(q, b, g, r);

        nq_alter_single(q, alpha, j, b, g, r);

        if (rad)
            nq_alter_neighbours(q, rad, j, b, g, r);

        pos = (pos + step) % count;
        i++;

        if (i % delta == 0) {
            alpha -= alpha / alphadec;
            radius -= radius / NQ_RADIUSDEC;
            rad = radius >> NQ_RADBIASSHIFT;

            if (rad <= 1)
                rad = 0;

            nq_radpower(q, rad, alpha);
        }
    }

    for (int i = 0; i < n; i++) {
        for (int ch = 0; ch < 3; ch++) {
            int v = (q->net[i][ch] + (1 << (NQ_NETBIAS - 1))) >> NQ_NETBIAS;
            v = v < 0 ? 0 : (v > 255 ? 255 : v);
            pal[3 * i + (2 - ch)] = (unsigned char)v;   // net is b, g, r
        }
    }

    free(q);
}

//------------------------------------------------------------------------
// a palette and a fast pixel -> index map
//------------------------------------------------------------------------
typedef struct {
    unsigned char rgb[256 * 3];
    int n;                  // colours in use
    int transparent;        // their index for transparent pixels, or -1
    unsigned int exact[512];// palette colour + 1, open addressed -> index in exactIx
    unsigned char exactIx[512];
    short *cache;           // 6 bits a channel -> nearest index (-1: not yet)
} Palette;

static void pal_exact_add(Palette *p, unsigned int rgb, int ix) {
    unsigned int h = (rgb * 2654435761u) >> 23;

    for (int k = 0; k < 512; k++) {
        unsigned int i = (h + (unsigned int)k) & 511;

        if (p->exact[i] == 0) {
            p->exact[i] = rgb + 1;
            p->exactIx[i] = (unsigned char)ix;
            return;
        }

        if (p->exact[i] == rgb + 1)
            return;
    }
}

static int pal_exact_find(const Palette *p, unsigned int rgb) {
    unsigned int h = (rgb * 2654435761u) >> 23;

    for (int k = 0; k < 512; k++) {
        unsigned int i = (h + (unsigned int)k) & 511;

        if (p->exact[i] == 0)
            return -1;

        if (p->exact[i] == rgb + 1)
            return p->exactIx[i];
    }

    return -1;
}

static void pal_index(Palette *p) {
    memset(p->exact, 0, sizeof(p->exact));

    for (int i = 0; i < p->n; i++) {
        if (i == p->transparent)
            continue;

        const unsigned char *c = p->rgb + 3 * i;
        pal_exact_add(p, ((unsigned int)c[0] << 16) | ((unsigned int)c[1] << 8) | c[2], i);
    }
}

// The palette index for an opaque colour: exact if it's in the palette, else
// the nearest (weighted for the eye: green counts most), cached by 6 bits a
// channel
static int pal_lookup(Palette *p, const unsigned char *c) {
    unsigned int rgb = ((unsigned int)c[0] << 16) | ((unsigned int)c[1] << 8) | c[2];
    int ix = pal_exact_find(p, rgb);

    if (ix >= 0)
        return ix;

    int key = ((c[0] >> 2) << 12) | ((c[1] >> 2) << 6) | (c[2] >> 2);

    if (p->cache && p->cache[key] >= 0)
        return p->cache[key];

    int best = 0, bestd = 0x7fffffff;

    for (int i = 0; i < p->n; i++) {
        if (i == p->transparent)
            continue;

        const unsigned char *q = p->rgb + 3 * i;
        int dr = (int)c[0] - q[0], dg = (int)c[1] - q[1], db = (int)c[2] - q[2];
        int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;

        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }

    if (p->cache)
        p->cache[key] = (short)best;

    return best;
}

// A palette of at most maxcolors for the frames: their own colours when few
// enough, else NeuQuant's, with the most used colours exact. needAlpha
// reserves an entry for transparent pixels (frames with alpha, or frame
// differencing). Returns 0 if memory runs out.
static int make_palette(const unsigned char **frames, const int *chans, int nframes, int w, int h,
                        int maxcolors, int samplefac, int needAlpha, Palette *p) {
    memset(p, 0, sizeof(*p));
    p->transparent = -1;
    p->cache = NULL;

    size_t npix = (size_t)w * (size_t)h;
    int anyTransparent = 0;
    Counts counts;

    if (!counts_init(&counts)) {
        counts_free(&counts);
        return 0;
    }

    for (int f = 0; f < nframes; f++) {
        for (size_t i = 0; i < npix; i++) {
            unsigned char rgb[3];
            int opaque;
            pixel_rgb(frames[f] + i * (size_t)chans[f], chans[f], rgb, &opaque);

            if (!opaque) {
                anyTransparent = 1;
                continue;
            }

            counts_add(&counts, ((unsigned int)rgb[0] << 16) | ((unsigned int)rgb[1] << 8) | rgb[2]);
        }
    }

    int reserve = (needAlpha || anyTransparent) ? 1 : 0;
    int room = maxcolors - reserve;

    if (room < 1)
        room = 1;

    if (!counts.full && counts.distinct <= room) {
        // exact: the colours themselves
        int k = 0;

        for (int i = 0; i < COUNT_SIZE; i++) {
            if (counts.key[i]) {
                unsigned int c = counts.key[i] - 1;
                p->rgb[3 * k] = (unsigned char)(c >> 16);
                p->rgb[3 * k + 1] = (unsigned char)(c >> 8);
                p->rgb[3 * k + 2] = (unsigned char)c;
                k++;
            }
        }

        p->n = k;
    } else {
        // learn a palette from a sample of every frame's opaque pixels
        size_t cap = 1 << 20;                   // at most a million samples
        size_t total = npix * (size_t)nframes;
        size_t every = total / cap + 1;
        unsigned char *sample = (unsigned char *)malloc(3 * (total / every + 1));

        if (!sample) {
            counts_free(&counts);
            return 0;
        }

        size_t ns = 0, seen = 0;

        for (int f = 0; f < nframes; f++) {
            for (size_t i = 0; i < npix; i++, seen++) {
                if (seen % every)
                    continue;

                int opaque;
                pixel_rgb(frames[f] + i * (size_t)chans[f], chans[f], sample + 3 * ns, &opaque);

                if (opaque)
                    ns++;
            }
        }

        if (ns == 0) {
            sample[0] = sample[1] = sample[2] = 0;
            ns = 1;
        }

        int n = room < 2 ? 2 : room;
        neuquant(sample, ns, n, samplefac, p->rgb);
        free(sample);
        p->n = n;

        // the most used colours (each at least 1 in 1000 pixels), exactly:
        // move the nearest entry not yet claimed onto each
        unsigned char claimed[256] = { 0 };
        unsigned long long minCount = (npix * (unsigned long long)nframes) / 1000 + 1;

        for (int round = 0; round < 32; round++) {
            unsigned int bestCount = 0;
            int bestSlot = -1;

            for (int i = 0; i < COUNT_SIZE; i++) {
                if (counts.key[i] && counts.count[i] > bestCount) {
                    bestCount = counts.count[i];
                    bestSlot = i;
                }
            }

            if (bestSlot < 0 || bestCount < minCount)
                break;

            unsigned int c = counts.key[bestSlot] - 1;
            counts.count[bestSlot] = 0;         // taken: not chosen again
            unsigned char col[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
            int best = -1, bestd = 0x7fffffff;

            for (int i = 0; i < p->n; i++) {
                if (claimed[i])
                    continue;

                const unsigned char *q = p->rgb + 3 * i;
                int dr = (int)col[0] - q[0], dg = (int)col[1] - q[1], db = (int)col[2] - q[2];
                int d = dr * dr + dg * dg + db * db;

                if (d < bestd) {
                    bestd = d;
                    best = i;
                }
            }

            if (best < 0)
                break;

            memcpy(p->rgb + 3 * best, col, 3);
            claimed[best] = 1;
        }
    }

    counts_free(&counts);

    if (reserve) {
        p->transparent = p->n;
        p->rgb[3 * p->n] = p->rgb[3 * p->n + 1] = p->rgb[3 * p->n + 2] = 0;
        p->n++;
    }

    p->cache = (short *)malloc(sizeof(short) << 18);

    if (p->cache)
        memset(p->cache, 0xff, sizeof(short) << 18);   // all -1

    pal_index(p);
    return 1;
}

// Each pixel of a frame as its palette index
static void map_frame(Palette *p, const unsigned char *px, int c, size_t npix, unsigned char *out) {
    for (size_t i = 0; i < npix; i++) {
        unsigned char rgb[3];
        int opaque;
        pixel_rgb(px + i * (size_t)c, c, rgb, &opaque);
        out[i] = (unsigned char)(opaque ? pal_lookup(p, rgb) : (p->transparent >= 0 ? p->transparent : 0));
    }
}

//------------------------------------------------------------------------
// image_quantize: a frame's pixels in at most maxcolors colours (kept in its
// own channel layout; alpha is kept as it was)
//------------------------------------------------------------------------
int anim_quantize(const unsigned char *px, int w, int h, int c, int maxcolors, int samplefac, unsigned char *out) {
    Palette p;
    const unsigned char *frames[1] = { px };
    int chans[1] = { c };

    if (maxcolors < 2) maxcolors = 2;
    if (maxcolors > 256) maxcolors = 256;

    if (!make_palette(frames, chans, 1, w, h, maxcolors, samplefac, 0, &p))
        return 0;

    size_t npix = (size_t)w * (size_t)h;

    for (size_t i = 0; i < npix; i++) {
        const unsigned char *s = px + i * (size_t)c;
        unsigned char *d = out + i * (size_t)c;
        unsigned char rgb[3];
        int opaque;
        pixel_rgb(s, c, rgb, &opaque);
        int ix = pal_lookup(&p, rgb);
        const unsigned char *q = p.rgb + 3 * ix;

        if (c >= 3) {
            d[0] = q[0];
            d[1] = q[1];
            d[2] = q[2];

            if (c == 4)
                d[3] = s[3];
        } else {
            d[0] = (unsigned char)((q[0] * 30 + q[1] * 59 + q[2] * 11) / 100);

            if (c == 2)
                d[1] = s[1];
        }
    }

    free(p.cache);
    return 1;
}

//------------------------------------------------------------------------
// LZW, as GIF uses it: variable-width codes from minBits + 1 up to 12 bits,
// packed little-end first into sub-blocks of at most 255 bytes
//------------------------------------------------------------------------
typedef struct {
    Buf *out;
    unsigned char block[255];
    int blockLen;
    unsigned long acc;
    int accBits;
} BitOut;

static void bits_flush_block(BitOut *o) {
    if (o->blockLen > 0) {
        buf_byte(o->out, o->blockLen);
        buf_put(o->out, o->block, (size_t)o->blockLen);
        o->blockLen = 0;
    }
}

static void bits_put(BitOut *o, int code, int width) {
    o->acc |= (unsigned long)code << o->accBits;
    o->accBits += width;

    while (o->accBits >= 8) {
        o->block[o->blockLen++] = (unsigned char)(o->acc & 255);
        o->acc >>= 8;
        o->accBits -= 8;

        if (o->blockLen == 255)
            bits_flush_block(o);
    }
}

#define LZW_HASH 5003                   // a prime a little over 4096

static void lzw_encode(Buf *out, const unsigned char *ix, size_t n, int minBits) {
    int clear = 1 << minBits, end = clear + 1;
    int width = minBits + 1, next = clear + 2;
    int *hkey = (int *)malloc(sizeof(int) * LZW_HASH);     // (prefix << 8 | byte) + 1
    short *hcode = (short *)malloc(sizeof(short) * LZW_HASH);
    BitOut o = { out, { 0 }, 0, 0, 0 };

    buf_byte(out, minBits);

    if (!hkey || !hcode) {
        out->failed = 1;
        free(hkey);
        free(hcode);
        return;
    }

    memset(hkey, 0, sizeof(int) * LZW_HASH);
    bits_put(&o, clear, width);

    if (n > 0) {
        int prefix = ix[0];

        for (size_t i = 1; i < n; i++) {
            int byte = ix[i];
            int key = ((prefix << 8) | byte) + 1;
            int h = (int)(((unsigned)prefix << 8 ^ (unsigned)byte) % LZW_HASH);
            int found = -1;

            while (hkey[h]) {
                if (hkey[h] == key) {
                    found = hcode[h];
                    break;
                }

                h = (h + 1) % LZW_HASH;
            }

            if (found >= 0) {
                prefix = found;
                continue;
            }

            bits_put(&o, prefix, width);

            if (next < 4096) {
                hkey[h] = key;
                hcode[h] = (short)next;

                // the decoder widens its codes one code later than we add them
                if (next == (1 << width) && width < 12)
                    width++;

                next++;
            } else {
                // the table is full: start afresh
                bits_put(&o, clear, width);
                memset(hkey, 0, sizeof(int) * LZW_HASH);
                width = minBits + 1;
                next = clear + 2;
            }

            prefix = byte;
        }

        bits_put(&o, prefix, width);
    }

    bits_put(&o, end, width);

    if (o.accBits > 0)
        bits_put(&o, 0, 8 - o.accBits);

    bits_flush_block(&o);
    buf_byte(out, 0);                   // no more sub-blocks
    free(hkey);
    free(hcode);
}

//------------------------------------------------------------------------
// anim_gif: frames (same size; channels may differ) as an animated GIF.
// delays in milliseconds, one a frame; loop: 0 forever, else how many times
// (1: play once). Returns a malloc'd buffer and its length, or NULL.
//------------------------------------------------------------------------
unsigned char *anim_gif(const unsigned char **frames, const int *chans, int nframes, int w, int h,
                        const int *delays, int loop, int maxcolors, int samplefac, size_t *outLen) {
    if (maxcolors < 2) maxcolors = 2;
    if (maxcolors > 256) maxcolors = 256;

    Palette p;
    // frames after the first are drawn as changes, which need a transparent colour
    int needAlpha = nframes > 1;

    if (!make_palette(frames, chans, nframes, w, h, maxcolors, samplefac, needAlpha, &p))
        return NULL;

    size_t npix = (size_t)w * (size_t)h;
    unsigned char *cur = (unsigned char *)malloc(npix);
    unsigned char *prev = (unsigned char *)malloc(npix);
    unsigned char *rect = (unsigned char *)malloc(npix);
    Buf b = { NULL, 0, 0, 0 };

    if (!cur || !prev || !rect) {
        free(cur);
        free(prev);
        free(rect);
        free(p.cache);
        return NULL;
    }

    // the palette is padded to a power of two, at least 2
    int bits = 1;

    while ((1 << bits) < p.n)
        bits++;

    buf_put(&b, "GIF89a", 6);
    buf_le16(&b, w);
    buf_le16(&b, h);
    buf_byte(&b, 0x80 | 0x70 | (bits - 1));     // global palette, 8-bit colour, its size
    buf_byte(&b, p.transparent >= 0 ? p.transparent : 0);
    buf_byte(&b, 0);

    for (int i = 0; i < (1 << bits); i++) {
        if (i < p.n)
            buf_put(&b, p.rgb + 3 * i, 3);
        else
            buf_put(&b, "\0\0\0", 3);
    }

    if (nframes > 1) {
        // NETSCAPE2.0: how many times to play (0: for ever)
        buf_put(&b, "\x21\xff\x0bNETSCAPE2.0\x03\x01", 16);
        buf_le16(&b, loop == 0 ? 0 : (loop > 65535 ? 65535 : loop));
        buf_byte(&b, 0);
    }

    int minBits = bits < 2 ? 2 : bits;

    for (int f = 0; f < nframes; f++) {
        map_frame(&p, frames[f], chans[f], npix, cur);

        // the rectangle that changed since the last frame (all of the first)
        int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
        int diff = f > 0;

        if (diff) {
            x0 = w;
            y0 = h;
            x1 = -1;
            y1 = -1;

            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    size_t i = (size_t)y * (size_t)w + (size_t)x;

                    if (cur[i] != prev[i]) {
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
                }
            }

            if (x1 < 0) {
                // nothing changed: one transparent pixel keeps the frame's time
                x0 = x1 = 0;
                y0 = y1 = 0;
            }
        }

        int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
        size_t k = 0;

        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                // unchanged pixels show the frame below
                rect[k++] = (diff && cur[i] == prev[i]) ? (unsigned char)p.transparent : cur[i];
            }
        }

        int cs = (delays[f] + 5) / 10;          // centiseconds

        if (cs < 0) cs = 0;
        if (cs > 65535) cs = 65535;

        // graphic control: leave the frame in place, its delay, transparency
        buf_put(&b, "\x21\xf9\x04", 3);
        buf_byte(&b, (1 << 2) | (p.transparent >= 0 ? 1 : 0));
        buf_le16(&b, cs);
        buf_byte(&b, p.transparent >= 0 ? p.transparent : 0);
        buf_byte(&b, 0);

        buf_byte(&b, 0x2c);
        buf_le16(&b, x0);
        buf_le16(&b, y0);
        buf_le16(&b, rw);
        buf_le16(&b, rh);
        buf_byte(&b, 0);                        // no local palette, not interlaced

        lzw_encode(&b, rect, k, minBits);

        unsigned char *t = prev;
        prev = cur;
        cur = t;
    }

    buf_byte(&b, 0x3b);

    free(cur);
    free(prev);
    free(rect);
    free(p.cache);

    if (b.failed) {
        free(b.data);
        return NULL;
    }

    *outLen = b.len;
    return b.data;
}

//------------------------------------------------------------------------
// APNG
//------------------------------------------------------------------------
static unsigned long crc_table[256];
static int crc_ready = 0;

static unsigned long crc32_update(unsigned long crc, const unsigned char *p, size_t n) {
    if (!crc_ready) {
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long c = i;

            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;

            crc_table[i] = c;
        }

        crc_ready = 1;
    }

    crc ^= 0xffffffffUL;

    for (size_t i = 0; i < n; i++)
        crc = crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);

    return crc ^ 0xffffffffUL;
}

// A PNG chunk: length, type, data, CRC of type and data
static void png_chunk(Buf *b, const char *type, const unsigned char *data, size_t n) {
    buf_be32(b, (unsigned long)n);
    size_t start = b->len;
    buf_put(b, type, 4);

    if (n)
        buf_put(b, data, n);

    if (b->failed)
        return;

    buf_be32(b, crc32_update(0, b->data + start, n + 4));
}

static unsigned long be32(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

// Expand a frame to c channels (1..4)
static unsigned char *to_channels(const unsigned char *px, int from, int to, size_t npix) {
    unsigned char *out = (unsigned char *)malloc(npix * (size_t)to);

    if (!out)
        return NULL;

    for (size_t i = 0; i < npix; i++) {
        const unsigned char *s = px + i * (size_t)from;
        unsigned char *d = out + i * (size_t)to;
        unsigned char r, g, bl, a;

        if (from >= 3) { r = s[0]; g = s[1]; bl = s[2]; }
        else { r = g = bl = s[0]; }

        a = from == 4 ? s[3] : (from == 2 ? s[1] : 255);

        if (to >= 3) {
            d[0] = r;
            d[1] = g;
            d[2] = bl;

            if (to == 4) d[3] = a;
        } else {
            d[0] = (unsigned char)((r * 30 + g * 59 + bl * 11) / 100);

            if (to == 2) d[1] = a;
        }
    }

    return out;
}

//------------------------------------------------------------------------
// anim_apng: frames as an animated PNG, full colour. Every frame is whole
// (no differencing). delays in milliseconds; loop: 0 forever, else times.
//------------------------------------------------------------------------
unsigned char *anim_apng(const unsigned char **frames, const int *chans, int nframes, int w, int h,
                         const int *delays, int loop, size_t *outLen) {
    // one channel layout for all: the widest the frames use
    int c = 1;

    for (int f = 0; f < nframes; f++)
        if (chans[f] > c) c = chans[f];

    if (c == 2) c = 4;                          // gray + alpha with colour frames is rare; keep it simple

    size_t npix = (size_t)w * (size_t)h;
    Buf b = { NULL, 0, 0, 0 };
    unsigned long seq = 0;

    buf_put(&b, "\x89PNG\r\n\x1a\n", 8);

    for (int f = 0; f < nframes; f++) {
        const unsigned char *px = frames[f];
        unsigned char *conv = NULL;

        if (chans[f] != c) {
            conv = to_channels(px, chans[f], c, npix);

            if (!conv) {
                b.failed = 1;
                break;
            }

            px = conv;
        }

        int len = 0;
        unsigned char *png = stbi_write_png_to_mem(px, (ptrdiff_t)w * c, w, h, c, &len);
        free(conv);

        if (!png) {
            b.failed = 1;
            break;
        }

        // walk the PNG's chunks: copy IHDR once (then acTL), take IDAT data
        size_t at = 8;

        if (f == 0) {
            while (at + 12 <= (size_t)len) {
                unsigned long n = be32(png + at);

                if (memcmp(png + at + 4, "IHDR", 4) == 0) {
                    png_chunk(&b, "IHDR", png + at + 8, n);
                    break;
                }

                at += 12 + n;
            }

            unsigned char actl[8];
            unsigned long nf = (unsigned long)nframes, nl = (unsigned long)(loop < 0 ? 0 : loop);
            actl[0] = (unsigned char)(nf >> 24); actl[1] = (unsigned char)(nf >> 16);
            actl[2] = (unsigned char)(nf >> 8);  actl[3] = (unsigned char)nf;
            actl[4] = (unsigned char)(nl >> 24); actl[5] = (unsigned char)(nl >> 16);
            actl[6] = (unsigned char)(nl >> 8);  actl[7] = (unsigned char)nl;
            png_chunk(&b, "acTL", actl, 8);
        }

        // fcTL: the whole canvas, this frame's delay (ms / 1000), keep it, replace
        unsigned char fctl[26];
        int ms = delays[f] < 0 ? 0 : (delays[f] > 65535 ? 65535 : delays[f]);
        unsigned long v[5] = { seq++, (unsigned long)w, (unsigned long)h, 0, 0 };

        for (int k = 0; k < 5; k++) {
            fctl[4 * k] = (unsigned char)(v[k] >> 24);
            fctl[4 * k + 1] = (unsigned char)(v[k] >> 16);
            fctl[4 * k + 2] = (unsigned char)(v[k] >> 8);
            fctl[4 * k + 3] = (unsigned char)v[k];
        }

        fctl[20] = (unsigned char)(ms >> 8);
        fctl[21] = (unsigned char)ms;
        fctl[22] = 0x03;                        // 1000: the delay is in ms
        fctl[23] = 0xe8;
        fctl[24] = 0;                           // dispose: none
        fctl[25] = 0;                           // blend: source
        png_chunk(&b, "fcTL", fctl, 26);

        at = 8;

        while (at + 12 <= (size_t)len) {
            unsigned long n = be32(png + at);
            const unsigned char *type = png + at + 4;

            if (memcmp(type, "IDAT", 4) == 0) {
                if (f == 0) {
                    png_chunk(&b, "IDAT", png + at + 8, n);
                } else {
                    // fdAT: a sequence number, then the same data
                    unsigned char *d = (unsigned char *)malloc(n + 4);

                    if (!d) {
                        b.failed = 1;
                        break;
                    }

                    d[0] = (unsigned char)(seq >> 24);
                    d[1] = (unsigned char)(seq >> 16);
                    d[2] = (unsigned char)(seq >> 8);
                    d[3] = (unsigned char)seq;
                    seq++;
                    memcpy(d + 4, png + at + 8, n);
                    png_chunk(&b, "fdAT", d, n + 4);
                    free(d);
                }
            }

            at += 12 + n;
        }

        free(png);

        if (b.failed)
            break;
    }

    png_chunk(&b, "IEND", NULL, 0);

    if (b.failed) {
        free(b.data);
        return NULL;
    }

    *outLen = b.len;
    return b.data;
}
