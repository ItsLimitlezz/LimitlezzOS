#include "app_permissions.h"
#include "mesh.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t bit;
    const char *name;
    const char *prompt;
} lz_app_permission_desc_t;

static const lz_app_permission_desc_t LZ_APP_PERMS[] = {
    { LZ_APP_PERM_DISPLAY,       "display",       "show content on screen" },
    { LZ_APP_PERM_INPUT,         "input",         "use buttons, trackball, or keyboard while open" },
    { LZ_APP_PERM_STORAGE,       "storage",       "read and write this app's own data" },
    { LZ_APP_PERM_MESH_READ,     "mesh_read",     "read mesh messages routed to app APIs" },
    { LZ_APP_PERM_MESH_SEND,     "mesh_send",     "send mesh messages through OS services" },
    { LZ_APP_PERM_SYSTEM_TIME,   "system_time",   "read the device clock" },
    { LZ_APP_PERM_BATTERY,       "battery",       "read battery level" },
    { LZ_APP_PERM_NOTIFICATIONS, "notifications", "ask the OS to show notifications" },
    { LZ_APP_PERM_NETWORK_WIFI,  "network_wifi",  "use Wi-Fi through OS network services" },
};

static void append_text(char *out, size_t cap, const char *text)
{
    if(!out || cap == 0 || !text) return;
    size_t len = strlen(out);
    if(len >= cap - 1) {
        out[cap - 1] = 0;
        return;
    }
    snprintf(out + len, cap - len, "%s", text);
}

static const lz_app_permission_desc_t *find_perm(uint16_t bit)
{
    for(unsigned i = 0; i < sizeof(LZ_APP_PERMS) / sizeof(LZ_APP_PERMS[0]); i++) {
        if(LZ_APP_PERMS[i].bit == bit) return &LZ_APP_PERMS[i];
    }
    return NULL;
}

const char *lz_app_permission_name(uint16_t bit)
{
    const lz_app_permission_desc_t *p = find_perm(bit);
    return p ? p->name : NULL;
}

const char *lz_app_permission_prompt(uint16_t bit)
{
    const lz_app_permission_desc_t *p = find_perm(bit);
    return p ? p->prompt : NULL;
}

uint16_t lz_app_permission_known_mask(void)
{
    uint16_t mask = 0;
    for(unsigned i = 0; i < sizeof(LZ_APP_PERMS) / sizeof(LZ_APP_PERMS[0]); i++)
        mask |= LZ_APP_PERMS[i].bit;
    return mask;
}

void lz_app_permissions_list(uint16_t perms, char *out, size_t cap)
{
    if(!out || cap == 0) return;
    out[0] = 0;

    bool any = false;
    for(unsigned i = 0; i < sizeof(LZ_APP_PERMS) / sizeof(LZ_APP_PERMS[0]); i++) {
        if((perms & LZ_APP_PERMS[i].bit) == 0) continue;
        if(any) append_text(out, cap, ", ");
        append_text(out, cap, LZ_APP_PERMS[i].name);
        any = true;
    }
    if(perms & (uint16_t)~lz_app_permission_known_mask()) {
        if(any) append_text(out, cap, ", ");
        append_text(out, cap, "unknown");
        any = true;
    }
    if(!any) snprintf(out, cap, "none");
}

void lz_app_permissions_summary(uint16_t perms, char *out, size_t cap)
{
    if(!out || cap == 0) return;
    out[0] = 0;
    if(perms == 0) {
        snprintf(out, cap, "No app permissions requested.");
        return;
    }

    bool any = false;
    for(unsigned i = 0; i < sizeof(LZ_APP_PERMS) / sizeof(LZ_APP_PERMS[0]); i++) {
        if((perms & LZ_APP_PERMS[i].bit) == 0) continue;
        append_text(out, cap, any ? "; " : "Can ");
        append_text(out, cap, LZ_APP_PERMS[i].prompt);
        any = true;
    }
    if(any) append_text(out, cap, ".");

    if(perms & (uint16_t)~lz_app_permission_known_mask()) {
        if(any) append_text(out, cap, " ");
        append_text(out, cap, "Unknown app access request.");
        any = true;
    }
    if(!any) snprintf(out, cap, "No app permissions requested.");
}
