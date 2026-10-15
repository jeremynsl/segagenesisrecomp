#include "hirate.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Largest per-tick motion that interpolates; larger jumps are cuts. */
#define HIRATE_SNAP_PX 16

typedef struct Capture {
    GVDP            vdp;
    uint32_t        pal[GVDP_OUTPUT_PALETTE_SIZE];
    int             valid;
    int             raster_dirty;
    int             n;                          /* SAT entries in link order */
    uint16_t        sat_off[GVDP_SPRITES_MAX];  /* VRAM offset of each entry */
    int             has_refs;
    HirateSpriteRef ref[GVDP_SPRITES_MAX];
} Capture;

static int s_enabled;
static Capture s_cap[2];
static int s_cur;
static GVDP s_work;
static uint32_t s_pal[GVDP_OUTPUT_PALETTE_SIZE];
static HirateSpriteRef s_pending_ref[GVDP_SPRITES_MAX];
static int s_pending_n = -1;

void hirate_set_enabled(int on) { s_enabled = !!on; if (!on) hirate_reset(); }
int  hirate_enabled(void) { return s_enabled; }

void hirate_reset(void)
{
    s_cap[0].valid = s_cap[1].valid = 0;
    s_pending_n = -1;
}

void hirate_set_sprite_refs(const HirateSpriteRef *refs, int count)
{
    if (count < 0) count = 0;
    if (count > GVDP_SPRITES_MAX) count = GVDP_SPRITES_MAX;
    memcpy(s_pending_ref, refs, (size_t)count * sizeof(*refs));
    s_pending_n = count;
}

static uint16_t rd16(const GVDP *v, unsigned a)
{
    return (uint16_t)((v->vram[a & 0xFFFF] << 8) | v->vram[(a + 1) & 0xFFFF]);
}

static void wr16(GVDP *v, unsigned a, uint16_t w)
{
    v->vram[a & 0xFFFF] = (uint8_t)(w >> 8);
    v->vram[(a + 1) & 0xFFFF] = (uint8_t)w;
}

static int plane_px(unsigned code2) { return (code2 & 3) == 1 ? 512 : (code2 & 3) == 3 ? 1024 : 256; }
static unsigned hscroll_base(const GVDP *v) { return (unsigned)(v->reg[0x0D] & 0x3F) << 10; }
static int full_screen_scroll(const GVDP *v) { return (v->reg[0x0B] & 0x07) == 0; }
static int interlace2(const GVDP *v) { return (v->reg[0x0C] & 0x06) == 0x06; }

static void walk_sat(Capture *c)
{
    unsigned base = (unsigned)(c->vdp.reg[0x05] & 0x7F) << 9;
    int max = (c->vdp.reg[0x0C] & 0x81) ? 80 : 64;
    int link = 0;
    c->n = 0;
    while (c->n < max) {
        unsigned e = base + (unsigned)link * 8;
        c->sat_off[c->n++] = (uint16_t)e;
        link = c->vdp.vram[(e + 3) & 0xFFFF] & 0x7F;
        if (link == 0 || link >= max) break;
    }
}

void hirate_capture(const GVDP *v, const uint32_t *palette)
{
    int dirty = gvdp_take_raster_write();
    if (!s_enabled) return;
    s_cur ^= 1;
    Capture *c = &s_cap[s_cur];
    memcpy(&c->vdp, v, sizeof(*v));
    memcpy(c->pal, palette, sizeof(c->pal));
    c->raster_dirty = dirty;
    walk_sat(c);
    c->has_refs = s_pending_n == c->n;
    if (c->has_refs) memcpy(c->ref, s_pending_ref, (size_t)c->n * sizeof(c->ref[0]));
    s_pending_n = -1;
    c->valid = 1;
}

/* Signed difference on a ring of `size` (a power of two); 0 past the snap. */
static int ring_delta(int cur, int prev, int size)
{
    int d = (cur - prev) & (size - 1);
    if (d >= size / 2) d -= size;
    return (d > HIRATE_SNAP_PX || d < -HIRATE_SNAP_PX) ? 0 : d;
}

static int clamp_delta(int d)
{
    return (d > HIRATE_SNAP_PX || d < -HIRATE_SNAP_PX) ? 0 : d;
}

/* Offset in half-pixels of content at time t (<= 0) after moving `d` px. */
static int half_steps(double t, int d) { return (int)lround(2.0 * t * d); }

/* Whole-pixel shift for output parity p under a k half-pixel offset. */
static int parity_shift(int p, int k)
{
    int n = p - k;
    int fl = n >= 0 ? n / 2 : -((-n + 1) / 2);
    return -fl;
}

static int sprite_tile(const GVDP *v, unsigned e) { return rd16(v, e + 4) & 0x07FF; }

/* Same tile and size: entries that could be one sprite across captures. */
static int same_look(const Capture *c, unsigned ce, const Capture *p, unsigned pe)
{
    return sprite_tile(&c->vdp, ce) == sprite_tile(&p->vdp, pe) &&
           c->vdp.vram[(ce + 2) & 0xFFFF] == p->vdp.vram[(pe + 2) & 0xFFFF];
}

/* Screen motion of curr SAT entry i since the previous capture. */
static void sprite_delta(const Capture *c, const Capture *p, int i, int *dx, int *dy)
{
    *dx = *dy = 0;
    if (c->has_refs && p->has_refs) {
        uint32_t id = c->ref[i].id;
        if (!id) return;
        for (int j = 0; j < p->n; j++) {
            if (p->ref[j].id != id) continue;
            *dx = clamp_delta(c->ref[i].x - p->ref[j].x);
            *dy = clamp_delta(c->ref[i].y - p->ref[j].y);
            return;
        }
        return;
    }
    if (i >= p->n) return;
    unsigned ce = c->sat_off[i], pe = p->sat_off[i];
    if (!same_look(c, ce, p, pe)) return;
    int cx = rd16(&c->vdp, ce + 6) & 0x3FF, cy = rd16(&c->vdp, ce) & 0x3FF;
    int ddx = cx - (rd16(&p->vdp, pe + 6) & 0x3FF), ddy = cy - (rd16(&p->vdp, pe) & 0x3FF);
    /* A look-alike at least as close makes the pair ambiguous; refuse it. */
    for (int j = 0; j < p->n; j++) {
        unsigned oe = p->sat_off[j];
        if (j == i || !same_look(c, ce, p, oe)) continue;
        int ox = cx - (rd16(&p->vdp, oe + 6) & 0x3FF), oy = cy - (rd16(&p->vdp, oe) & 0x3FF);
        if (abs(ox) + abs(oy) <= abs(ddx) + abs(ddy)) return;
    }
    *dx = clamp_delta(ddx);
    *dy = clamp_delta(ddy);
}

static uint32_t lerp_argb(uint32_t a, uint32_t b, double t)
{
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        int ca = (int)((a >> s) & 0xFF), cb = (int)((b >> s) & 0xFF);
        out |= (uint32_t)(ca + (int)lround((cb - ca) * t)) << s;
    }
    return out;
}

int hirate_render(double alpha, HirateColorMap map, uint32_t *out, int stride, int *w, int *h)
{
    const Capture *c = &s_cap[s_cur], *p = &s_cap[s_cur ^ 1];
    if (!s_enabled || !c->valid || !p->valid || c->raster_dirty) return 0;
    if (interlace2(&c->vdp)) return 0;
    if (alpha > 1.0) alpha = 1.0;
    if (alpha < 0.0) alpha = 0.0;
    double t = alpha - 1.0;

    /* Plane offsets in half-pixels: [plane A, plane B][x, y]. */
    int kp[2][2] = {{0, 0}, {0, 0}};
    if (full_screen_scroll(&c->vdp) && full_screen_scroll(&p->vdp)) {
        int pw = plane_px(c->vdp.reg[0x10]), ph = plane_px(c->vdp.reg[0x10] >> 4);
        unsigned hc = hscroll_base(&c->vdp), hp = hscroll_base(&p->vdp);
        for (int l = 0; l < 2; l++) {
            int dx = ring_delta(rd16(&c->vdp, hc + 2u * l), rd16(&p->vdp, hp + 2u * l), pw);
            int dy = -ring_delta(c->vdp.vsram[l], p->vdp.vsram[l], ph);
            kp[l][0] = half_steps(t, dx);
            kp[l][1] = half_steps(t, dy);
        }
    }
    int ks[GVDP_SPRITES_MAX][2];
    for (int i = 0; i < c->n; i++) {
        int dx, dy;
        sprite_delta(c, p, i, &dx, &dy);
        ks[i][0] = half_steps(t, dx);
        ks[i][1] = half_steps(t, dy);
    }

    for (int i = 0; i < GVDP_OUTPUT_PALETTE_SIZE; i++) {
        uint32_t cc = map ? map(c->pal[i]) : c->pal[i];
        uint32_t pc = map ? map(p->pal[i]) : p->pal[i];
        s_pal[i] = cc == pc ? cc : lerp_argb(pc, cc, alpha);
    }

    int height = gvdp_screen_height(&c->vdp), width = 0;
    unsigned hbase = hscroll_base(&c->vdp);
    static uint8_t idx[GVDP_MAX_WIDTH];
    for (int py = 0; py < 2; py++) {
        for (int px = 0; px < 2; px++) {
            memcpy(&s_work, &c->vdp, sizeof(s_work));
            for (int l = 0; l < 2; l++) {
                int sx = parity_shift(px, kp[l][0]), sy = parity_shift(py, kp[l][1]);
                uint16_t hs = rd16(&s_work, hbase + 2u * l);
                wr16(&s_work, hbase + 2u * l, (uint16_t)((hs & ~0x3FFu) | ((hs + sx) & 0x3FF)));
                s_work.vsram[l] = (uint16_t)((s_work.vsram[l] - sy) & 0x3FF);
            }
            for (int i = 0; i < c->n; i++) {
                unsigned e = c->sat_off[i];
                int sx = parity_shift(px, ks[i][0]), sy = parity_shift(py, ks[i][1]);
                if (!sx && !sy) continue;
                uint16_t y = rd16(&s_work, e), x = rd16(&s_work, e + 6);
                wr16(&s_work, e, (uint16_t)((y & ~0x3FFu) | ((y + sy) & 0x3FF)));
                wr16(&s_work, e + 6, (uint16_t)((x & ~0x3FFu) | ((x + sx) & 0x3FF)));
            }
            for (int line = 0; line < height; line++) {
                int n = gvdp_render_scanline(&s_work, line, idx);
                uint32_t *row = out + (size_t)(2 * line + py) * (size_t)stride + px;
                for (int x = 0; x < n; x++) row[2 * x] = s_pal[idx[x]];
                width = n;
            }
        }
    }
    *w = width;
    *h = height;
    return 1;
}
