/**
 * LimitlezzOS virtualized list — render only what's on screen.
 *
 * A long list (e.g. 250 heard nodes) would create thousands of LVGL objects
 * and exhaust the 96KB object pool, crashing the app. lz_vlist instead builds
 * only the rows in the viewport plus a small buffer (2 above / 2 below) and
 * recycles them as the list scrolls — rows that leave the window are deleted,
 * rows that enter are created. The full scroll height is preserved by a tall
 * content panel, so the scrollbar and touch momentum behave normally.
 *
 * Keyboard/trackball focus moves go through the normal screen rebuild (one
 * discrete step); only touch-drag/momentum recycles rows in place, so a fling
 * is never interrupted by destroying the container under the finger.
 */
#ifndef LZ_VLIST_H
#define LZ_VLIST_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build one row for `index` as a child of `content`. The callback must create
 * the row (parent = content), give it a fixed height, position it with
 * lv_obj_set_y(row, y), and RETURN it (so it can be recycled). `focused` = this
 * item currently holds the nav focus. */
typedef lv_obj_t *(*lz_vlist_row_cb)(lv_obj_t *content, int index, int y, bool focused, void *ctx);

/* Create a virtualized list inside scroll container `body`.
 *   header_h   : px reserved above item 0 for caller-drawn header rows
 *   count      : number of list items
 *   stride     : per-item vertical step in px (row height incl. the gap)
 *   focus_base : nav focus index of item 0 (header rows occupy 0..focus_base-1)
 *   cb, ctx    : row builder + opaque context
 * Returns the content panel; the caller draws header rows into it at y<header_h
 * (and calls lz_nav_track on them). */
lv_obj_t *lz_vlist(lv_obj_t *body, int header_h, int count, int stride,
                   int focus_base, lz_vlist_row_cb cb, void *ctx);

/* Reset the remembered scroll offset — call when entering a list screen fresh
 * so it starts at the top instead of where a previous list left off. */
void lz_vlist_reset_scroll(void);

/* Drop references to the scroll container — call right before the screen is
 * wiped (lz_rebuild), so a deferred reconcile can't touch a deleted body. */
void lz_vlist_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif
