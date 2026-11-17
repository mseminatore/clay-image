//------------------------------------------------------------------------
// canvas.c -- CanvasCore: anti-aliased drawing into an RGBA pixel buffer
//
//  src/main.clay wraps it as Canvas. Everything is drawn by one rasterizer
//  that fills polygons with the nonzero rule: strokes (with dashes, caps and
//  joins), circles and text (from the glyph outlines of the built-in font,
//  so it can be turned to any angle) are all turned into polygons first. A
//  fill or a stroke is rasterized in a single pass, so the parts of a thick
//  line that overlap at its joins are not blended twice.
//
//  Anti-aliasing: each pixel row is sampled at SUBSAMPLES heights, and at
//  each the covered part of every pixel is measured exactly.
//
//  Colours are r, g, b in 0..255 and an alpha in 0..1; pixels are stored
//  RGBA with straight (not premultiplied) alpha.
//------------------------------------------------------------------------

#include "clay_plugin.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"
#include "font/dejavu_sans.h"

#define SUBSAMPLES 8
#define MITER_LIMIT 4.0f
#define MAX_SIDE 16384

static const ClayPluginAPI *api;

//------------------------------------------------------------------------
// what each canvas owns
//------------------------------------------------------------------------
typedef struct {
    unsigned char *px;      // width * height * 4 bytes, RGBA
    int width;
    int height;
    int clip_x0, clip_y0;   // drawing is limited to [x0, x1) x [y0, y1)
    int clip_x1, clip_y1;
    float *cov;             // one row of coverage, width + 2 floats
} Canvas;

static void canvas_free(void *data) {
    Canvas *cv = (Canvas *)data;

    if (cv) {
        free(cv->px);
        free(cv->cov);
        free(cv);
    }
}

static Canvas *canvas_of(ClayPluginObject self) {
    Canvas *cv = (Canvas *)api->get_data(self);

    if (!cv || !cv->px) {
        api->raise("Canvas: not a canvas (make one with Canvas new)", "RuntimeError");
        return NULL;
    }

    return cv;
}

//------------------------------------------------------------------------
// numbers from arguments
//------------------------------------------------------------------------
static float num_arg(int argc, ClayPluginValue *argv, int i, float dflt) {
    if (i >= argc || api->is_nil(argv[i]))
        return dflt;
    if (api->is_int(argv[i]))
        return (float)api->to_int(argv[i]);
    if (api->is_float(argv[i]))
        return api->to_float(argv[i]);
    return dflt;
}

// A flat Array of numbers (rows are flattened) into a new buffer; *n gets
// the count. NULL (and *n = 0) for an empty Array, or after raising.
static float *floats_arg(ClayPluginValue v, size_t *n, const char *what) {
    *n = 0;

    if (!api->is_array(v)) {
        api->raise(what, "ArgumentError");
        return NULL;
    }

    size_t count = api->to_floats(v, NULL, 0);
    if (count == 0)
        return NULL;

    float *f = (float *)malloc(count * sizeof(float));
    if (!f) {
        api->raise("Canvas: out of memory", "RuntimeError");
        return NULL;
    }

    api->to_floats(v, f, count);
    *n = count;
    return f;
}

typedef struct { float r, g, b, a; } Color;

// r, g, b (0..255) and alpha (0..1) from argv[i..i+3]
static Color color_args(int argc, ClayPluginValue *argv, int i) {
    Color c;
    c.r = num_arg(argc, argv, i, 0.0f);
    c.g = num_arg(argc, argv, i + 1, 0.0f);
    c.b = num_arg(argc, argv, i + 2, 0.0f);
    c.a = num_arg(argc, argv, i + 3, 1.0f);
    c.r = c.r < 0 ? 0 : (c.r > 255 ? 255 : c.r);
    c.g = c.g < 0 ? 0 : (c.g > 255 ? 255 : c.g);
    c.b = c.b < 0 ? 0 : (c.b > 255 ? 255 : c.b);
    c.a = !(c.a > 0) ? 0 : (c.a > 1 ? 1 : c.a);
    return c;
}

//------------------------------------------------------------------------
// blending: src over dst, with straight alpha
//------------------------------------------------------------------------
static void blend(unsigned char *d, float r, float g, float b, float sa) {
    if (sa <= 0.0f)
        return;

    if (sa >= 1.0f) {
        d[0] = (unsigned char)(r + 0.5f);
        d[1] = (unsigned char)(g + 0.5f);
        d[2] = (unsigned char)(b + 0.5f);
        d[3] = 255;
        return;
    }

    float da = d[3] / 255.0f;
    float oa = sa + da * (1.0f - sa);

    if (oa <= 0.0f)
        return;

    float k = da * (1.0f - sa);
    d[0] = (unsigned char)((r * sa + d[0] * k) / oa + 0.5f);
    d[1] = (unsigned char)((g * sa + d[1] * k) / oa + 0.5f);
    d[2] = (unsigned char)((b * sa + d[2] * k) / oa + 0.5f);
    d[3] = (unsigned char)(oa * 255.0f + 0.5f);
}

//------------------------------------------------------------------------
// edges: what the rasterizer fills
//------------------------------------------------------------------------
typedef struct {
    float x0, y0, x1, y1;   // y0 < y1
    float dxdy;
    int dir;                // +1 if the edge ran downwards, -1 upwards
} Edge;

typedef struct {
    Edge *e;
    int n, cap;
    int failed;
} EdgeList;

static void add_edge(EdgeList *el, float x0, float y0, float x1, float y1) {
    if (y0 == y1 || el->failed)
        return;
    if (!(x0 == x0) || !(y0 == y0) || !(x1 == x1) || !(y1 == y1))
        return;    // NaN

    if (el->n == el->cap) {
        int cap = el->cap ? el->cap * 2 : 256;
        Edge *grown = (Edge *)realloc(el->e, (size_t)cap * sizeof(Edge));
        if (!grown) {
            el->failed = 1;
            return;
        }
        el->e = grown;
        el->cap = cap;
    }

    Edge *e = &el->e[el->n++];
    if (y0 < y1) {
        e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir = 1;
    } else {
        e->x0 = x1; e->y0 = y1; e->x1 = x0; e->y1 = y0; e->dir = -1;
    }
    e->dxdy = (e->x1 - e->x0) / (e->y1 - e->y0);
}

// A closed polygon of n points (x, y pairs), as given
static void add_poly_raw(EdgeList *el, const float *p, int n) {
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        add_edge(el, p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1]);
    }
}

// A closed polygon, turned to a positive orientation: the pieces of a
// stroke must all wind the same way, or where they overlap they cancel out
static void add_poly(EdgeList *el, const float *p, int n) {
    float area = 0.0f;

    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += p[2 * i] * p[2 * j + 1] - p[2 * j] * p[2 * i + 1];
    }

    if (area >= 0.0f) {
        add_poly_raw(el, p, n);
        return;
    }

    for (int i = 0; i < n; i++) {
        int a = n - 1 - i, b = (n - 2 - i + n) % n;
        add_edge(el, p[2 * a], p[2 * a + 1], p[2 * b], p[2 * b + 1]);
    }
}

static int circle_segments(float r) {
    int n = (int)(r * 1.5f) + 8;
    return n < 8 ? 8 : (n > 256 ? 256 : n);
}

static void add_circle(EdgeList *el, float cx, float cy, float r) {
    if (!(r > 0.0f))
        return;

    int n = circle_segments(r);
    float pts[2 * 256];

    for (int i = 0; i < n; i++) {
        float t = (float)i * 6.28318531f / (float)n;
        pts[2 * i] = cx + r * cosf(t);
        pts[2 * i + 1] = cy + r * sinf(t);
    }

    add_poly(el, pts, n);
}

//------------------------------------------------------------------------
// the rasterizer: fill everything in el, nonzero (or even-odd) rule
//------------------------------------------------------------------------
static int edge_cmp(const void *a, const void *b) {
    float ya = ((const Edge *)a)->y0, yb = ((const Edge *)b)->y0;
    return ya < yb ? -1 : (ya > yb ? 1 : 0);
}

typedef struct { float x; int dir; } Crossing;

static void cover_span(Canvas *cv, float xa, float xb, float w, int *lo, int *hi) {
    float cx0 = (float)cv->clip_x0, cx1 = (float)cv->clip_x1;

    if (xa < cx0) xa = cx0;
    if (xb > cx1) xb = cx1;
    if (!(xb > xa))
        return;

    int ia = (int)floorf(xa), ib = (int)floorf(xb);
    float *cov = cv->cov;

    if (ia < *lo) *lo = ia;
    if (ib > *hi) *hi = ib;

    if (ia == ib) {
        cov[ia] += (xb - xa) * w;
        return;
    }

    cov[ia] += ((float)(ia + 1) - xa) * w;
    for (int i = ia + 1; i < ib; i++)
        cov[i] += w;
    if (ib < cv->width)
        cov[ib] += (xb - (float)ib) * w;
}

static void rasterize(Canvas *cv, EdgeList *el, Color c, int evenodd) {
    if (el->failed) {
        api->raise("Canvas: out of memory", "RuntimeError");
        return;
    }
    if (el->n == 0 || c.a <= 0.0f)
        return;

    float ymin = el->e[0].y0, ymax = el->e[0].y1;
    for (int i = 1; i < el->n; i++) {
        if (el->e[i].y0 < ymin) ymin = el->e[i].y0;
        if (el->e[i].y1 > ymax) ymax = el->e[i].y1;
    }

    int py0 = (int)floorf(ymin), py1 = (int)ceilf(ymax);
    if (py0 < cv->clip_y0) py0 = cv->clip_y0;
    if (py1 > cv->clip_y1) py1 = cv->clip_y1;
    if (py0 >= py1 || cv->clip_x0 >= cv->clip_x1)
        return;

    qsort(el->e, (size_t)el->n, sizeof(Edge), edge_cmp);

    int *active = (int *)malloc((size_t)el->n * sizeof(int));
    Crossing *xs = (Crossing *)malloc((size_t)el->n * sizeof(Crossing));
    if (!active || !xs) {
        free(active);
        free(xs);
        api->raise("Canvas: out of memory", "RuntimeError");
        return;
    }

    int nactive = 0, next = 0;
    const float w = 1.0f / SUBSAMPLES;

    for (int py = py0; py < py1; py++) {
        int lo = cv->width, hi = -1;

        for (int s = 0; s < SUBSAMPLES; s++) {
            float sy = (float)py + ((float)s + 0.5f) * w;

            // edges start being active when sy reaches y0, and stop at y1
            while (next < el->n && el->e[next].y0 <= sy)
                active[nactive++] = next++;

            int nx = 0;
            for (int k = 0; k < nactive; k++) {
                Edge *e = &el->e[active[k]];

                if (e->y1 <= sy) {
                    active[k--] = active[--nactive];
                    continue;
                }
                if (e->y0 > sy)
                    continue;

                xs[nx].x = e->x0 + (sy - e->y0) * e->dxdy;
                xs[nx].dir = e->dir;
                nx++;
            }

            // sort the crossings left to right (few per row: insertion sort)
            for (int i = 1; i < nx; i++) {
                Crossing t = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j].x > t.x) {
                    xs[j + 1] = xs[j];
                    j--;
                }
                xs[j + 1] = t;
            }

            int wind = 0;
            for (int i = 0; i + 1 < nx; i++) {
                wind += evenodd ? 1 : xs[i].dir;
                int inside = evenodd ? (wind & 1) : (wind != 0);
                if (inside)
                    cover_span(cv, xs[i].x, xs[i + 1].x, w, &lo, &hi);
            }
        }

        if (hi < lo)
            continue;
        if (hi >= cv->width)
            hi = cv->width - 1;

        unsigned char *row = cv->px + (size_t)py * cv->width * 4;
        for (int x = lo; x <= hi; x++) {
            float cover = cv->cov[x];
            cv->cov[x] = 0.0f;
            if (cover > 0.0f)
                blend(row + 4 * x, c.r, c.g, c.b, (cover > 1.0f ? 1.0f : cover) * c.a);
        }
    }

    free(active);
    free(xs);
}

//------------------------------------------------------------------------
// strokes: a polyline into polygons
//------------------------------------------------------------------------
enum { CAP_BUTT = 0, CAP_ROUND = 1, CAP_SQUARE = 2 };
enum { JOIN_MITER = 0, JOIN_ROUND = 1, JOIN_BEVEL = 2 };

static void add_join(EdgeList *el, float vx, float vy, float d0x, float d0y, float d1x, float d1y,
                     float hw, int join) {
    if (join == JOIN_ROUND) {
        add_circle(el, vx, vy, hw);
        return;
    }

    float cross = d0x * d1y - d0y * d1x;
    if (fabsf(cross) < 1e-6f && d0x * d1x + d0y * d1y > 0.0f)
        return;    // straight on: nothing to fill

    // the outer side of the turn
    float side = cross > 0.0f ? -1.0f : 1.0f;
    float o0x = -d0y * hw * side, o0y = d0x * hw * side;
    float o1x = -d1y * hw * side, o1y = d1x * hw * side;

    float cosang = (o0x * o1x + o0y * o1y) / (hw * hw);
    if (join == JOIN_MITER && 1.0f + cosang > 1e-6f) {
        float k = 1.0f / (1.0f + cosang);    // |miter| = hw / cos(theta / 2)
        float mx = (o0x + o1x) * k, my = (o0y + o1y) * k;
        if (sqrtf(mx * mx + my * my) <= MITER_LIMIT * hw) {
            float p[8] = { vx, vy, vx + o0x, vy + o0y, vx + mx, vy + my, vx + o1x, vy + o1y };
            add_poly(el, p, 4);
            return;
        }
    }

    float p[6] = { vx, vy, vx + o0x, vy + o0y, vx + o1x, vy + o1y };
    add_poly(el, p, 3);
}

static void add_cap(EdgeList *el, float x, float y, float dx, float dy, float hw, int cap) {
    // (dx, dy) points out of the line
    if (cap == CAP_ROUND) {
        add_circle(el, x, y, hw);
    } else if (cap == CAP_SQUARE) {
        float nx = -dy * hw, ny = dx * hw;
        float p[8] = { x + nx, y + ny, x + nx + dx * hw, y + ny + dy * hw,
                       x - nx + dx * hw, y - ny + dy * hw, x - nx, y - ny };
        add_poly(el, p, 4);
    }
}

// One polyline (n points, consecutive repeats already removed)
static void stroke_one(EdgeList *el, const float *p, int n, float hw, int cap, int join, int closed) {
    if (n < 2) {
        if (n == 1 && cap == CAP_ROUND) add_circle(el, p[0], p[1], hw);
        if (n == 1 && cap == CAP_SQUARE) {
            float q[8] = { p[0] - hw, p[1] - hw, p[0] + hw, p[1] - hw, p[0] + hw, p[1] + hw, p[0] - hw, p[1] + hw };
            add_poly(el, q, 4);
        }
        return;
    }

    int segs = closed ? n : n - 1;
    float pdx = 0, pdy = 0, fdx = 0, fdy = 0;

    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % n;
        float ax = p[2 * i], ay = p[2 * i + 1], bx = p[2 * j], by = p[2 * j + 1];
        float dx = bx - ax, dy = by - ay;
        float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f)
            continue;
        dx /= len;
        dy /= len;

        float nx = -dy * hw, ny = dx * hw;
        float q[8] = { ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny };
        add_poly(el, q, 4);

        if (i == 0) {
            fdx = dx;
            fdy = dy;
        } else {
            add_join(el, ax, ay, pdx, pdy, dx, dy, hw, join);
        }
        pdx = dx;
        pdy = dy;
    }

    if (closed) {
        add_join(el, p[0], p[1], pdx, pdy, fdx, fdy, hw, join);
    } else {
        add_cap(el, p[0], p[1], -fdx, -fdy, hw, cap);
        add_cap(el, p[2 * (n - 1)], p[2 * (n - 1) + 1], pdx, pdy, hw, cap);
    }
}

// A polyline, cut into dashes if there are any (lengths: on, off, on, ...)
static void stroke_path(EdgeList *el, const float *p, int n, float hw, int cap, int join, int closed,
                        const float *dash, int ndash) {
    // drop repeated points
    float *q = (float *)malloc((size_t)(n + 1) * 2 * sizeof(float));
    if (!q) {
        el->failed = 1;
        return;
    }

    int m = 0;
    for (int i = 0; i < n; i++) {
        float x = p[2 * i], y = p[2 * i + 1];
        if (!(x == x) || !(y == y))
            continue;
        if (m > 0 && q[2 * (m - 1)] == x && q[2 * (m - 1) + 1] == y)
            continue;
        q[2 * m] = x;
        q[2 * m + 1] = y;
        m++;
    }

    float total = 0.0f;
    for (int i = 0; i < ndash; i++)
        total += dash[i] > 0 ? dash[i] : 0;

    if (ndash < 2 || total <= 0.0f) {
        stroke_one(el, q, m, hw, cap, join, closed && m > 2);
        free(q);
        return;
    }

    if (closed && m > 2) {
        q[2 * m] = q[0];
        q[2 * m + 1] = q[1];
        m++;
    }

    // walk the line, emitting the "on" pieces
    float *piece = (float *)malloc((size_t)(2 * m + 4) * 2 * sizeof(float));
    if (!piece) {
        free(q);
        el->failed = 1;
        return;
    }

    int di = 0, np = 0, on = 1;
    float left = dash[0];

    piece[np * 2] = q[0];
    piece[np * 2 + 1] = q[1];
    np = 1;

    for (int i = 0; i + 1 < m; i++) {
        float ax = q[2 * i], ay = q[2 * i + 1], bx = q[2 * i + 2], by = q[2 * i + 3];
        float dx = bx - ax, dy = by - ay;
        float len = sqrtf(dx * dx + dy * dy), at = 0.0f;

        while (len - at > left) {
            at += left;
            float x = ax + dx * at / len, y = ay + dy * at / len;

            if (on) {
                piece[np * 2] = x;
                piece[np * 2 + 1] = y;
                np++;
                stroke_one(el, piece, np, hw, cap, join, 0);
            }

            on = !on;
            di = (di + 1) % ndash;
            left = dash[di] > 0 ? dash[di] : 0;
            np = 0;
            if (on) {
                piece[0] = x;
                piece[1] = y;
                np = 1;
            }
        }

        left -= len - at;
        if (on) {
            piece[np * 2] = bx;
            piece[np * 2 + 1] = by;
            np++;
            if (np >= 2 * m + 2) {    // keep the piece short: restart it here
                stroke_one(el, piece, np, hw, cap, join, 0);
                piece[0] = bx;
                piece[1] = by;
                np = 1;
            }
        }
    }

    if (on && np >= 2)
        stroke_one(el, piece, np, hw, cap, join, 0);

    free(piece);
    free(q);
}

//------------------------------------------------------------------------
// text: glyph outlines from the built-in font
//------------------------------------------------------------------------
static stbtt_fontinfo font;
static int font_state;    // 0 not loaded, 1 ok, -1 failed

static int font_ready(void) {
    if (font_state == 0) {
        int off = stbtt_GetFontOffsetForIndex(dejavu_sans_ttf, 0);
        font_state = (off >= 0 && stbtt_InitFont(&font, dejavu_sans_ttf, off)) ? 1 : -1;
    }
    if (font_state < 0)
        api->raise("Canvas: the built-in font couldn't be read", "RuntimeError");
    return font_state > 0;
}

// The next code point of a UTF-8 string (bad bytes read as U+FFFD)
static int utf8_next(const unsigned char **s) {
    const unsigned char *p = *s;
    int c = *p++;

    if (c >= 0x80) {
        int extra = c >= 0xF0 ? 3 : (c >= 0xE0 ? 2 : (c >= 0xC0 ? 1 : -1));
        if (extra < 0) {
            c = 0xFFFD;
        } else {
            c &= 0x3F >> extra;
            for (int i = 0; i < extra; i++) {
                if ((*p & 0xC0) != 0x80) {
                    c = 0xFFFD;
                    break;
                }
                c = (c << 6) | (*p++ & 0x3F);
            }
        }
    }

    *s = p;
    return c;
}

// Width in pixels of a string at a size (the em, in pixels)
static float text_width(const char *s, float size) {
    float scale = stbtt_ScaleForMappingEmToPixels(&font, size);
    const unsigned char *p = (const unsigned char *)s;
    float pen = 0.0f;
    int prev = 0;

    while (*p) {
        int g = stbtt_FindGlyphIndex(&font, utf8_next(&p));
        int adv, lsb;

        if (prev)
            pen += stbtt_GetGlyphKernAdvance(&font, prev, g) * scale;
        stbtt_GetGlyphHMetrics(&font, g, &adv, &lsb);
        pen += adv * scale;
        prev = g;
    }

    return pen;
}

typedef struct {
    float *p;
    int n, cap;
} Points;

static int pts_push(Points *pt, float x, float y) {
    if (pt->n == pt->cap) {
        int cap = pt->cap ? pt->cap * 2 : 64;
        float *grown = (float *)realloc(pt->p, (size_t)cap * 2 * sizeof(float));
        if (!grown)
            return 0;
        pt->p = grown;
        pt->cap = cap;
    }
    pt->p[2 * pt->n] = x;
    pt->p[2 * pt->n + 1] = y;
    pt->n++;
    return 1;
}

// Glyph outlines of s, with the baseline's start at (x, y), turned by
// angle (radians, counter-clockwise on screen)
static void add_text(EdgeList *el, const char *s, float x, float y, float size, float angle) {
    float scale = stbtt_ScaleForMappingEmToPixels(&font, size);
    float ca = cosf(angle), sa = sinf(angle);
    int steps = size < 12 ? 3 : (size < 40 ? 5 : 8);
    const unsigned char *p = (const unsigned char *)s;
    float pen = 0.0f;
    int prev = 0;
    Points pt = { NULL, 0, 0 };

    while (*p) {
        int g = stbtt_FindGlyphIndex(&font, utf8_next(&p));
        int adv, lsb;

        if (prev)
            pen += stbtt_GetGlyphKernAdvance(&font, prev, g) * scale;

        stbtt_vertex *v = NULL;
        int nv = stbtt_GetGlyphShape(&font, g, &v);
        float lx = 0, ly = 0;
        pt.n = 0;

        for (int i = 0; i < nv; i++) {
            // glyph units, y up -> pixels along the baseline, y down
            float gx = v[i].x * scale, gy = v[i].y * scale;

            if (v[i].type == STBTT_vmove) {
                if (pt.n > 2) add_poly_raw(el, pt.p, pt.n);
                pt.n = 0;
            } else if (v[i].type == STBTT_vcurve) {
                float cx = v[i].cx * scale, cy = v[i].cy * scale;
                for (int k = 1; k < steps; k++) {
                    float t = (float)k / steps, u = 1.0f - t;
                    float qx = u * u * lx + 2 * u * t * cx + t * t * gx;
                    float qy = u * u * ly + 2 * u * t * cy + t * t * gy;
                    float ox = pen + qx, oy = -qy;
                    if (!pts_push(&pt, x + ox * ca + oy * sa, y - ox * sa + oy * ca)) el->failed = 1;
                }
            } else if (v[i].type == STBTT_vcubic) {
                float c1x = v[i].cx * scale, c1y = v[i].cy * scale;
                float c2x = v[i].cx1 * scale, c2y = v[i].cy1 * scale;
                for (int k = 1; k < steps; k++) {
                    float t = (float)k / steps, u = 1.0f - t;
                    float qx = u * u * u * lx + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * gx;
                    float qy = u * u * u * ly + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * gy;
                    float ox = pen + qx, oy = -qy;
                    if (!pts_push(&pt, x + ox * ca + oy * sa, y - ox * sa + oy * ca)) el->failed = 1;
                }
            }

            float ox = pen + gx, oy = -gy;
            if (!pts_push(&pt, x + ox * ca + oy * sa, y - ox * sa + oy * ca)) el->failed = 1;
            lx = gx;
            ly = gy;
        }

        if (pt.n > 2) add_poly_raw(el, pt.p, pt.n);
        stbtt_FreeShape(&font, v);

        stbtt_GetGlyphHMetrics(&font, g, &adv, &lsb);
        pen += adv * scale;
        prev = g;
    }

    free(pt.p);
}

//------------------------------------------------------------------------
// CanvasCore _create(width, height, r, g, b, a) -> a new canvas filled
// with the colour; self is the prototype
//------------------------------------------------------------------------
static ClayPluginValue canvas_create(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    if (argc < 2 || !api->is_int(argv[0]) || !api->is_int(argv[1])) {
        api->raise("Canvas: new expects a width and a height", "ArgumentError");
        return 0;
    }

    intptr_t w = api->to_int(argv[0]), h = api->to_int(argv[1]);
    if (w < 1 || h < 1 || w > MAX_SIDE || h > MAX_SIDE) {
        api->raise("Canvas: width and height must be 1 to 16384", "ArgumentError");
        return 0;
    }

    Canvas *cv = (Canvas *)calloc(1, sizeof(Canvas));
    if (cv) {
        cv->px = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
        cv->cov = (float *)calloc((size_t)w + 2, sizeof(float));
    }
    if (!cv || !cv->px || !cv->cov) {
        canvas_free(cv);
        api->raise("Canvas: out of memory", "RuntimeError");
        return 0;
    }

    cv->width = (int)w;
    cv->height = (int)h;
    cv->clip_x1 = cv->width;
    cv->clip_y1 = cv->height;

    Color c = color_args(argc, argv, 2);
    unsigned char fill[4] = { (unsigned char)(c.r + 0.5f), (unsigned char)(c.g + 0.5f),
                              (unsigned char)(c.b + 0.5f), (unsigned char)(c.a * 255.0f + 0.5f) };
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        memcpy(cv->px + 4 * i, fill, 4);

    ClayPluginObject obj = api->clone(self);
    api->set_data(obj, cv);
    return (ClayPluginValue)obj;
}

static ClayPluginValue canvas_width(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc);
    CLAY_UNUSED(argv);
    Canvas *cv = canvas_of(self);
    return cv ? api->make_int(cv->width) : 0;
}

static ClayPluginValue canvas_height(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc);
    CLAY_UNUSED(argv);
    Canvas *cv = canvas_of(self);
    return cv ? api->make_int(cv->height) : 0;
}

// The paths of a fill or stroke: flat x, y coordinates and a count of
// points per path; counts nil means one path of every point
static int read_paths(ClayPluginValue coords, ClayPluginValue counts, float **p, size_t *np,
                      float **cnt, size_t *ncnt) {
    *p = floats_arg(coords, np, "Canvas: expected an Array of x, y coordinates");
    *cnt = NULL;
    *ncnt = 0;

    if (*np % 2 != 0) {
        free(*p);
        *p = NULL;
        api->raise("Canvas: coordinates come in x, y pairs", "ArgumentError");
        return 0;
    }

    if (!api->is_nil(counts)) {
        *cnt = floats_arg(counts, ncnt, "Canvas: expected an Array of point counts");
        size_t sum = 0;
        for (size_t i = 0; i < *ncnt; i++)
            sum += (*cnt)[i] > 0 ? (size_t)(*cnt)[i] : 0;
        if (sum != *np / 2) {
            free(*p);
            free(*cnt);
            *p = NULL;
            *cnt = NULL;
            api->raise("Canvas: the point counts don't add up to the coordinates", "ArgumentError");
            return 0;
        }
    }

    return 1;
}

//------------------------------------------------------------------------
// canvas _fill(coords, counts, r, g, b, a, evenOdd)
//------------------------------------------------------------------------
static ClayPluginValue canvas_fill(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Canvas *cv = canvas_of(self);
    if (!cv)
        return 0;
    if (argc < 2) {
        api->raise("Canvas: fill expects coordinates", "ArgumentError");
        return 0;
    }

    float *p, *cnt;
    size_t np, ncnt;
    if (!read_paths(argv[0], argv[1], &p, &np, &cnt, &ncnt))
        return 0;

    EdgeList el = { NULL, 0, 0, 0 };
    if (cnt) {
        size_t at = 0;
        for (size_t i = 0; i < ncnt; i++) {
            int k = cnt[i] > 0 ? (int)cnt[i] : 0;
            if (k >= 3) add_poly_raw(&el, p + 2 * at, k);
            at += (size_t)k;
        }
    } else if (np >= 6) {
        add_poly_raw(&el, p, (int)(np / 2));
    }

    int evenodd = (int)num_arg(argc, argv, 6, 0) != 0;

    rasterize(cv, &el, color_args(argc, argv, 2), evenodd);
    free(el.e);
    free(p);
    free(cnt);
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// canvas _stroke(coords, counts, width, r, g, b, a, cap, join, closed, dashes)
// cap 0 butt, 1 round, 2 square; join 0 miter, 1 round, 2 bevel
//------------------------------------------------------------------------
static ClayPluginValue canvas_stroke(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Canvas *cv = canvas_of(self);
    if (!cv)
        return 0;
    if (argc < 3) {
        api->raise("Canvas: stroke expects coordinates and a width", "ArgumentError");
        return 0;
    }

    float *p, *cnt;
    size_t np, ncnt;
    if (!read_paths(argv[0], argv[1], &p, &np, &cnt, &ncnt))
        return 0;

    float width = num_arg(argc, argv, 2, 1.0f);
    Color c = color_args(argc, argv, 3);
    int cap = (int)num_arg(argc, argv, 7, CAP_BUTT);
    int join = (int)num_arg(argc, argv, 8, JOIN_ROUND);
    int closed = (int)num_arg(argc, argv, 9, 0);

    float *dash = NULL;
    size_t ndash = 0;
    if (argc > 10 && !api->is_nil(argv[10]))
        dash = floats_arg(argv[10], &ndash, "Canvas: dashes are an Array of lengths");

    EdgeList el = { NULL, 0, 0, 0 };
    if (width > 0.0f && np >= 2) {
        float hw = width / 2.0f;
        if (cnt) {
            size_t at = 0;
            for (size_t i = 0; i < ncnt; i++) {
                int k = cnt[i] > 0 ? (int)cnt[i] : 0;
                if (k > 0) stroke_path(&el, p + 2 * at, k, hw, cap, join, closed, dash, (int)ndash);
                at += (size_t)k;
            }
        } else {
            stroke_path(&el, p, (int)(np / 2), hw, cap, join, closed, dash, (int)ndash);
        }
    }

    rasterize(cv, &el, c, 0);
    free(el.e);
    free(p);
    free(cnt);
    free(dash);
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// canvas _circles(xs_ys, radius(es), r, g, b, a) -> fill circles, all in
// one pass (scatter plots)
//------------------------------------------------------------------------
static ClayPluginValue canvas_circles(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Canvas *cv = canvas_of(self);
    if (!cv)
        return 0;
    if (argc < 2) {
        api->raise("Canvas: circles expects centres and a radius", "ArgumentError");
        return 0;
    }

    size_t np = 0, nr = 0;
    float *p = floats_arg(argv[0], &np, "Canvas: expected an Array of x, y centres");
    float one = num_arg(argc, argv, 1, 0.0f);
    float *radii = api->is_array(argv[1]) ? floats_arg(argv[1], &nr, "Canvas: radii") : NULL;

    EdgeList el = { NULL, 0, 0, 0 };
    for (size_t i = 0; i + 1 < np; i += 2) {
        float r = radii ? (i / 2 < nr ? radii[i / 2] : 0.0f) : one;
        add_circle(&el, p[i], p[i + 1], r);
    }

    rasterize(cv, &el, color_args(argc, argv, 2), 0);
    free(el.e);
    free(p);
    free(radii);
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// canvas _text(string, x, y, size, r, g, b, a, angleDegrees)
// x, y: where the baseline starts
//------------------------------------------------------------------------
static ClayPluginValue canvas_text(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Canvas *cv = canvas_of(self);
    if (!cv)
        return 0;
    if (argc < 4 || !api->is_string(argv[0])) {
        api->raise("Canvas: text expects a String, x, y and a size", "ArgumentError");
        return 0;
    }
    if (!font_ready())
        return 0;

    float x = num_arg(argc, argv, 1, 0), y = num_arg(argc, argv, 2, 0), size = num_arg(argc, argv, 3, 12);
    float angle = num_arg(argc, argv, 8, 0) * 3.14159265f / 180.0f;

    EdgeList el = { NULL, 0, 0, 0 };
    if (size > 0.0f)
        add_text(&el, api->to_string(argv[0]), x, y, size, angle);

    rasterize(cv, &el, color_args(argc, argv, 4), 0);
    free(el.e);
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// canvas textWidth(string, size) -> its width in pixels
//------------------------------------------------------------------------
static ClayPluginValue canvas_text_width(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);
    if (argc < 2 || !api->is_string(argv[0])) {
        api->raise("Canvas: textWidth expects a String and a size", "ArgumentError");
        return 0;
    }
    if (!font_ready())
        return 0;

    return api->make_float(text_width(api->to_string(argv[0]), num_arg(argc, argv, 1, 12)));
}

//------------------------------------------------------------------------
// canvas _blit(bytes, width, height, channels, x, y, w, h, alpha, flipY)
// An image's pixels stretched over a rectangle, nearest pixel
//------------------------------------------------------------------------
static ClayPluginValue canvas_blit(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Canvas *cv = canvas_of(self);
    if (!cv)
        return 0;
    if (argc < 8 || !api->is_string(argv[0])) {
        api->raise("Canvas: drawImage expects an image and a rectangle", "ArgumentError");
        return 0;
    }

    size_t len = 0;
    const unsigned char *src = (const unsigned char *)api->to_bytes(argv[0], &len);
    int iw = (int)num_arg(argc, argv, 1, 0), ih = (int)num_arg(argc, argv, 2, 0), ic = (int)num_arg(argc, argv, 3, 0);

    if (!src || iw < 1 || ih < 1 || ic < 1 || ic > 4 || len != (size_t)iw * ih * ic) {
        api->raise("Canvas: drawImage: the image's pixels don't match its size", "ArgumentError");
        return 0;
    }

    float x = num_arg(argc, argv, 4, 0), y = num_arg(argc, argv, 5, 0);
    float w = num_arg(argc, argv, 6, (float)iw), h = num_arg(argc, argv, 7, (float)ih);
    float alpha = num_arg(argc, argv, 8, 1.0f);
    int flip = (int)num_arg(argc, argv, 9, 0);

    if (!(w > 0.0f) || !(h > 0.0f) || !(alpha > 0.0f))
        return (ClayPluginValue)self;
    if (alpha > 1.0f)
        alpha = 1.0f;

    int x0 = (int)floorf(x + 0.5f), y0 = (int)floorf(y + 0.5f);
    int x1 = (int)floorf(x + w + 0.5f), y1 = (int)floorf(y + h + 0.5f);
    if (x0 < cv->clip_x0) x0 = cv->clip_x0;
    if (y0 < cv->clip_y0) y0 = cv->clip_y0;
    if (x1 > cv->clip_x1) x1 = cv->clip_x1;
    if (y1 > cv->clip_y1) y1 = cv->clip_y1;

    for (int py = y0; py < y1; py++) {
        int sy = (int)(((float)py + 0.5f - y) / h * ih);
        if (sy < 0) sy = 0;
        if (sy >= ih) sy = ih - 1;
        if (flip) sy = ih - 1 - sy;

        unsigned char *row = cv->px + (size_t)py * cv->width * 4;
        for (int px = x0; px < x1; px++) {
            int sx = (int)(((float)px + 0.5f - x) / w * iw);
            if (sx < 0) sx = 0;
            if (sx >= iw) sx = iw - 1;

            const unsigned char *s = src + ((size_t)sy * iw + sx) * ic;
            float r, g, b, a;
            if (ic < 3) {
                r = g = b = s[0];
                a = ic == 2 ? s[1] / 255.0f : 1.0f;
            } else {
                r = s[0];
                g = s[1];
                b = s[2];
                a = ic == 4 ? s[3] / 255.0f : 1.0f;
            }
            blend(row + 4 * px, r, g, b, a * alpha);
        }
    }

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// canvas clip(x, y, w, h) / unclip
//------------------------------------------------------------------------
static ClayPluginValue canvas_clip(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Canvas *cv = canvas_of(self);
    if (!cv)
        return 0;

    float x = num_arg(argc, argv, 0, 0), y = num_arg(argc, argv, 1, 0);
    float w = num_arg(argc, argv, 2, (float)cv->width), h = num_arg(argc, argv, 3, (float)cv->height);
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);

    cv->clip_x0 = x0 < 0 ? 0 : (x0 > cv->width ? cv->width : x0);
    cv->clip_y0 = y0 < 0 ? 0 : (y0 > cv->height ? cv->height : y0);
    cv->clip_x1 = x1 < cv->clip_x0 ? cv->clip_x0 : (x1 > cv->width ? cv->width : x1);
    cv->clip_y1 = y1 < cv->clip_y0 ? cv->clip_y0 : (y1 > cv->height ? cv->height : y1);
    return (ClayPluginValue)self;
}

static ClayPluginValue canvas_unclip(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc);
    CLAY_UNUSED(argv);
    Canvas *cv = canvas_of(self);
    if (!cv)
        return 0;

    cv->clip_x0 = cv->clip_y0 = 0;
    cv->clip_x1 = cv->width;
    cv->clip_y1 = cv->height;
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// canvas rgba -> the pixels, a String of width * height * 4 bytes
//------------------------------------------------------------------------
static ClayPluginValue canvas_rgba(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc);
    CLAY_UNUSED(argv);
    Canvas *cv = canvas_of(self);
    return cv ? api->make_bytes(cv->px, (size_t)cv->width * cv->height * 4) : 0;
}

static ClayPluginMethod canvas_methods[] = {
    { "_create", canvas_create },
    { "width", canvas_width },
    { "height", canvas_height },
    { "_fill", canvas_fill },
    { "_stroke", canvas_stroke },
    { "_circles", canvas_circles },
    { "_text", canvas_text },
    { "textWidth", canvas_text_width },
    { "_blit", canvas_blit },
    { "clip", canvas_clip },
    { "unclip", canvas_unclip },
    { "rgba", canvas_rgba },
    { NULL, NULL }
};

//------------------------------------------------------------------------
// Register CanvasCore (called from image_plugin_init)
//------------------------------------------------------------------------
void canvas_register(const ClayPluginAPI *clay_api) {
    api = clay_api;

    ClayPluginType def = { "CanvasCore", canvas_methods, canvas_free, sizeof(void *) };
    api->register_type(&def);
}
