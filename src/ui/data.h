/**
 * Mock data model — exact sample data from the design handoff.
 * This is the seam where the real Messaging service / protocol stacks
 * plug in later (spec phases 1.6 / 2.3).
 */
#ifndef LZ_DATA_H
#define LZ_DATA_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LZ_NET_MT, LZ_NET_MC } lz_net_t;

typedef struct {
    const char *id, *name, *icon, *sub;
    int hue;                 /* -1 = neutral graphite */
} lz_app_t;

typedef struct {
    const char *id, *name;
    lz_net_t net;
    const char *addr, *text, *t;
    int unread;
    const char *path;
} lz_thread_t;

typedef struct {
    bool self;
    const char *text;
} lz_msg_t;

typedef struct {
    const char *id, *name;
    lz_net_t net;
    const char *icon, *sub, *text, *t;
} lz_chan_t;

typedef struct {
    const char *name, *shortcode;
    lz_net_t net;
    const char *id, *role;
    float snr;               /* NAN = none */
    int batt;                /* -1 = none */
    const char *snr_s, *last, *hw, *dist;
} lz_node_t;

typedef struct {
    const char *label, *value;
    int pct;                 /* bar fill %; bar color chosen by row index */
} lz_sys_stat_t;

extern const lz_app_t       LZ_APPS[8];
extern const lz_thread_t    LZ_THREADS[6];
extern const lz_msg_t       LZ_MSGS_AVA[5];
extern const lz_msg_t       LZ_MSGS_DMITRI[4];
extern const lz_chan_t      LZ_CHANS[4];
extern const lz_node_t      LZ_NODES[9];
extern const char          *LZ_TERM_LINES[12];
extern const int            LZ_TERM_KIND[12]; /* 0 dim, 1 cmd, 2 out */
extern const lz_sys_stat_t  LZ_SYS_STATS[5];

lv_color_t lz_tile_color(int hue);
const char *lz_app_icon_glyph(const char *token);
lv_color_t lz_net_color(lz_net_t n);
lv_color_t lz_av_color(lz_net_t n);
const char *lz_net_name(lz_net_t n);
lv_color_t lz_snr_color(float snr);

#ifdef __cplusplus
}
#endif

#endif
