/* Virtualized list — see vlist.h. Only the rows in the viewport (+2 buffer
 * above/below) exist as LVGL objects; the rest of the scroll height is held by
 * a tall, empty content panel so scrolling and momentum feel normal. */
#include "vlist.h"
#include "ui.h"
#include "theme.h"
#include <string.h>

#define VL_BUFFER 2          /* rows kept above and below the viewport */
#define VL_MAX    256        /* >= LZ_MAX_NODES; tracked rows */

static struct {
    lv_obj_t       *body;     /* scroll container */
    lv_obj_t       *content;  /* tall inner panel (absolute-positioned rows) */
    int             header_h;
    int             count;
    int             stride;
    int             focus_base;
    lz_vlist_row_cb cb;
    void           *ctx;
    lv_obj_t       *rows[VL_MAX];   /* rows[i] = object for item i, or NULL */
    int             win_first, win_last;  /* instantiated range, -1 = none */
} VL;

/* scroll offset remembered across full screen rebuilds (passive refreshes must
 * not yank the list back to the top). Reset on screen entry. */
static int g_saved_scroll = 0;
static bool g_pending = false;   /* a reconcile is queued (coalesces scroll bursts) */

void lz_vlist_reset_scroll(void) { g_saved_scroll = 0; }

/* called by lz_rebuild() before it wipes the screen: the body (and its scroll
 * handler) is about to be deleted, so drop our pointers and don't touch them. */
void lz_vlist_invalidate(void) { VL.body = NULL; VL.content = NULL; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* window of items to keep alive for a given scroll offset */
static void compute_window(int scroll_y, int *out_first, int *out_last)
{
    int view_h = lv_obj_get_height(VL.body);
    if(view_h <= 0) view_h = 200;
    int first = (scroll_y - VL.header_h) / VL.stride - VL_BUFFER;
    int last  = (scroll_y + view_h - VL.header_h) / VL.stride + VL_BUFFER;
    /* the focused row must exist so the focus ring and scroll-into-view work */
    int fi = S.focus - VL.focus_base;
    if(fi >= 0 && fi < VL.count) {
        if(fi < first) first = fi;
        if(fi > last)  last  = fi;
    }
    first = clampi(first, 0, VL.count - 1);
    last  = clampi(last,  0, VL.count - 1);
    if(last < first) last = first;
    *out_first = first;
    *out_last  = last;
}

static void build_row(int i)
{
    if(i < 0 || i >= VL.count || i >= VL_MAX) return;
    if(VL.rows[i]) return;
    int y = VL.header_h + i * VL.stride;
    bool focused = (S.focus == VL.focus_base + i);
    VL.rows[i] = VL.cb(VL.content, i, y, focused, VL.ctx);
}

/* delete rows that left the window, create rows that entered it — in place,
 * without touching the scroll container (so touch momentum is preserved) */
static void reconcile(int nf, int nl)
{
    for(int i = VL.win_first; i <= VL.win_last; i++) {
        if(i < 0 || i >= VL_MAX) continue;
        if((i < nf || i > nl) && VL.rows[i]) {
            lv_obj_del(VL.rows[i]);
            VL.rows[i] = NULL;
        }
    }
    for(int i = nf; i <= nl; i++) build_row(i);
    VL.win_first = nf;
    VL.win_last  = nl;
}

/* Reconcile runs deferred (lv_async_call): doing it inside the scroll event
 * could delete the row the input device is mid-gesture on. By the time this
 * fires the gesture state has settled, and reading the live scroll position
 * means a burst of scroll events collapses into one reconcile to the latest. */
static void vl_reconcile_async(void *p)
{
    (void)p;
    g_pending = false;
    if(!VL.body) return;          /* screen was rebuilt out from under us */
    int sy = lv_obj_get_scroll_y(VL.body);
    int nf, nl;
    compute_window(sy, &nf, &nl);
    if(nf != VL.win_first || nl != VL.win_last) reconcile(nf, nl);
}

static void vl_scroll_cb(lv_event_t *e)
{
    (void)e;
    g_saved_scroll = lv_obj_get_scroll_y(VL.body);
    if(!g_pending) { g_pending = true; lv_async_call(vl_reconcile_async, NULL); }
}

lv_obj_t *lz_vlist(lv_obj_t *body, int header_h, int count, int stride,
                   int focus_base, lz_vlist_row_cb cb, void *ctx)
{
    memset(&VL, 0, sizeof VL);
    g_pending = false;
    VL.body = body;
    VL.header_h = header_h;
    VL.count = count;
    VL.stride = stride;
    VL.focus_base = focus_base;
    VL.cb = cb;
    VL.ctx = ctx;
    VL.win_first = 0;
    VL.win_last = -1;

    int content_h = header_h + (count > 0 ? count * stride : 0);

    lv_obj_t *content = lz_box(body);
    VL.content = content;
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_height(content, content_h);

    lv_obj_update_layout(body);
    int view_h = lv_obj_get_height(body);
    if(view_h <= 0) view_h = 200;
    int max_scroll = content_h - view_h;
    if(max_scroll < 0) max_scroll = 0;

    /* start from the remembered offset, then nudge so the focused item shows */
    int sc = clampi(g_saved_scroll, 0, max_scroll);
    int fi = S.focus - focus_base;
    if(fi >= 0 && fi < count) {
        int top = header_h + fi * stride;
        int bot = top + stride;
        if(top < sc)               sc = top;
        else if(bot > sc + view_h) sc = bot - view_h;
        sc = clampi(sc, 0, max_scroll);
    }

    int nf, nl;
    /* compute_window reads S.focus; temporarily evaluate at the chosen offset */
    {
        int saved = g_saved_scroll;
        g_saved_scroll = sc;
        compute_window(sc, &nf, &nl);
        g_saved_scroll = saved;
    }
    for(int i = nf; i <= nl; i++) build_row(i);
    VL.win_first = nf;
    VL.win_last  = nl;

    lv_obj_add_event_cb(body, vl_scroll_cb, LV_EVENT_SCROLL, NULL);
    if(sc > 0) lv_obj_scroll_to_y(body, sc, LV_ANIM_OFF);
    g_saved_scroll = sc;

    return content;
}
