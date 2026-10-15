#ifndef GENESIS_HIRATE_H
#define GENESIS_HIRATE_H

/* High-refresh presentation: frames between two guest ticks, drawn at 2x so
 * half-pixel scroll and sprite motion is visible. Renders from private copies
 * of captured VDP state; the live machine is never touched. */

#include <stdint.h>
#include "genesis_vdp.h"

void hirate_set_enabled(int on);
int  hirate_enabled(void);

/* Record the state that produced the frame just rendered. `palette` maps the
 * renderer's 256 output indices to ARGB. */
void hirate_capture(const GVDP *v, const uint32_t *palette);

/* Forget both captures (state load, reset). */
void hirate_reset(void);

/* Owner of one SAT entry: a stable object id (0 = unknown) and the object's
 * screen position, which moves all pieces of the object together. */
typedef struct HirateSpriteRef {
    uint32_t id;
    int16_t  x, y;
} HirateSpriteRef;

/* Owners for the SAT that the next capture shows, in link order. Without
 * owners, sprites pair by link position, tile and size. */
void hirate_set_sprite_refs(const HirateSpriteRef *refs, int count);

/* Per-color transform applied to both captured palettes before they blend. */
typedef uint32_t (*HirateColorMap)(uint32_t argb);

/* Draw the frame at `alpha` (0 = previous capture, 1 = current) into `out`
 * (ARGB, row stride `stride` pixels): 2*w x 2*h pixels. `map` may be NULL.
 * Returns 0 when no interpolation applies; the caller then presents the
 * real frame. */
int hirate_render(double alpha, HirateColorMap map, uint32_t *out, int stride, int *w, int *h);

#endif
