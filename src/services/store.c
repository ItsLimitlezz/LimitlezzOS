/**
 * Persistent message store — long-term chat history that stays fast.
 *
 * Layout (under the data dir; SD card on the T-Deck, ./lzdata in the sim):
 *   threads.idx        one line per thread (tiny, rewritten atomically)
 *   m_<addr>.log       per-conversation append-only binary log
 *   nodes.db           one line per heard node (rewritten on change)
 *
 * Design: appends are O(1) — a message write touches one file, once. The
 * UI never reads whole histories: opening a conversation streams the file
 * once and keeps only the last LZ_TAIL_MAX messages in a ring. The thread
 * index carries everything list screens need (last line, unread, time) so
 * the Messages screen renders without opening any log.
 *
 * Uses POSIX stdio so the exact same code runs in the simulator and on
 * ESP32 (Arduino mounts SD/LittleFS onto the VFS). datadir == NULL keeps
 * everything RAM-only.
 */
#include "mesh.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <errno.h>
#endif

#if !defined(S_IFMT) && defined(_S_IFMT)
#define S_IFMT _S_IFMT
#endif
#if !defined(S_IFDIR) && defined(_S_IFDIR)
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

static char g_dir[96];
static bool g_persist;

void lz_store_init(const char *datadir)
{
    g_persist = datadir && datadir[0];
    if(g_persist) {
        snprintf(g_dir, sizeof g_dir, "%s", datadir);
    }
}

const char *lz_store_file_root(void)
{
    if(!g_persist) return NULL;
    if(strcmp(g_dir, "/sd") == 0 || strncmp(g_dir, "/sd/", 4) == 0) return "/sd";
    if(strcmp(g_dir, "/appfs") == 0 || strncmp(g_dir, "/appfs/", 7) == 0) return "/appfs";
    return g_dir;  /* simulator/local POSIX data directory */
}

static void path_for(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", g_dir, name);
}

static void path_join(char *out, size_t n, const char *base, const char *name)
{
    size_t bl = strlen(base);
    snprintf(out, n, "%s%s%s", base, (bl && base[bl - 1] == '/') ? "" : "/", name);
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static bool path_mkdir(const char *path)
{
    if(path_is_dir(path)) return true;
#ifdef _WIN32
    return _mkdir(path) == 0 || path_is_dir(path);
#else
    return mkdir(path, 0775) == 0 || errno == EEXIST || path_is_dir(path);
#endif
}

/* ---- local app manifests ----
 *
 * First V0.95 app-platform increment: discover installable local app packages
 * without a VM/runtime yet. Each package is:
 *
 *   apps/<id>/manifest.json
 *   apps/<id>/<entry>
 *
 * The manifest parser intentionally accepts a tiny top-level JSON subset so it
 * stays deterministic on ESP32: string fields, integer hue, and a bounded
 * permissions string array; no allocation, no recursion, and bounded file size.
 */

static const char *skip_ws(const char *p)
{
    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char *json_value_for(const char *json, const char *key)
{
    char needle[36];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = json;
    size_t nl = strlen(needle);
    while((p = strstr(p, needle)) != NULL) {
        const char *q = skip_ws(p + nl);
        if(*q == ':') return skip_ws(q + 1);
        p = q;
    }
    return NULL;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t n)
{
    if(!out || n == 0) return false;
    const char *p = json_value_for(json, key);
    if(!p || *p != '"') return false;
    p++;
    size_t j = 0;
    while(*p && *p != '"') {
        char c = *p++;
        if(c == '\\' && *p) {
            char e = *p++;
            if(e == 'n') c = '\n';
            else if(e == 'r') c = '\r';
            else if(e == 't') c = '\t';
            else c = e;
        }
        if(j + 1 < n && c >= 32) out[j++] = c;
    }
    if(*p != '"') return false;
    out[j] = 0;
    return j > 0;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    const char *p = json_value_for(json, key);
    if(!p) return false;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if(end == p) return false;
    *out = (int)v;
    return true;
}

static uint16_t app_permission_bit(const char *name)
{
    if(strcmp(name, "display") == 0) return LZ_APP_PERM_DISPLAY;
    if(strcmp(name, "input") == 0) return LZ_APP_PERM_INPUT;
    if(strcmp(name, "storage") == 0) return LZ_APP_PERM_STORAGE;
    if(strcmp(name, "mesh_read") == 0) return LZ_APP_PERM_MESH_READ;
    if(strcmp(name, "mesh_send") == 0) return LZ_APP_PERM_MESH_SEND;
    if(strcmp(name, "system_time") == 0) return LZ_APP_PERM_SYSTEM_TIME;
    if(strcmp(name, "battery") == 0) return LZ_APP_PERM_BATTERY;
    if(strcmp(name, "notifications") == 0) return LZ_APP_PERM_NOTIFICATIONS;
    if(strcmp(name, "network_wifi") == 0) return LZ_APP_PERM_NETWORK_WIFI;
    return 0;
}

static bool json_parse_permissions_value(const char *p, uint16_t *out)
{
    if(!p || !out) return false;
    p = skip_ws(p);
    if(*p != '[') return false;
    p = skip_ws(p + 1);
    uint16_t bits = 0;
    if(*p == ']') { *out = 0; return true; }
    for(;;) {
        if(*p != '"') return false;
        p++;
        char name[24];
        size_t j = 0;
        while(*p && *p != '"') {
            char c = *p++;
            if(c == '\\' && *p) c = *p++;
            if(j + 1 < sizeof name && c >= 32) name[j++] = c;
        }
        if(*p != '"' || j == 0) return false;
        name[j] = 0;
        uint16_t bit = app_permission_bit(name);
        if(!bit) return false;
        bits |= bit;
        p = skip_ws(p + 1);
        if(*p == ',') { p = skip_ws(p + 1); continue; }
        if(*p == ']') { *out = bits; return true; }
        return false;
    }
}

static bool api_version_supported(const char *v)
{
    return strcmp(v, "0.1") == 0 || strcmp(v, "0.1.0") == 0;
}

static bool safe_id(const char *s)
{
    if(!s || !s[0]) return false;
    for(int i = 0; s[i]; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if(!ok) return false;
    }
    return true;
}

static bool safe_entry(const char *s)
{
    if(!s || !s[0] || s[0] == '/' || s[0] == '\\') return false;
    if(strstr(s, "..")) return false;
    for(int i = 0; s[i]; i++) {
        char c = s[i];
        if(c == '\\' || c == ':' || c < 32) return false;
    }
    return true;
}

static bool local_app_seen(const lz_local_app_t *out, int n, const char *id)
{
    for(int i = 0; i < n; i++)
        if(strcmp(out[i].id, id) == 0) return true;
    return false;
}

static bool manifest_fail(char *reason, size_t cap, const char *msg)
{
    if(reason && cap > 0) snprintf(reason, cap, "%s", msg);
    return false;
}

static bool load_app_manifest(const char *pkg_dir, lz_local_app_t *app,
                              char *reason, size_t reason_cap)
{
    char manifest[136];
    path_join(manifest, sizeof manifest, pkg_dir, "manifest.json");
    FILE *f = fopen(manifest, "rb");
    if(!f) return manifest_fail(reason, reason_cap, "missing manifest");

    char json[1537];
    size_t n = fread(json, 1, sizeof json - 1, f);
    fclose(f);
    json[n] = 0;
    if(n == 0) return manifest_fail(reason, reason_cap, "empty manifest");
    if(n >= sizeof json - 1) return manifest_fail(reason, reason_cap, "manifest too large");

    memset(app, 0, sizeof *app);
    app->hue = -1;
    snprintf(app->version, sizeof app->version, "0.0.0");
    snprintf(app->author, sizeof app->author, "local");
    snprintf(app->api_version, sizeof app->api_version, "0.1");
    snprintf(app->icon, sizeof app->icon, "description");
    app->permissions = LZ_APP_PERM_DISPLAY | LZ_APP_PERM_INPUT;

    if(!json_get_string(json, "id", app->id, sizeof app->id))
        return manifest_fail(reason, reason_cap, "missing id");
    if(!json_get_string(json, "name", app->name, sizeof app->name))
        return manifest_fail(reason, reason_cap, "missing name");
    if(!json_get_string(json, "entry", app->entry, sizeof app->entry))
        return manifest_fail(reason, reason_cap, "missing entry");
    json_get_string(json, "version", app->version, sizeof app->version);
    json_get_string(json, "author", app->author, sizeof app->author);
    json_get_string(json, "api_version", app->api_version, sizeof app->api_version);
    if(!json_get_string(json, "summary", app->summary, sizeof app->summary))
        json_get_string(json, "description", app->summary, sizeof app->summary);
    json_get_string(json, "icon", app->icon, sizeof app->icon);
    json_get_int(json, "hue", &app->hue);

    const char *perms = json_value_for(json, "permissions");
    if(perms && !json_parse_permissions_value(perms, &app->permissions))
        return manifest_fail(reason, reason_cap, "bad permissions");

    if(!safe_id(app->id)) return manifest_fail(reason, reason_cap, "unsafe id");
    if(!safe_entry(app->entry)) return manifest_fail(reason, reason_cap, "unsafe entry");
    if(!api_version_supported(app->api_version))
        return manifest_fail(reason, reason_cap, "unsupported SDK");
    if(app->hue < -1 || app->hue > 359) app->hue = -1;

    char entry_path[160];
    path_join(entry_path, sizeof entry_path, pkg_dir, app->entry);
    if(!path_is_file(entry_path))
        return manifest_fail(reason, reason_cap, "missing entry file");

    snprintf(app->path, sizeof app->path, "%s", pkg_dir);
    return true;
}

static void scan_app_root(const char *apps_dir, lz_local_app_t *out, int cap, int *count)
{
    if(!apps_dir || !out || !count || *count >= cap) return;
    DIR *d = opendir(apps_dir);
    if(!d) return;

    struct dirent *e;
    while(*count < cap && (e = readdir(d)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char pkg[128];
        path_join(pkg, sizeof pkg, apps_dir, e->d_name);
        if(!path_is_dir(pkg)) continue;

        lz_local_app_t app;
        if(!load_app_manifest(pkg, &app, NULL, 0)) continue;
        if(local_app_seen(out, *count, app.id)) continue;
        out[(*count)++] = app;
    }
    closedir(d);
}

int lz_store_scan_apps(lz_local_app_t *out, int cap)
{
    if(!g_persist || !out || cap <= 0) return 0;
    int count = 0;

    char dir[128];
    path_join(dir, sizeof dir, g_dir, "apps");
    scan_app_root(dir, out, cap, &count);

    const char *root = lz_store_file_root();
    if(root && strcmp(root, g_dir) != 0 && count < cap) {
        path_join(dir, sizeof dir, root, "apps");
        scan_app_root(dir, out, cap, &count);
    }

    if(count < cap)
        scan_app_root("/appfs/apps", out, cap, &count);

    return count;
}

static void scan_app_issue_root(const char *apps_dir, lz_local_app_issue_t *out, int cap, int *count)
{
    if(!apps_dir || !out || !count || *count >= cap) return;
    DIR *d = opendir(apps_dir);
    if(!d) return;

    struct dirent *e;
    while(*count < cap && (e = readdir(d)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char pkg[128];
        path_join(pkg, sizeof pkg, apps_dir, e->d_name);
        if(!path_is_dir(pkg)) continue;

        lz_local_app_t app;
        char reason[48] = "invalid manifest";
        if(load_app_manifest(pkg, &app, reason, sizeof reason)) continue;

        lz_local_app_issue_t *issue = &out[(*count)++];
        memset(issue, 0, sizeof *issue);
        snprintf(issue->package, sizeof issue->package, "%s", e->d_name);
        snprintf(issue->reason, sizeof issue->reason, "%s", reason);
        snprintf(issue->path, sizeof issue->path, "%s", pkg);
    }
    closedir(d);
}

int lz_store_scan_app_issues(lz_local_app_issue_t *out, int cap)
{
    if(!g_persist || !out || cap <= 0) return 0;
    int count = 0;

    char dir[128];
    path_join(dir, sizeof dir, g_dir, "apps");
    scan_app_issue_root(dir, out, cap, &count);

    const char *root = lz_store_file_root();
    if(root && strcmp(root, g_dir) != 0 && count < cap) {
        path_join(dir, sizeof dir, root, "apps");
        scan_app_issue_root(dir, out, cap, &count);
    }

    if(count < cap)
        scan_app_issue_root("/appfs/apps", out, cap, &count);

    return count;
}

bool lz_store_prepare_app_data(const lz_local_app_t *app, char *path_out, int path_cap,
                               char *err, int err_cap)
{
    if(path_out && path_cap > 0) path_out[0] = 0;
    if(err && err_cap > 0) err[0] = 0;
    if(!g_persist) {
        if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "storage unavailable");
        return false;
    }
    if(!app || !app->path[0] || !path_is_dir(app->path)) {
        if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "package missing");
        return false;
    }
    char data[128];
    path_join(data, sizeof data, app->path, "data");
    if(!path_mkdir(data) || !path_is_dir(data)) {
        if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "data mkdir failed");
        return false;
    }
    if(path_out && path_cap > 0) snprintf(path_out, (size_t)path_cap, "%s", data);
    return true;
}

/* addr strings can contain '!' etc — keep alnum only in filenames */
static void log_name(char *out, size_t n, const char *addr)
{
    char clean[16];
    int j = 0;
    for(int i = 0; addr[i] && j < 12; i++) {
        char c = addr[i];
        if((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            clean[j++] = c;
    }
    clean[j] = 0;
    snprintf(out, n, "m_%s.log", clean);
}

/* ---- per-thread logs ----
 * v1: [u8 self][u32 ts][u16 len][text]
 * v2: [u8 self][u32 ts][u16 len|0x8000][u8 status][u32 pkt_id][text]
 * v3: [u8 self][u32 ts][u16 len|0xc000][u8 status][u32 pkt_id]
 *     [u8 retries][u8 fail_reason][text]
 *
 * Text is capped below 0x4000 bytes, so the top two bits can mark records
 * that carry sent-DM delivery metadata.
 */
#define LZ_LOG_EXTENDED 0x8000u
#define LZ_LOG_DELIVERY_META 0x4000u
#define LZ_LOG_LEN_MASK 0x3FFFu

void lz_store_append(const char *addr, const lz_msg_rt *m)
{
    if(!g_persist) return;
    char name[24], path[128];
    log_name(name, sizeof name, addr);
    path_for(path, sizeof path, name);
    FILE *f = fopen(path, "ab");
    if(!f) return;

    /* Pack the whole record [u8 self][u32 ts][u16 len][text] and write it in a
     * single fwrite: a short write then leaves at most a clean truncation at
     * EOF (which load_tail tolerates) rather than a half-record that desyncs
     * every following message. */
    uint16_t len = (uint16_t)strlen(m->text);
    if(len > LZ_TEXT_MAX) len = LZ_TEXT_MAX;
    bool extended = m->self && (m->status != LZ_MSG_NONE || m->pkt_id != 0);
    bool meta = extended;       /* reserve in-place update bytes for new DMs */
    uint16_t len_field = len | (extended ? LZ_LOG_EXTENDED : 0)
                             | (meta ? LZ_LOG_DELIVERY_META : 0);
    uint8_t rec[1 + 4 + 2 + 1 + 4 + 2 + LZ_TEXT_MAX];
    size_t n = 0;
    rec[n++] = m->self ? 1 : 0;
    memcpy(rec + n, &m->ts, 4); n += 4;
    memcpy(rec + n, &len_field, 2); n += 2;
    if(extended) {
        rec[n++] = m->status;
        memcpy(rec + n, &m->pkt_id, 4); n += 4;
        if(meta) {
            rec[n++] = m->retries;
            rec[n++] = m->fail_reason;
        }
    }
    memcpy(rec + n, m->text, len); n += len;
    fwrite(rec, 1, n, f);       /* one record, one write */
    fclose(f);
}

/* stream the log once, keep the last `cap` records in the caller's ring;
 * returns count stored (ring is in arrival order) */
int lz_store_load_tail(const char *addr, lz_msg_rt *ring, int cap)
{
    if(!g_persist) return 0;
    char name[24], path[128];
    log_name(name, sizeof name, addr);
    path_for(path, sizeof path, name);
    FILE *f = fopen(path, "rb");
    if(!f) return 0;

    int head = 0, count = 0;
    for(;;) {
        uint8_t self;
        uint32_t ts;
        uint16_t len_field;
        if(fread(&self, 1, 1, f) != 1) break;
        if(fread(&ts, 4, 1, f) != 1) break;
        if(fread(&len_field, 2, 1, f) != 1) break;
        bool extended = (len_field & LZ_LOG_EXTENDED) != 0;
        bool meta = (len_field & LZ_LOG_DELIVERY_META) != 0;
        uint16_t len = len_field & LZ_LOG_LEN_MASK;
        uint8_t status = LZ_MSG_NONE;
        uint8_t retries = 0;
        uint8_t fail_reason = LZ_FAIL_NONE;
        uint32_t pkt_id = 0;
        if(extended) {
            if(fread(&status, 1, 1, f) != 1) break;
            if(fread(&pkt_id, 4, 1, f) != 1) break;
            if(meta) {
                if(fread(&retries, 1, 1, f) != 1) break;
                if(fread(&fail_reason, 1, 1, f) != 1) break;
            }
        }
        lz_msg_rt *slot = &ring[head];
        head = (head + 1) % cap;
        if(count < cap) count++;
        memset(slot, 0, sizeof *slot);
        slot->self = self != 0;
        slot->ts = ts;
        slot->status = status;
        slot->retries = retries;
        slot->fail_reason = fail_reason;
        slot->pkt_id = pkt_id;
        size_t rd = len < LZ_TEXT_MAX - 1 ? len : LZ_TEXT_MAX - 1;
        if(fread(slot->text, 1, rd, f) != rd) { count--; break; }
        slot->text[rd] = 0;
        if(rd < len) fseek(f, len - rd, SEEK_CUR);   /* oversized record: skip rest */
    }
    fclose(f);

    /* un-rotate: oldest first */
    if(count == cap && head != 0) {
        static lz_msg_rt tmp[LZ_TAIL_MAX];
        for(int i = 0; i < count; i++) tmp[i] = ring[(head + i) % cap];
        memcpy(ring, tmp, sizeof(lz_msg_rt) * count);
    }
    return count;
}

bool lz_store_find_delivery(const char *addr, uint32_t pkt_id, lz_msg_rt *out)
{
    if(!g_persist || !pkt_id || !out) return false;
    char name[24], path[128];
    log_name(name, sizeof name, addr);
    path_for(path, sizeof path, name);
    FILE *f = fopen(path, "rb");
    if(!f) return false;

    bool ok = false;
    for(;;) {
        uint8_t self;
        uint32_t ts;
        uint16_t len_field;
        if(fread(&self, 1, 1, f) != 1) break;
        if(fread(&ts, 4, 1, f) != 1) break;
        if(fread(&len_field, 2, 1, f) != 1) break;
        bool extended = (len_field & LZ_LOG_EXTENDED) != 0;
        bool meta = (len_field & LZ_LOG_DELIVERY_META) != 0;
        uint16_t len = len_field & LZ_LOG_LEN_MASK;
        uint8_t status = LZ_MSG_NONE;
        uint8_t retries = 0;
        uint8_t fail_reason = LZ_FAIL_NONE;
        uint32_t rec_pkt_id = 0;

        if(extended) {
            if(fread(&status, 1, 1, f) != 1) break;
            if(fread(&rec_pkt_id, 4, 1, f) != 1) break;
            if(meta) {
                if(fread(&retries, 1, 1, f) != 1) break;
                if(fread(&fail_reason, 1, 1, f) != 1) break;
            }
        }

        if(self && rec_pkt_id == pkt_id) {
            memset(out, 0, sizeof *out);
            out->self = true;
            out->ts = ts;
            out->status = status;
            out->retries = retries;
            out->fail_reason = fail_reason;
            out->pkt_id = rec_pkt_id;
            size_t rd = len < LZ_TEXT_MAX - 1 ? len : LZ_TEXT_MAX - 1;
            if(fread(out->text, 1, rd, f) != rd) break;
            out->text[rd] = 0;
            ok = true;
            break;
        }

        if(fseek(f, len, SEEK_CUR) != 0) break;
    }
    fclose(f);
    return ok;
}

bool lz_store_update_delivery(const char *addr, uint32_t old_pkt_id,
                              uint32_t new_pkt_id, uint8_t status,
                              uint8_t retries, uint8_t fail_reason)
{
    if(!g_persist || !old_pkt_id) return false;
    char name[24], path[128];
    log_name(name, sizeof name, addr);
    path_for(path, sizeof path, name);
    FILE *f = fopen(path, "r+b");
    if(!f) return false;

    bool ok = false;
    for(;;) {
        uint8_t self;
        uint32_t ts;
        uint16_t len_field;
        if(fread(&self, 1, 1, f) != 1) break;
        if(fread(&ts, 4, 1, f) != 1) break;
        if(fread(&len_field, 2, 1, f) != 1) break;
        bool extended = (len_field & LZ_LOG_EXTENDED) != 0;
        bool meta = (len_field & LZ_LOG_DELIVERY_META) != 0;
        uint16_t len = len_field & LZ_LOG_LEN_MASK;

        if(extended) {
            long status_pos = ftell(f);
            uint8_t rec_status;
            uint32_t rec_pkt_id;
            if(fread(&rec_status, 1, 1, f) != 1) break;
            if(fread(&rec_pkt_id, 4, 1, f) != 1) break;
            if(self && rec_pkt_id == old_pkt_id) {
                fseek(f, status_pos, SEEK_SET);
                fwrite(&status, 1, 1, f);
                fwrite(&new_pkt_id, 4, 1, f);
                if(meta) {
                    fwrite(&retries, 1, 1, f);
                    fwrite(&fail_reason, 1, 1, f);
                }
                ok = true;
                break;
            }
        }
        /* non-matching extended-meta records carry 2 trailing metadata bytes
         * (retries, fail_reason) that we did NOT consume above; skip them too
         * or the scan desyncs and every later record is misread. */
        if(fseek(f, len + (extended && meta ? 2 : 0), SEEK_CUR) != 0) break;
    }
    fclose(f);
    return ok;
}

/* ---- thread index ---- */

void lz_store_save_threads(const lz_thread_rt *t, int n)
{
    if(!g_persist) return;
    char path[128], tmp[132];
    path_for(path, sizeof path, "threads.idx");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    for(int i = 0; i < n; i++) {
        fprintf(f, "%s|%d|%u|%u|%d|%d|%s|%s|%s\n",
                t[i].addr, (int)t[i].net, (unsigned)t[i].node_num,
                (unsigned)t[i].last_ts, t[i].unread, t[i].messageable ? 1 : 0,
                t[i].path, t[i].name, t[i].last_text);
    }
    fclose(f);
    remove(path);
    rename(tmp, path);
}

static char *field(char **cur)
{
    char *s = *cur;
    if(!s) return NULL;
    char *bar = strchr(s, '|');
    if(bar) { *bar = 0; *cur = bar + 1; }
    else *cur = NULL;
    return s;
}

int lz_store_load_threads(lz_thread_rt *out, int cap)
{
    if(!g_persist) return 0;
    char path[128];
    path_for(path, sizeof path, "threads.idx");
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    char line[256];
    int n = 0;
    while(n < cap && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *cur = line;
        char *addr = field(&cur), *net = field(&cur), *num = field(&cur),
             *ts = field(&cur), *unread = field(&cur), *msgable = field(&cur),
             *pathf = field(&cur), *name = field(&cur), *text = cur;
        if(!addr || !net || !num || !ts || !unread || !msgable || !pathf || !name || !text)
            continue;
        lz_thread_rt *t = &out[n++];
        memset(t, 0, sizeof *t);
        snprintf(t->addr, sizeof t->addr, "%s", addr);
        t->net = atoi(net) == 1 ? LZ_NET_MC : LZ_NET_MT;
        t->node_num = (uint32_t)strtoul(num, NULL, 10);
        t->last_ts = (uint32_t)strtoul(ts, NULL, 10);
        t->unread = atoi(unread);
        t->messageable = atoi(msgable) != 0;
        t->is_channel = (t->node_num == LZ_BROADCAST);   /* broadcast = channel */
        snprintf(t->path, sizeof t->path, "%s", pathf);
        snprintf(t->name, sizeof t->name, "%s", name);
        snprintf(t->last_text, sizeof t->last_text, "%s", text);
    }
    fclose(f);
    return n;
}

/* ---- identity (set once during onboarding) ---- */

void lz_store_save_identity(const char *longn, const char *shortn)
{
    if(!g_persist) return;
    char path[128];
    path_for(path, sizeof path, "identity.txt");
    FILE *f = fopen(path, "w");
    if(!f) return;
    fprintf(f, "%s|%s\n", longn, shortn);
    fclose(f);
}

bool lz_store_load_identity(char *longn, int ln, char *shortn, int sn)
{
    if(!g_persist) return false;
    char path[128];
    path_for(path, sizeof path, "identity.txt");
    FILE *f = fopen(path, "r");
    if(!f) return false;
    char line[96];
    bool ok = false;
    if(fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *bar = strchr(line, '|');
        if(bar) {
            *bar = 0;
            snprintf(longn, ln, "%s", line);
            snprintf(shortn, sn, "%s", bar + 1);
            ok = longn[0] != 0;
        }
    }
    fclose(f);
    return ok;
}

/* ---- user settings ---- */

void lz_store_save_settings(const lz_user_settings_t *s)
{
    if(!g_persist || !s) return;
    char path[128], tmp[132];
    path_for(path, sizeof path, "settings.cfg");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    fprintf(f, "2 %d %d %d %d %d %d %d %d %d %d %d\n",
            s->net_mt ? 1 : 0, s->net_mc ? 1 : 0, s->tx, s->gps ? 1 : 0,
            s->bright, s->timeout, s->kb_light, s->tz_idx,
            s->clock24 ? 1 : 0, s->save ? 1 : 0, s->developer ? 1 : 0);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

bool lz_store_load_settings(lz_user_settings_t *s)
{
    if(!g_persist || !s) return false;
    char path[128];
    path_for(path, sizeof path, "settings.cfg");
    FILE *f = fopen(path, "r");
    if(!f) return false;
    char line[160];
    bool have_line = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if(!have_line) return false;
    int ver, mt, mc, tx, gps, bright, timeout, kb, tz, clock24, save, developer = 0;
    int got = sscanf(line, "%d %d %d %d %d %d %d %d %d %d %d %d",
                     &ver, &mt, &mc, &tx, &gps, &bright, &timeout, &kb, &tz,
                     &clock24, &save, &developer);
    if(!((ver == 1 && got == 11) || (ver == 2 && got == 12))) return false;
    s->net_mt = mt != 0;
    s->net_mc = mc != 0;
    s->tx = tx;
    s->gps = gps != 0;
    s->bright = bright;
    s->timeout = timeout;
    s->kb_light = kb;
    s->tz_idx = tz;
    s->clock24 = clock24 != 0;
    s->save = save != 0;
    s->developer = ver >= 2 && developer != 0;
    return true;
}

/* ---- saved Wi-Fi (one network: ssid|password|autoconnect) ---- */

void lz_store_save_wifi(const char *ssid, const char *pass, int autoconnect)
{
    if(!g_persist) return;
    char path[128];
    path_for(path, sizeof path, "wifi.cfg");
    FILE *f = fopen(path, "w");
    if(!f) return;
    fprintf(f, "%s|%s|%d\n", ssid ? ssid : "", pass ? pass : "", autoconnect);
    fclose(f);
}

bool lz_store_load_wifi(char *ssid, int sn, char *pass, int pn, int *autoconnect)
{
    if(!g_persist) return false;
    char path[128];
    path_for(path, sizeof path, "wifi.cfg");
    FILE *f = fopen(path, "r");
    if(!f) return false;
    char line[160];
    bool ok = false;
    if(fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *cur = line;
        char *s = field(&cur), *p = field(&cur), *ac = cur;
        if(s) {
            snprintf(ssid, sn, "%s", s);
            snprintf(pass, pn, "%s", p ? p : "");
            if(autoconnect) *autoconnect = ac && ac[0] ? atoi(ac) : 1;
            ok = ssid[0] != 0;
        }
    }
    fclose(f);
    return ok;
}

/* ---- MeshCore Ed25519 identity (32-byte private seed, hex) ---- */

void lz_store_save_mc_key(const uint8_t *prv32)
{
    if(!g_persist) return;
    char path[128];
    path_for(path, sizeof path, "mc_id.txt");
    FILE *f = fopen(path, "w");
    if(!f) return;
    for(int i = 0; i < 32; i++) fprintf(f, "%02x", prv32[i]);
    fputc('\n', f);
    fclose(f);
}

bool lz_store_load_mc_key(uint8_t *prv32)
{
    if(!g_persist) return false;
    char path[128];
    path_for(path, sizeof path, "mc_id.txt");
    FILE *f = fopen(path, "r");
    if(!f) return false;
    char hex[80];
    bool ok = false;
    if(fgets(hex, sizeof hex, f) && strlen(hex) >= 64) {
        ok = true;
        for(int i = 0; i < 32; i++) {
            unsigned b;
            if(sscanf(hex + i * 2, "%02x", &b) != 1) { ok = false; break; }
            prv32[i] = (uint8_t)b;
        }
    }
    fclose(f);
    return ok;
}

/* ---- Meshtastic PKI X25519 identity (32-byte private key, hex) ---- */

void lz_store_save_mt_key(const uint8_t *prv32)
{
    if(!g_persist) return;
    char path[128];
    path_for(path, sizeof path, "mt_pki.txt");
    FILE *f = fopen(path, "w");
    if(!f) return;
    for(int i = 0; i < 32; i++) fprintf(f, "%02x", prv32[i]);
    fputc('\n', f);
    fclose(f);
}

bool lz_store_load_mt_key(uint8_t *prv32)
{
    if(!g_persist) return false;
    char path[128];
    path_for(path, sizeof path, "mt_pki.txt");
    FILE *f = fopen(path, "r");
    if(!f) return false;
    char hex[80];
    bool ok = false;
    if(fgets(hex, sizeof hex, f) && strlen(hex) >= 64) {
        ok = true;
        for(int i = 0; i < 32; i++) {
            unsigned b;
            if(sscanf(hex + i * 2, "%02x", &b) != 1) { ok = false; break; }
            prv32[i] = (uint8_t)b;
        }
    }
    fclose(f);
    return ok;
}

/* ---- touch calibration transform (swap/invx/invy) ---- */

void lz_store_save_touch(int swap, int invx, int invy)
{
    if(!g_persist) return;
    char path[128];
    path_for(path, sizeof path, "touch.cfg");
    FILE *f = fopen(path, "w");
    if(!f) return;
    fprintf(f, "%d %d %d\n", swap, invx, invy);
    fclose(f);
}

bool lz_store_load_touch(int *swap, int *invx, int *invy)
{
    if(!g_persist) return false;
    char path[128];
    path_for(path, sizeof path, "touch.cfg");
    FILE *f = fopen(path, "r");
    if(!f) return false;
    bool ok = (fscanf(f, "%d %d %d", swap, invx, invy) == 3);
    fclose(f);
    return ok;
}

/* ---- node db ---- */

void lz_store_save_nodes(const lz_node_rt *nodes, int n)
{
    if(!g_persist) return;
    char path[128], tmp[132];
    path_for(path, sizeof path, "nodes.db");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    for(int i = 0; i < n; i++) {
        char pk[66];                         /* X25519 pubkey hex, or "-" */
        if(nodes[i].has_key) { for(int j = 0; j < 32; j++) sprintf(pk + j * 2, "%02x", nodes[i].pubkey[j]); }
        else { pk[0] = '-'; pk[1] = 0; }
        fprintf(f, "%u|%s|%d|%s|%s|%d|%d|%u|%.1f|%s|%s|%s|%s|%u|%ld|%ld|%ld|%u|%u|%u|%u|%d|%u|%u|%u\n",
                (unsigned)nodes[i].num, nodes[i].id, (int)nodes[i].net,
                nodes[i].role, nodes[i].hw, nodes[i].batt,
                nodes[i].contact ? 1 : 0, (unsigned)nodes[i].last_heard,
                (double)nodes[i].snr, nodes[i].dist,
                nodes[i].shortcode, nodes[i].name, pk,
                (unsigned)nodes[i].pos_flags, (long)nodes[i].lat_i,
                (long)nodes[i].lon_i, (long)nodes[i].alt_m,
                (unsigned)nodes[i].pos_time, (unsigned)nodes[i].precision_bits,
                (unsigned)nodes[i].telem_flags, (unsigned)nodes[i].voltage_mv,
                (int)nodes[i].temp_c10, (unsigned)nodes[i].humidity10,
                (unsigned)nodes[i].pressure10, (unsigned)nodes[i].uptime_s);
    }
    fclose(f);
    remove(path);
    rename(tmp, path);
}

int lz_store_load_nodes(lz_node_rt *out, int cap)
{
    if(!g_persist) return 0;
    char path[128];
    path_for(path, sizeof path, "nodes.db");
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    char line[560];     /* room for pubkey plus optional position/telemetry fields */
    int n = 0;
    while(n < cap && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *cur = line;
        char *num = field(&cur), *id = field(&cur), *net = field(&cur),
             *role = field(&cur), *hw = field(&cur), *batt = field(&cur),
             *contact = field(&cur), *heard = field(&cur), *snr = field(&cur),
             *dist = field(&cur), *sc = field(&cur), *name = field(&cur),
             *pk = field(&cur), *posf = field(&cur), *lat = field(&cur),
             *lon = field(&cur), *alt = field(&cur), *post = field(&cur),
             *prec = field(&cur), *telf = field(&cur), *vmv = field(&cur),
             *tc10 = field(&cur), *hum10 = field(&cur), *press10 = field(&cur),
             *uptime = cur;   /* trailing optional fields may be absent */
        if(!num || !id || !net || !role || !hw || !batt || !contact || !heard ||
           !snr || !dist || !sc || !name)
            continue;
        lz_node_rt *nd = &out[n++];
        memset(nd, 0, sizeof *nd);
        nd->num = (uint32_t)strtoul(num, NULL, 10);
        snprintf(nd->id, sizeof nd->id, "%s", id);
        nd->net = atoi(net) == 1 ? LZ_NET_MC : LZ_NET_MT;
        snprintf(nd->role, sizeof nd->role, "%s", role);
        snprintf(nd->hw, sizeof nd->hw, "%s", hw);
        nd->batt = atoi(batt);
        nd->contact = atoi(contact) != 0;
        nd->last_heard = (uint32_t)strtoul(heard, NULL, 10);
        nd->snr = (float)atof(snr);
        snprintf(nd->dist, sizeof nd->dist, "%s", dist);
        snprintf(nd->shortcode, sizeof nd->shortcode, "%s", sc);
        snprintf(nd->name, sizeof nd->name, "%s", name);
        if(pk && pk[0] && pk[0] != '-' && strlen(pk) >= 64) {   /* restore X25519 key */
            bool ok = true;
            for(int j = 0; j < 32; j++) {
                unsigned b;
                if(sscanf(pk + j * 2, "%02x", &b) != 1) { ok = false; break; }
                nd->pubkey[j] = (uint8_t)b;
            }
            nd->has_key = ok;
        }
        if(posf) nd->pos_flags = (uint8_t)strtoul(posf, NULL, 10);
        if(lat)  nd->lat_i = (int32_t)strtol(lat, NULL, 10);
        if(lon)  nd->lon_i = (int32_t)strtol(lon, NULL, 10);
        if(alt)  nd->alt_m = (int32_t)strtol(alt, NULL, 10);
        if(post) nd->pos_time = (uint32_t)strtoul(post, NULL, 10);
        if(prec) nd->precision_bits = (uint8_t)strtoul(prec, NULL, 10);
        if(telf) nd->telem_flags = (uint8_t)strtoul(telf, NULL, 10);
        if(vmv)  nd->voltage_mv = (uint16_t)strtoul(vmv, NULL, 10);
        if(tc10) nd->temp_c10 = (int16_t)strtol(tc10, NULL, 10);
        if(hum10) nd->humidity10 = (uint16_t)strtoul(hum10, NULL, 10);
        if(press10) nd->pressure10 = (uint16_t)strtoul(press10, NULL, 10);
        if(uptime) nd->uptime_s = (uint32_t)strtoul(uptime, NULL, 10);
    }
    fclose(f);
    return n;
}
