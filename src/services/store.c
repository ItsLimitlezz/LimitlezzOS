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
#include "mc_crypto.h"
#include "app_package_fetch.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#ifdef LZ_TARGET_TDECK
#include "esp_system.h"
#endif
#ifdef _WIN32
#include <direct.h>
#else
#include <errno.h>
#include <unistd.h>
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
static char g_appfs_dir[96];
static bool g_persist;

static bool path_is_dir(const char *path);

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

void lz_store_set_appfs_root(const char *root)
{
    if(root && root[0] && path_is_dir(root)) snprintf(g_appfs_dir, sizeof g_appfs_dir, "%s", root);
    else g_appfs_dir[0] = 0;
}

const char *lz_store_appfs_root(void)
{
    return g_appfs_dir[0] ? g_appfs_dir : NULL;
}

int lz_store_file_roots(const char **out, int cap)
{
    if(!out || cap <= 0) return 0;
    int n = 0;
    const char *root = lz_store_file_root();
    if(root && root[0]) out[n++] = root;
    if(g_appfs_dir[0] && n < cap) {
        bool dup = false;
        for(int i = 0; i < n; i++)
            if(strcmp(out[i], g_appfs_dir) == 0) dup = true;
        if(!dup) out[n++] = g_appfs_dir;
    }
    return n;
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

static bool path_rmdir(const char *path)
{
#ifdef _WIN32
    return _rmdir(path) == 0 || !path_is_dir(path);
#else
    return rmdir(path) == 0 || !path_is_dir(path);
#endif
}

#define LZ_APP_DATA_MAX_ENTRIES 96
#define LZ_APP_DATA_MAX_DEPTH 3

static void set_err(char *err, int err_cap, const char *msg)
{
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
}

#define LZ_SHA256_HEX_LEN 64

static int sha256_hex_val(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool sha256_expected_valid(const char *hex)
{
    if(!hex) return false;
    for(int i = 0; i < LZ_SHA256_HEX_LEN; i++) {
        if(sha256_hex_val(hex[i]) < 0) return false;
    }
    return hex[LZ_SHA256_HEX_LEN] == 0;
}

static void sha256_to_hex(const uint8_t digest[32], char *out, int out_cap)
{
    static const char H[] = "0123456789abcdef";
    if(!out || out_cap <= 0) return;
    out[0] = 0;
    if(out_cap < LZ_SHA256_HEX_LEN + 1) return;
    for(int i = 0; i < 32; i++) {
        out[i * 2] = H[digest[i] >> 4];
        out[i * 2 + 1] = H[digest[i] & 0x0f];
    }
    out[LZ_SHA256_HEX_LEN] = 0;
}

static bool sha256_hex_equal(const char *a, const char *b)
{
    if(!a || !b) return false;
    for(int i = 0; i < LZ_SHA256_HEX_LEN; i++) {
        if(sha256_hex_val(a[i]) != sha256_hex_val(b[i])) return false;
    }
    return a[LZ_SHA256_HEX_LEN] == 0 && b[LZ_SHA256_HEX_LEN] == 0;
}

bool lz_store_file_sha256(const char *path, char *out_hex, int out_cap,
                          char *err, int err_cap)
{
    if(out_hex && out_cap > 0) out_hex[0] = 0;
    set_err(err, err_cap, "");
    if(!out_hex || out_cap < LZ_SHA256_HEX_LEN + 1) {
        set_err(err, err_cap, "hash buffer small");
        return false;
    }
    if(!path || !path[0]) {
        set_err(err, err_cap, "missing path");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if(!f) {
        set_err(err, err_cap, "package missing");
        return false;
    }

    lz_sha256_ctx ctx;
    lz_sha256_init(&ctx);
    uint8_t buf[256];
    size_t n = 0;
    while((n = fread(buf, 1, sizeof buf, f)) > 0) {
        lz_sha256_update(&ctx, buf, n);
    }
    if(ferror(f)) {
        fclose(f);
        set_err(err, err_cap, "package read failed");
        return false;
    }
    fclose(f);

    uint8_t digest[32];
    lz_sha256_final(&ctx, digest);
    sha256_to_hex(digest, out_hex, out_cap);
    return true;
}

bool lz_store_verify_file_sha256(const char *path, const char *expected_hex,
                                 char *err, int err_cap)
{
    set_err(err, err_cap, "");
    if(!sha256_expected_valid(expected_hex)) {
        set_err(err, err_cap, "bad sha256");
        return false;
    }

    char actual[LZ_SHA256_HEX_LEN + 1];
    if(!lz_store_file_sha256(path, actual, sizeof actual, err, err_cap)) return false;
    if(!sha256_hex_equal(actual, expected_hex)) {
        set_err(err, err_cap, "sha mismatch");
        return false;
    }
    return true;
}

static bool app_data_usage_walk(const char *dir, int depth, int *entries,
                                uint32_t *used, char *err, int err_cap)
{
    if(depth > LZ_APP_DATA_MAX_DEPTH) {
        set_err(err, err_cap, "data too deep");
        return false;
    }
    DIR *d = opendir(dir);
    if(!d) {
        set_err(err, err_cap, "data scan failed");
        return false;
    }

    struct dirent *e;
    while((e = readdir(d)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if(++(*entries) > LZ_APP_DATA_MAX_ENTRIES) {
            closedir(d);
            set_err(err, err_cap, "data too many files");
            return false;
        }

        char path[160];
        path_join(path, sizeof path, dir, e->d_name);
        struct stat st;
        if(stat(path, &st) != 0) {
            closedir(d);
            set_err(err, err_cap, "data scan failed");
            return false;
        }
        if(S_ISDIR(st.st_mode)) {
            if(!app_data_usage_walk(path, depth + 1, entries, used, err, err_cap)) {
                closedir(d);
                return false;
            }
        } else {
            uint32_t sz = 0;
            if(st.st_size > 0) {
                unsigned long long raw_size = (unsigned long long)st.st_size;
                sz = raw_size > UINT32_MAX ? UINT32_MAX : (uint32_t)raw_size;
            }
            if(UINT32_MAX - *used < sz) *used = UINT32_MAX;
            else *used += sz;
        }
    }
    closedir(d);
    return true;
}

static bool app_data_clear_walk(const char *dir, int depth, int *entries,
                                char *err, int err_cap)
{
    if(depth > LZ_APP_DATA_MAX_DEPTH) {
        set_err(err, err_cap, "data too deep");
        return false;
    }
    DIR *d = opendir(dir);
    if(!d) {
        set_err(err, err_cap, "data scan failed");
        return false;
    }

    struct dirent *e;
    while((e = readdir(d)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if(++(*entries) > LZ_APP_DATA_MAX_ENTRIES) {
            closedir(d);
            set_err(err, err_cap, "data too many files");
            return false;
        }

        char path[160];
        path_join(path, sizeof path, dir, e->d_name);
        struct stat st;
        if(stat(path, &st) != 0) {
            closedir(d);
            set_err(err, err_cap, "data scan failed");
            return false;
        }
        if(S_ISDIR(st.st_mode)) {
            if(!app_data_clear_walk(path, depth + 1, entries, err, err_cap)) {
                closedir(d);
                return false;
            }
            if(!path_rmdir(path)) {
                closedir(d);
                set_err(err, err_cap, "data clear failed");
                return false;
            }
        } else if(remove(path) != 0) {
            closedir(d);
            set_err(err, err_cap, "data clear failed");
            return false;
        }
    }
    closedir(d);
    return true;
}

#define LZ_APP_REMOVE_MAX_ENTRIES 128
#define LZ_APP_REMOVE_MAX_DEPTH 4

static bool remove_tree_walk(const char *dir, int depth, int *entries,
                             char *err, int err_cap)
{
    if(depth > LZ_APP_REMOVE_MAX_DEPTH) {
        set_err(err, err_cap, "remove too deep");
        return false;
    }
    DIR *d = opendir(dir);
    if(!d) {
        set_err(err, err_cap, "remove scan failed");
        return false;
    }

    struct dirent *e;
    while((e = readdir(d)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if(++(*entries) > LZ_APP_REMOVE_MAX_ENTRIES) {
            closedir(d);
            set_err(err, err_cap, "remove too many files");
            return false;
        }

        char path[160];
        path_join(path, sizeof path, dir, e->d_name);
        struct stat st;
        if(stat(path, &st) != 0) {
            closedir(d);
            set_err(err, err_cap, "remove scan failed");
            return false;
        }
        if(S_ISDIR(st.st_mode)) {
            if(!remove_tree_walk(path, depth + 1, entries, err, err_cap)) {
                closedir(d);
                return false;
            }
            if(!path_rmdir(path)) {
                closedir(d);
                set_err(err, err_cap, "remove failed");
                return false;
            }
        } else if(remove(path) != 0) {
            closedir(d);
            set_err(err, err_cap, "remove failed");
            return false;
        }
    }
    closedir(d);
    return true;
}

static bool remove_tree(const char *path, char *err, int err_cap)
{
    if(!path || !path[0]) {
        set_err(err, err_cap, "remove failed");
        return false;
    }
    struct stat st;
    if(stat(path, &st) != 0) return true;
    if(!S_ISDIR(st.st_mode)) {
        if(remove(path) == 0) return true;
        set_err(err, err_cap, "remove failed");
        return false;
    }
    int entries = 0;
    if(!remove_tree_walk(path, 0, &entries, err, err_cap)) return false;
    if(!path_rmdir(path)) {
        set_err(err, err_cap, "remove failed");
        return false;
    }
    return true;
}

bool lz_store_save_app_catalog_cache(const char *json, int len, char *err, int err_cap)
{
    set_err(err, err_cap, "");
    if(!g_persist) {
        set_err(err, err_cap, "storage unavailable");
        return false;
    }
    if(!json || len <= 0) {
        set_err(err, err_cap, "catalog empty");
        return false;
    }
    if(len > LZ_APP_CATALOG_CACHE_MAX) {
        set_err(err, err_cap, "catalog too large");
        return false;
    }

    char path[128], tmp[132];
    path_for(path, sizeof path, "app_catalog.json");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if(!f) {
        set_err(err, err_cap, "catalog write failed");
        return false;
    }
    bool ok = fwrite(json, 1, (size_t)len, f) == (size_t)len;
    if(fclose(f) != 0) ok = false;
    if(!ok) {
        remove(tmp);
        set_err(err, err_cap, "catalog write failed");
        return false;
    }
    remove(path);
    if(rename(tmp, path) != 0) {
        remove(tmp);
        set_err(err, err_cap, "catalog commit failed");
        return false;
    }
    return true;
}

bool lz_store_load_app_catalog_cache(char *out, int cap, int *out_len,
                                     char *err, int err_cap)
{
    if(out && cap > 0) out[0] = 0;
    if(out_len) *out_len = 0;
    set_err(err, err_cap, "");
    if(!g_persist) {
        set_err(err, err_cap, "storage unavailable");
        return false;
    }
    if(!out || cap <= 1) {
        set_err(err, err_cap, "catalog buffer small");
        return false;
    }

    char path[128];
    path_for(path, sizeof path, "app_catalog.json");
    FILE *f = fopen(path, "rb");
    if(!f) {
        set_err(err, err_cap, "catalog missing");
        return false;
    }
    if(fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err, err_cap, "catalog read failed");
        return false;
    }
    long raw_len = ftell(f);
    if(raw_len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_err(err, err_cap, "catalog read failed");
        return false;
    }
    if(raw_len > LZ_APP_CATALOG_CACHE_MAX) {
        fclose(f);
        set_err(err, err_cap, "catalog too large");
        return false;
    }
    if(raw_len >= cap) {
        fclose(f);
        set_err(err, err_cap, "catalog buffer small");
        return false;
    }
    size_t n = fread(out, 1, (size_t)raw_len, f);
    bool ok = n == (size_t)raw_len && ferror(f) == 0;
    fclose(f);
    if(!ok) {
        out[0] = 0;
        set_err(err, err_cap, "catalog read failed");
        return false;
    }
    out[raw_len] = 0;
    if(out_len) *out_len = (int)raw_len;
    return true;
}

bool lz_store_clear_app_catalog_cache(char *err, int err_cap)
{
    set_err(err, err_cap, "");
    if(!g_persist) {
        set_err(err, err_cap, "storage unavailable");
        return false;
    }
    char path[128];
    path_for(path, sizeof path, "app_catalog.json");
    remove(path);
    return true;
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

static bool json_get_string_bounded(const char *json, const char *key, char *out, size_t n)
{
    if(!out || n == 0) return false;
    const char *p = json_value_for(json, key);
    if(!p || *p != '"') return false;
    p++;
    size_t j = 0;
    bool too_long = false;
    while(*p && *p != '"') {
        char c = *p++;
        if(c == '\\' && *p) {
            char e = *p++;
            if(e == 'n') c = '\n';
            else if(e == 'r') c = '\r';
            else if(e == 't') c = '\t';
            else c = e;
        }
        if(c < 32) continue;
        if(j + 1 < n) out[j++] = c;
        else too_long = true;
    }
    if(*p != '"' || too_long) return false;
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

static bool json_get_u32(const char *json, const char *key, uint32_t *out)
{
    const char *p = json_value_for(json, key);
    if(!p || !out) return false;
    if(*p == '-') return false;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if(end == p) return false;
    while(*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if(*end && *end != ',' && *end != '}') return false;
    if(v > UINT32_MAX) return false;
    *out = (uint32_t)v;
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
    if(strcmp(s, ".") == 0) return false;   /* lone current-dir token */
    if(strstr(s, "..")) return false;       /* parent-dir traversal: id is a path segment */
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

static bool catalog_fail(lz_app_catalog_report_t *r, const char *id, const char *msg)
{
    if(r) {
        r->ok = false;
        r->rejected_count++;
        if(!r->first_error[0]) {
            snprintf(r->first_error, sizeof r->first_error, "%s", msg ? msg : "invalid catalog");
            if(id && id[0]) snprintf(r->first_id, sizeof r->first_id, "%s", id);
        }
    }
    return false;
}

static bool json_string_present_max(const char *json, const char *key, size_t max_len)
{
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
        if(c < 32) continue;
        if(++j > max_len) return false;
    }
    return *p == '"' && j > 0;
}

static bool catalog_version_ok(const char *s)
{
    if(!s || !(s[0] >= '0' && s[0] <= '9')) return false;
    for(int i = 0; s[i]; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') || c == '_' || c == '.' ||
                  c == '+' || c == '-';
        if(!ok) return false;
    }
    return true;
}

static bool catalog_icon_ok(const char *s)
{
    if(!s || !s[0]) return false;
    for(int i = 0; s[i]; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') || c == '_' || c == '-';
        if(!ok) return false;
    }
    return true;
}

static bool catalog_url_ok(const char *url)
{
    if(!url || !url[0]) return false;
    if(strncmp(url, "https://", 8) != 0) return false;
    if(!url[8] || url[8] == '/') return false;
    for(int i = 0; url[i]; i++) {
        char c = url[i];
        if(c <= 32 || c == '"' || c == '<' || c == '>') return false;
    }
    return true;
}

static bool catalog_sha256_ok(const char *s)
{
    if(!s) return false;
    for(int i = 0; i < 64; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if(!hex) return false;
    }
    return s[64] == 0;
}

static const char *json_array_for(const char *json, const char *key)
{
    const char *p = json_value_for(json, key);
    if(!p || *p != '[') return NULL;
    return p;
}

static bool catalog_parse_permissions_value(const char *p, uint16_t *out)
{
    if(!p || !out) return false;
    p = skip_ws(p);
    if(*p != '[') return false;
    p = skip_ws(p + 1);
    uint16_t bits = 0;
    int count = 0;
    if(*p == ']') return false;
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
        if(!bit || (bits & bit)) return false;
        bits |= bit;
        count++;
        p = skip_ws(p + 1);
        if(*p == ',') { p = skip_ws(p + 1); continue; }
        if(*p == ']') {
            if(count == 0 || !(bits & LZ_APP_PERM_DISPLAY)) return false;
            *out = bits;
            return true;
        }
        return false;
    }
}

static bool catalog_api_versions_ok(const char *p, const char *api)
{
    if(!p || *skip_ws(p) != '[' || !api || !api[0]) return false;
    p = skip_ws(p) + 1;
    p = skip_ws(p);
    bool found = false;
    int count = 0;
    if(*p == ']') return false;
    for(;;) {
        if(*p != '"') return false;
        p++;
        char value[12];
        size_t j = 0;
        while(*p && *p != '"') {
            char c = *p++;
            if(c == '\\' && *p) c = *p++;
            if(j + 1 < sizeof value && c >= 32) value[j++] = c;
            else return false;
        }
        if(*p != '"' || j == 0) return false;
        value[j] = 0;
        if(!api_version_supported(value)) return false;
        if(strcmp(value, api) == 0) found = true;
        count++;
        p = skip_ws(p + 1);
        if(*p == ',') { p = skip_ws(p + 1); continue; }
        if(*p == ']') return count > 0 && found;
        return false;
    }
}

static bool catalog_targets_ok(const char *p, bool *target_tdeck, bool *target_sim)
{
    if(target_tdeck) *target_tdeck = false;
    if(target_sim) *target_sim = false;
    if(!p || *skip_ws(p) != '[') return false;
    p = skip_ws(p) + 1;
    p = skip_ws(p);
    int count = 0;
    if(*p == ']') return false;
    for(;;) {
        if(*p != '"') return false;
        p++;
        char value[12];
        size_t j = 0;
        while(*p && *p != '"') {
            char c = *p++;
            if(c == '\\' && *p) c = *p++;
            if(j + 1 < sizeof value && c >= 32) value[j++] = c;
            else return false;
        }
        if(*p != '"' || j == 0) return false;
        value[j] = 0;
        if(strcmp(value, "tdeck") == 0) {
            if(target_tdeck) *target_tdeck = true;
        } else if(strcmp(value, "sim") == 0) {
            if(target_sim) *target_sim = true;
        } else {
            return false;
        }
        count++;
        p = skip_ws(p + 1);
        if(*p == ',') { p = skip_ws(p + 1); continue; }
        if(*p == ']') return count > 0;
        return false;
    }
}

static const char *catalog_next_object(const char *p, char *out, size_t cap,
                                       bool *done, bool *too_big)
{
    if(done) *done = false;
    if(too_big) *too_big = false;
    if(!p || !out || cap == 0) return NULL;
    p = skip_ws(p);
    if(*p == ',') p = skip_ws(p + 1);
    if(*p == ']') {
        if(done) *done = true;
        return p + 1;
    }
    if(*p != '{') return NULL;

    int depth = 0;
    bool in_str = false;
    bool esc = false;
    size_t j = 0;
    for(; *p; p++) {
        char c = *p;
        if(j + 1 < cap) out[j++] = c;
        else if(too_big) *too_big = true;

        if(in_str) {
            if(esc) esc = false;
            else if(c == '\\') esc = true;
            else if(c == '"') in_str = false;
        } else {
            if(c == '"') in_str = true;
            else if(c == '{') depth++;
            else if(c == '}') {
                depth--;
                if(depth == 0) {
                    out[j] = 0;
                    return p + 1;
                }
            }
        }
    }
    return NULL;
}

static bool catalog_screenshots_ok(const char *p, uint8_t *count_out)
{
    if(count_out) *count_out = 0;
    if(!p) return true;
    p = skip_ws(p);
    if(*p != '[') return false;
    p = skip_ws(p + 1);
    int count = 0;
    if(*p == ']') return true;
    for(;;) {
        if(++count > LZ_APP_CATALOG_SCREENSHOT_MAX) return false;
        char obj[360];
        bool done = false, too_big = false;
        p = catalog_next_object(p, obj, sizeof obj, &done, &too_big);
        if(done || !p || too_big) return false;
        char url[181];
        uint32_t width = 0, height = 0;
        if(!json_get_string_bounded(obj, "url", url, sizeof url) ||
           !catalog_url_ok(url) ||
           !json_get_u32(obj, "width", &width) || width == 0 ||
           !json_get_u32(obj, "height", &height) || height == 0) {
            return false;
        }
        const char *sha_p = json_value_for(obj, "sha256");
        if(sha_p) {
            char sha[65];
            if(!json_get_string_bounded(obj, "sha256", sha, sizeof sha) ||
               !catalog_sha256_ok(sha)) {
                return false;
            }
        }
        p = skip_ws(p);
        if(*p == ',') { p = skip_ws(p + 1); continue; }
        if(*p == ']') {
            if(count_out) *count_out = (uint8_t)count;
            return true;
        }
        return false;
    }
}

static bool catalog_id_seen(const char seen[][24], int count, const char *id)
{
    for(int i = 0; i < count; i++)
        if(strcmp(seen[i], id) == 0) return true;
    return false;
}

static bool catalog_validate_app(const char *obj, lz_app_catalog_report_t *r,
                                 lz_app_catalog_entry_t *entry,
                                 char seen_ids[][24], int seen_count)
{
    lz_app_catalog_entry_t e;
    memset(&e, 0, sizeof e);
    e.hue = -1;

    if(!json_get_string_bounded(obj, "id", e.id, sizeof e.id))
        return catalog_fail(r, NULL, "missing id");
    if(!safe_id(e.id)) return catalog_fail(r, e.id, "unsafe id");
    if(catalog_id_seen((const char (*)[24])seen_ids, seen_count, e.id))
        return catalog_fail(r, e.id, "duplicate id");
    if(!json_get_string_bounded(obj, "name", e.name, sizeof e.name))
        return catalog_fail(r, e.id, "missing name");
    if(!json_get_string_bounded(obj, "version", e.version, sizeof e.version))
        return catalog_fail(r, e.id, "missing version");
    if(!catalog_version_ok(e.version))
        return catalog_fail(r, e.id, "bad version");
    if(!json_get_string_bounded(obj, "author", e.author, sizeof e.author))
        return catalog_fail(r, e.id, "missing author");
    if(!json_get_string_bounded(obj, "summary", e.summary, sizeof e.summary))
        return catalog_fail(r, e.id, "missing summary");
    if(!json_string_present_max(obj, "description", 240))
        return catalog_fail(r, e.id, "missing description");
    if(!json_get_string_bounded(obj, "icon", e.icon, sizeof e.icon))
        return catalog_fail(r, e.id, "missing icon");
    if(!catalog_icon_ok(e.icon)) return catalog_fail(r, e.id, "bad icon");
    if(!json_get_string_bounded(obj, "api_version", e.api_version, sizeof e.api_version))
        return catalog_fail(r, e.id, "missing api_version");
    if(!api_version_supported(e.api_version)) return catalog_fail(r, e.id, "unsupported SDK");
    if(!json_get_string_bounded(obj, "package_url", e.package_url, sizeof e.package_url))
        return catalog_fail(r, e.id, "missing package_url");
    if(!catalog_url_ok(e.package_url)) return catalog_fail(r, e.id, "bad package_url");
    if(!json_get_string_bounded(obj, "package_sha256", e.package_sha256, sizeof e.package_sha256))
        return catalog_fail(r, e.id, "missing package_sha256");
    if(!catalog_sha256_ok(e.package_sha256)) return catalog_fail(r, e.id, "bad package_sha256");
    if(!json_get_u32(obj, "package_bytes", &e.package_bytes))
        return catalog_fail(r, e.id, "missing package_bytes");
    if(e.package_bytes == 0 || e.package_bytes > LZ_APP_CATALOG_PACKAGE_MAX_BYTES)
        return catalog_fail(r, e.id, "bad package_bytes");
    if(!json_get_int(obj, "hue", &e.hue) || e.hue < -1 || e.hue > 359)
        return catalog_fail(r, e.id, "bad hue");

    uint16_t perms = 0;
    const char *p = json_value_for(obj, "permissions");
    if(!p || !catalog_parse_permissions_value(p, &perms))
        return catalog_fail(r, e.id, "bad permissions");
    e.permissions = perms;

    const char *compat = json_value_for(obj, "compatibility");
    if(!compat || *skip_ws(compat) != '{')
        return catalog_fail(r, e.id, "missing compatibility");
    if(!catalog_api_versions_ok(json_value_for(compat, "api_versions"), e.api_version))
        return catalog_fail(r, e.id, "bad compatibility");
    if(!catalog_targets_ok(json_value_for(compat, "targets"), &e.target_tdeck, &e.target_sim))
        return catalog_fail(r, e.id, "bad compatibility");
    const char *min_os = json_value_for(compat, "min_os");
    if(min_os) {
        if(!json_get_string_bounded(compat, "min_os", e.min_os, sizeof e.min_os) ||
           !catalog_version_ok(e.min_os))
            return catalog_fail(r, e.id, "bad compatibility");
    }

    const char *shots = json_value_for(obj, "screenshots");
    if(!catalog_screenshots_ok(shots, &e.screenshot_count))
        return catalog_fail(r, e.id, "bad screenshots");

    if(entry) *entry = e;
    return true;
}

bool lz_store_parse_app_catalog_json(const char *json, lz_app_catalog_entry_t *out,
                                     int cap, lz_app_catalog_report_t *report)
{
    lz_app_catalog_report_t r;
    memset(&r, 0, sizeof r);
    r.ok = false;
    if(!json || !json[0]) {
        catalog_fail(&r, NULL, "empty catalog");
        if(report) *report = r;
        return false;
    }
    if(*skip_ws(json) != '{') {
        catalog_fail(&r, NULL, "catalog root");
        if(report) *report = r;
        return false;
    }
    if(strlen(json) > LZ_APP_CATALOG_JSON_MAX) {
        catalog_fail(&r, NULL, "catalog too large");
        if(report) *report = r;
        return false;
    }

    char schema[32];
    if(!json_get_string_bounded(json, "schema", schema, sizeof schema) ||
       strcmp(schema, LZ_APP_CATALOG_SCHEMA) != 0) {
        catalog_fail(&r, NULL, "bad schema");
        if(report) *report = r;
        return false;
    }
    const char *generated_at = json_value_for(json, "generated_at");
    if(generated_at && *generated_at != '"') {
        catalog_fail(&r, NULL, "bad generated_at");
        if(report) *report = r;
        return false;
    }

    const char *apps = json_array_for(json, "apps");
    if(!apps) {
        catalog_fail(&r, NULL, "missing apps");
        if(report) *report = r;
        return false;
    }

    const char *p = skip_ws(apps + 1);
    if(*p == ']') {
        r.ok = true;
        r.app_count = 0;
        if(report) *report = r;
        return true;
    }

    char *obj = (char *)malloc(1536);
    char (*seen_ids)[24] = (char (*)[24])calloc(LZ_APP_CATALOG_MAX_APPS, sizeof(*seen_ids));
    if(!obj || !seen_ids) {
        free(obj);
        free(seen_ids);
        catalog_fail(&r, NULL, "catalog buffer unavailable");
        if(report) *report = r;
        return false;
    }
    bool result = false;
    for(;;) {
        bool done = false, too_big = false;
        p = catalog_next_object(p, obj, 1536, &done, &too_big);
        if(done) break;
        if(!p) {
            catalog_fail(&r, NULL, "bad apps array");
            goto done;
        }
        if(too_big) {
            catalog_fail(&r, NULL, "app entry too large");
            goto done;
        }
        if(r.app_count + r.rejected_count >= LZ_APP_CATALOG_MAX_APPS) {
            catalog_fail(&r, NULL, "too many apps");
            goto done;
        }
        lz_app_catalog_entry_t entry;
        if(!catalog_validate_app(obj, &r, &entry, seen_ids, r.app_count)) {
            goto done;
        }
        snprintf(seen_ids[r.app_count], sizeof seen_ids[r.app_count], "%s", entry.id);
        if(out && cap > 0 && r.app_count < cap) out[r.app_count] = entry;
        r.app_count++;
    }

    r.ok = r.rejected_count == 0;
    if(!r.ok && !r.first_error[0]) snprintf(r.first_error, sizeof r.first_error, "invalid catalog");
    result = r.ok;
done:
    free(obj);
    free(seen_ids);
    if(report) *report = r;
    return result;
}

bool lz_store_validate_app_catalog_json(const char *json, lz_app_catalog_report_t *out)
{
    return lz_store_parse_app_catalog_json(json, NULL, 0, out);
}

static int catalog_report_line(char *buf, int n, const char *prefix,
                               const lz_app_catalog_report_t *r)
{
    if(!buf || n <= 0 || !r) return 0;
    if(r->ok)
        return snprintf(buf, (size_t)n, "%s: ready apps=%d rejected=0\n",
                        prefix, r->app_count);
    if(r->first_id[0])
        return snprintf(buf, (size_t)n, "%s: invalid apps=%d rejected=%d first=%s error=\"%s\"\n",
                        prefix, r->app_count, r->rejected_count, r->first_id, r->first_error);
    return snprintf(buf, (size_t)n, "%s: invalid apps=%d rejected=%d error=\"%s\"\n",
                    prefix, r->app_count, r->rejected_count, r->first_error);
}

static bool catalog_read_file(const char *path, lz_app_catalog_entry_t *out, int cap,
                              lz_app_catalog_report_t *r, int *loaded)
{
    if(loaded) *loaded = 0;
    FILE *f = fopen(path, "rb");
    if(!f) return false;
    size_t cap_json = LZ_APP_CATALOG_JSON_MAX + 2;
    char *json = (char *)malloc(cap_json);
    if(!json) {
        fclose(f);
        lz_app_catalog_report_t tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.ok = false;
        catalog_fail(&tmp, NULL, "catalog buffer unavailable");
        if(r) *r = tmp;
        return true;
    }
    size_t n = fread(json, 1, cap_json - 1, f);
    fclose(f);
    json[n] = 0;
    if(n >= cap_json - 1) {
        lz_app_catalog_report_t tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.ok = false;
        catalog_fail(&tmp, NULL, "catalog too large");
        if(r) *r = tmp;
        free(json);
        return true;
    }
    lz_store_parse_app_catalog_json(json, out, cap, r);
    free(json);
    if(loaded && r && r->ok) *loaded = r->app_count < cap ? r->app_count : cap;
    return true;
}

int lz_store_load_app_catalog(lz_app_catalog_entry_t *out, int cap,
                              lz_app_catalog_report_t *report)
{
    if(!out || cap <= 0) return 0;
    if(report) memset(report, 0, sizeof *report);
    char path[160];
    int loaded = 0;
    if(g_persist) {
        path_for(path, sizeof path, "app_catalog.json");
        if(catalog_read_file(path, out, cap, report, &loaded)) return loaded;

        path_join(path, sizeof path, g_dir, "catalog/index.json");
        if(catalog_read_file(path, out, cap, report, &loaded)) return loaded;
    }
    if(g_appfs_dir[0]) {
        path_join(path, sizeof path, g_appfs_dir, "catalog/index.json");
        if(catalog_read_file(path, out, cap, report, &loaded)) return loaded;
    }
    if(report) {
        report->ok = false;
        snprintf(report->first_error, sizeof report->first_error, "catalog missing");
    }
    return 0;
}

int lz_store_app_catalog_diag(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    char path[160];
    if(g_persist) {
        path_for(path, sizeof path, "app_catalog.json");
        lz_app_catalog_report_t r;
        if(catalog_read_file(path, NULL, 0, &r, NULL))
            return catalog_report_line(buf, n, "app catalog", &r);

        path_join(path, sizeof path, g_dir, "catalog/index.json");
        if(catalog_read_file(path, NULL, 0, &r, NULL))
            return catalog_report_line(buf, n, "app catalog", &r);
    }
    if(g_appfs_dir[0]) {
        path_join(path, sizeof path, g_appfs_dir, "catalog/index.json");
        lz_app_catalog_report_t r;
        if(catalog_read_file(path, NULL, 0, &r, NULL))
            return catalog_report_line(buf, n, "app catalog", &r);
    }
    return snprintf(buf, (size_t)n, "app catalog: no cached index\n");
}

int lz_store_app_catalog_selftest(char *buf, int n)
{
    static const char valid[] =
        "{\"schema\":\"limitlezz.app.catalog.v1\",\"generated_at\":\"2026-06-18T00:00:00Z\","
        "\"apps\":["
        "{\"id\":\"weather.mesh\",\"name\":\"Weather Mesh\",\"version\":\"0.1.0\","
        "\"author\":\"Limitless\",\"summary\":\"Local weather reports\","
        "\"description\":\"Local weather reports from nearby mesh stations\","
        "\"icon\":\"weather\",\"hue\":48,\"api_version\":\"0.1\","
        "\"compatibility\":{\"api_versions\":[\"0.1\"],\"targets\":[\"tdeck\",\"sim\"]},"
        "\"permissions\":[\"display\",\"network_wifi\"],"
        "\"package_url\":\"https://apps.example.invalid/weather.mesh.zip\","
        "\"package_sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"package_bytes\":32768,"
        "\"screenshots\":[{\"url\":\"https://apps.example.invalid/weather.bmp\","
        "\"width\":320,\"height\":240}]},"
        "{\"id\":\"notes.local\",\"name\":\"Field Notes\",\"version\":\"0.1.0\","
        "\"author\":\"Limitless\",\"summary\":\"Simple local notes\","
        "\"description\":\"Simple local notes with scoped storage\","
        "\"icon\":\"notes\",\"hue\":95,\"api_version\":\"0.1\","
        "\"compatibility\":{\"api_versions\":[\"0.1\"],\"targets\":[\"tdeck\"]},"
        "\"permissions\":[\"display\",\"input\",\"storage\"],"
        "\"package_url\":\"https://apps.example.invalid/notes.local.zip\","
        "\"package_sha256\":\"abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd\","
        "\"package_bytes\":49152}]}";
    static const char invalid[] =
        "{\"schema\":\"limitlezz.app.catalog.v1\",\"apps\":["
        "{\"id\":\"bad.local\",\"name\":\"Bad\",\"version\":\"0.1.0\","
        "\"author\":\"Limitless\",\"summary\":\"Bad checksum\","
        "\"description\":\"Bad checksum\",\"icon\":\"bug\",\"hue\":20,"
        "\"api_version\":\"0.1\","
        "\"compatibility\":{\"api_versions\":[\"0.1\"],\"targets\":[\"tdeck\"]},"
        "\"permissions\":[\"display\"],"
        "\"package_url\":\"https://apps.example.invalid/bad.zip\","
        "\"package_sha256\":\"not-a-sha\",\"package_bytes\":1024}]}";

    lz_app_catalog_report_t ok, bad;
    lz_app_catalog_entry_t entries[2];
    bool valid_ok = lz_store_parse_app_catalog_json(valid, entries, 2, &ok);
    bool invalid_ok = lz_store_validate_app_catalog_json(invalid, &bad);
    const char *result = (valid_ok && ok.app_count == 2 && !invalid_ok &&
                          strcmp(bad.first_error, "bad package_sha256") == 0 &&
                          strcmp(entries[0].package_url,
                                 "https://apps.example.invalid/weather.mesh.zip") == 0) ? "PASS" : "FAIL";
    return snprintf(buf, (size_t)n,
                    "App catalog selftest: %s valid=%d invalid_error=\"%s\"\n",
                    result, ok.app_count, bad.first_error);
    }
#define LZ_APP_INSTALL_ID_MAX 23
#define LZ_APP_STAGING_PREFIX ".install-"
#define LZ_APP_BACKUP_PREFIX ".previous-"

static bool app_install_temp_name(const char *name)
{
    return name &&
           (strncmp(name, LZ_APP_STAGING_PREFIX, strlen(LZ_APP_STAGING_PREFIX)) == 0 ||
            strncmp(name, LZ_APP_BACKUP_PREFIX, strlen(LZ_APP_BACKUP_PREFIX)) == 0);
}

#define LZ_APP_RETAINED_PREFIX ".retained-"

static bool app_retained_name(const char *name)
{
    return name && strncmp(name, LZ_APP_RETAINED_PREFIX, strlen(LZ_APP_RETAINED_PREFIX)) == 0;
}

static bool app_install_root(char *root, size_t root_cap, char *err, int err_cap)
{
    if(g_persist) {
        snprintf(root, root_cap, "%s", g_dir);
        return true;
    }
    if(g_appfs_dir[0]) {
        snprintf(root, root_cap, "%s", g_appfs_dir);
        return true;
    }
    set_err(err, err_cap, "storage unavailable");
    return false;
}

static bool app_retained_paths(const char *id, char *root, size_t root_cap,
                               char *data, size_t data_cap,
                               char *err, int err_cap)
{
    if(!g_persist) {
        set_err(err, err_cap, "storage unavailable");
        return false;
    }
    if(!safe_id(id) || strlen(id) > 23) {
        set_err(err, err_cap, "unsafe id");
        return false;
    }

    char apps[128];
    char retained_name[40];
    int rn = snprintf(retained_name, sizeof retained_name, "%s%s", LZ_APP_RETAINED_PREFIX, id);
    if(rn < 0 || rn >= (int)sizeof retained_name) {
        set_err(err, err_cap, "path too long");
        return false;
    }
    path_join(apps, sizeof apps, g_dir, "apps");
    path_join(root, root_cap, apps, retained_name);
    path_join(data, data_cap, root, "data");
    return true;
}

static bool app_path_in_primary_apps(const char *path)
{
    if(!g_persist || !path || !path[0]) return false;
    char apps[128];
    path_join(apps, sizeof apps, g_dir, "apps");
    size_t al = strlen(apps);
    if(strncmp(path, apps, al) != 0) return false;
    if(path[al] != '/' && path[al] != '\\') return false;
    const char *rest = path + al + 1;
    return rest[0] && strchr(rest, '/') == NULL && strchr(rest, '\\') == NULL;
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

static void clean_line_copy(char *out, size_t cap, const char *src)
{
    if(!out || cap == 0) return;
    while(*src == ' ' || *src == '\t') src++;
    size_t j = 0;
    while(src[j] && src[j] != '\r' && src[j] != '\n') j++;
    while(j > 0 && (src[j - 1] == ' ' || src[j - 1] == '\t')) j--;
    size_t n = j < cap - 1 ? j : cap - 1;
    memcpy(out, src, n);
    out[n] = 0;
}

static const char *entry_value_for(const char *line, const char *key)
{
    line = skip_ws(line);
    if(line[0] == '-' && line[1] == '-') line = skip_ws(line + 2);
    else if(line[0] == '#') line = skip_ws(line + 1);
    size_t kl = strlen(key);
    if(strncmp(line, key, kl) != 0) return NULL;
    line = skip_ws(line + kl);
    if(*line != ':' && *line != '=') return NULL;
    return skip_ws(line + 1);
}

static bool append_body_line(char *body, size_t cap, const char *line)
{
    char clean[96];
    clean_line_copy(clean, sizeof clean, line);
    if(!clean[0]) return false;
    size_t have = strlen(body);
    size_t need = strlen(clean);
    if(have + (have ? 1 : 0) + need + 1 > cap) return false;
    if(have) strncat(body, "\n", cap - strlen(body) - 1);
    strncat(body, clean, cap - strlen(body) - 1);
    return true;
}

static const char *copy_action_part(const char *src, char *out, size_t cap)
{
    if(!src || !out || cap == 0) return src;
    src = skip_ws(src);
    const char *p = src;
    while(*p && *p != '|' && *p != '\r' && *p != '\n') p++;
    const char *end = p;
    while(end > src && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t n = (size_t)(end - src);
    if(n >= cap) n = cap - 1;
    memcpy(out, src, n);
    out[n] = 0;
    return *p == '|' ? p + 1 : p;
}

static bool add_session_action(lz_local_app_session_t *out, const char *spec)
{
    if(!out || !spec || out->action_count >= LZ_LOCAL_APP_ACTION_MAX) return false;
    lz_local_app_action_t *a = &out->actions[out->action_count];
    memset(a, 0, sizeof *a);
    spec = copy_action_part(spec, a->label, sizeof a->label);
    if(!a->label[0]) return false;
    spec = copy_action_part(spec, a->status, sizeof a->status);
    spec = copy_action_part(spec, a->body, sizeof a->body);
    copy_action_part(spec, a->effect, sizeof a->effect);
    if(!a->status[0]) snprintf(a->status, sizeof a->status, "Action handled");
    if(!a->body[0])
        snprintf(a->body, sizeof a->body, "%s completed in the foreground sandbox.",
                 a->label);
    out->action_count++;
    return true;
}

static bool effect_counter_key(const char *effect, char *key, size_t cap)
{
    if(!effect || !key || cap == 0) return false;
    const char *p = NULL;
    if(strncmp(effect, "counter:", 8) == 0) p = effect + 8;
    else if(strncmp(effect, "count:", 6) == 0) p = effect + 6;
    if(!p || !p[0]) return false;
    size_t j = 0;
    for(; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if(!ok) return false;
        if(j + 1 < cap) key[j++] = c;
        else return false;
    }
    key[j] = 0;
    return j > 0;
}

static bool effect_notify_text(const char *effect, char *text, size_t cap)
{
    if(!effect || !text || cap == 0) return false;
    if(strncmp(effect, "notify:", 7) != 0 || !effect[7]) return false;
    clean_line_copy(text, cap, effect + 7);
    return text[0] != 0;
}

static bool action_needs_storage(const lz_local_app_session_t *s)
{
    if(!s) return false;
    char key[20];
    for(int i = 0; i < s->action_count; i++)
        if(effect_counter_key(s->actions[i].effect, key, sizeof key)) return true;
    return false;
}

static bool action_needs_notifications(const lz_local_app_session_t *s)
{
    if(!s) return false;
    char text[LZ_FEEDBACK_BODY_MAX];
    for(int i = 0; i < s->action_count; i++)
        if(effect_notify_text(s->actions[i].effect, text, sizeof text)) return true;
    return false;
}

static bool action_effect_error(const lz_local_app_session_t *s, char *err, size_t cap)
{
    if(!s) return false;
    char key[20];
    char text[LZ_FEEDBACK_BODY_MAX];
    for(int i = 0; i < s->action_count; i++) {
        const char *effect = s->actions[i].effect;
        if(!effect[0]) continue;
        if(strncmp(effect, "counter:", 8) == 0 || strncmp(effect, "count:", 6) == 0) {
            if(effect_counter_key(effect, key, sizeof key)) continue;
            if(err && cap > 0) snprintf(err, cap, "bad action effect");
            return true;
        }
        if(strncmp(effect, "notify:", 7) == 0) {
            if(effect_notify_text(effect, text, sizeof text)) continue;
            if(err && cap > 0) snprintf(err, cap, "bad action effect");
            return true;
        }
        if(err && cap > 0) snprintf(err, cap, "unsupported action effect");
        return true;
    }
    return false;
}

static void template_count(char *out, size_t cap, const char *tmpl, uint32_t count)
{
    if(!out || cap == 0) return;
    out[0] = 0;
    char num[12];
    snprintf(num, sizeof num, "%lu", (unsigned long)count);
    const char *p = tmpl ? tmpl : "";
    while(*p && strlen(out) + 1 < cap) {
        if(strncmp(p, "{count}", 7) == 0) {
            strncat(out, num, cap - strlen(out) - 1);
            p += 7;
        } else {
            size_t n = strlen(out);
            out[n] = *p++;
            out[n + 1] = 0;
        }
    }
}

static bool counter_read_write(const char *data_path, const char *key, uint32_t *value)
{
    if(value) *value = 0;
    if(!data_path || !data_path[0] || !key || !key[0] || !path_is_dir(data_path))
        return false;
    char name[32], path[160];
    snprintf(name, sizeof name, "%s.count", key);
    path_join(path, sizeof path, data_path, name);

    uint32_t count = 0;
    FILE *r = fopen(path, "rb");
    if(r) {
        char buf[24];
        size_t n = fread(buf, 1, sizeof buf - 1, r);
        fclose(r);
        buf[n] = 0;
        unsigned long raw = strtoul(buf, NULL, 10);
        count = raw > UINT32_MAX ? UINT32_MAX : (uint32_t)raw;
    }
    if(count < UINT32_MAX) count++;

    FILE *w = fopen(path, "wb");
    if(!w) return false;
    fprintf(w, "%lu\n", (unsigned long)count);
    fclose(w);
    if(value) *value = count;
    return true;
}

static void refresh_session_data_usage(lz_local_app_session_t *s)
{
    if(!s || !s->storage_ready || !s->data_path[0] || !path_is_dir(s->data_path)) return;
    int entries = 0;
    uint32_t used = 0;
    char err[32];
    err[0] = 0;
    if(app_data_usage_walk(s->data_path, 0, &entries, &used, err, sizeof err))
        s->data_used_bytes = used;
}

static void runtime_count_text(uint32_t *used, const char *text)
{
    if(!used || !text || !text[0]) return;
    size_t len = strlen(text) + 1u;
    if(len > UINT32_MAX - *used) *used = UINT32_MAX;
    else *used += (uint32_t)len;
}

uint32_t lz_local_app_runtime_used(const lz_local_app_session_t *s)
{
    if(!s) return 0;
    uint32_t used = s->entry_source_bytes;
    runtime_count_text(&used, s->title);
    runtime_count_text(&used, s->status);
    runtime_count_text(&used, s->body);
    if(s->storage_ready) runtime_count_text(&used, s->data_path);
    for(int i = 0; i < s->action_count && i < LZ_LOCAL_APP_ACTION_MAX; i++) {
        runtime_count_text(&used, s->actions[i].label);
        runtime_count_text(&used, s->actions[i].status);
        runtime_count_text(&used, s->actions[i].body);
        runtime_count_text(&used, s->actions[i].effect);
    }
    return used;
}

void lz_local_app_runtime_refresh(lz_local_app_session_t *s)
{
    if(!s) return;
    if(!s->runtime_budget_bytes)
        s->runtime_budget_bytes = LZ_LOCAL_APP_RUNTIME_BUDGET_BYTES;
    s->runtime_used_bytes = lz_local_app_runtime_used(s);
}

bool lz_local_app_runtime_within_budget(lz_local_app_session_t *s)
{
    if(!s) return false;
    lz_local_app_runtime_refresh(s);
    if(s->runtime_used_bytes <= s->runtime_budget_bytes) return true;
    snprintf(s->status, sizeof s->status, "Launch blocked");
    snprintf(s->body, sizeof s->body, "App runtime metadata exceeds the SDK memory cap.");
    snprintf(s->error, sizeof s->error, "runtime memory cap exceeded");
    return false;
}
static void app_session_fault(lz_local_app_session_t *out, const char *phase,
                              const char *msg)
{
    if(!out) return;
    snprintf(out->fault, sizeof out->fault, "%s: %s",
             phase && phase[0] ? phase : "runtime",
             msg && msg[0] ? msg : "unknown error");
}

static bool app_session_fail(lz_local_app_session_t *out, const lz_local_app_t *app,
                             const char *msg)
{
    if(out) {
        if(!out->runtime_budget_bytes)
            out->runtime_budget_bytes = LZ_LOCAL_APP_RUNTIME_BUDGET_BYTES;
        if(app && app->name[0]) snprintf(out->title, sizeof out->title, "%s", app->name);
        if(!out->status[0]) snprintf(out->status, sizeof out->status, "Launch blocked");
        snprintf(out->error, sizeof out->error, "%s", msg ? msg : "unknown error");
        lz_local_app_runtime_refresh(out);
        app_session_fault(out, "launch", out->error);
    }
    return false;
}

static bool load_app_manifest(const char *pkg_dir, lz_local_app_t *app,
                              char *reason, size_t reason_cap)
{
    char manifest[136];
    path_join(manifest, sizeof manifest, pkg_dir, "manifest.json");
    FILE *f = fopen(manifest, "rb");
    if(!f) return manifest_fail(reason, reason_cap, "missing manifest");

    size_t cap_json = 1537;
    char *json = (char *)malloc(cap_json);
    if(!json) {
        fclose(f);
        return manifest_fail(reason, reason_cap, "manifest buffer unavailable");
    }
    bool ok = false;
    size_t n = fread(json, 1, cap_json - 1, f);
    fclose(f);
    json[n] = 0;
    if(n == 0) {
        manifest_fail(reason, reason_cap, "empty manifest");
        goto done;
    }
    if(n >= cap_json - 1) {
        manifest_fail(reason, reason_cap, "manifest too large");
        goto done;
    }

    memset(app, 0, sizeof *app);
    app->hue = -1;
    snprintf(app->version, sizeof app->version, "0.0.0");
    snprintf(app->author, sizeof app->author, "local");
    snprintf(app->api_version, sizeof app->api_version, "0.1");
    snprintf(app->icon, sizeof app->icon, "description");
    app->permissions = LZ_APP_PERM_DISPLAY | LZ_APP_PERM_INPUT;

    if(!json_get_string(json, "id", app->id, sizeof app->id)) {
        manifest_fail(reason, reason_cap, "missing id");
        goto done;
    }
    if(!json_get_string(json, "name", app->name, sizeof app->name)) {
        manifest_fail(reason, reason_cap, "missing name");
        goto done;
    }
    if(!json_get_string(json, "entry", app->entry, sizeof app->entry)) {
        manifest_fail(reason, reason_cap, "missing entry");
        goto done;
    }
    json_get_string(json, "version", app->version, sizeof app->version);
    json_get_string(json, "author", app->author, sizeof app->author);
    json_get_string(json, "api_version", app->api_version, sizeof app->api_version);
    if(!json_get_string(json, "summary", app->summary, sizeof app->summary))
        json_get_string(json, "description", app->summary, sizeof app->summary);
    json_get_string(json, "icon", app->icon, sizeof app->icon);
    json_get_int(json, "hue", &app->hue);

    const char *perms = json_value_for(json, "permissions");
    if(perms && !json_parse_permissions_value(perms, &app->permissions)) {
        manifest_fail(reason, reason_cap, "bad permissions");
        goto done;
    }

    if(!safe_id(app->id)) {
        manifest_fail(reason, reason_cap, "unsafe id");
        goto done;
    }
    if(!safe_entry(app->entry)) {
        manifest_fail(reason, reason_cap, "unsafe entry");
        goto done;
    }
    if(!api_version_supported(app->api_version)) {
        manifest_fail(reason, reason_cap, "unsupported SDK");
        goto done;
    }
    if(app->hue < -1 || app->hue > 359) app->hue = -1;

    char entry_path[160];
    path_join(entry_path, sizeof entry_path, pkg_dir, app->entry);
    if(!path_is_file(entry_path)) {
        manifest_fail(reason, reason_cap, "missing entry file");
        goto done;
    }

    snprintf(app->path, sizeof app->path, "%s", pkg_dir);
    ok = true;
done:
    free(json);
    return ok;
}

static bool app_install_paths(const char *id, char *apps, size_t apps_cap,
                              char *live, size_t live_cap,
                              char *staging, size_t staging_cap,
                              char *backup, size_t backup_cap,
                              char *err, int err_cap)
{
    if(!safe_id(id) || strlen(id) > LZ_APP_INSTALL_ID_MAX) {
        set_err(err, err_cap, "unsafe id");
        return false;
    }

    char staging_name[40];
    char backup_name[40];
    int sn = snprintf(staging_name, sizeof staging_name, "%s%s", LZ_APP_STAGING_PREFIX, id);
    int bn = snprintf(backup_name, sizeof backup_name, "%s%s", LZ_APP_BACKUP_PREFIX, id);
    if(sn < 0 || sn >= (int)sizeof staging_name || bn < 0 || bn >= (int)sizeof backup_name) {
        set_err(err, err_cap, "path too long");
        return false;
    }

    char root[128];
    if(!app_install_root(root, sizeof root, err, err_cap)) return false;
    path_join(apps, apps_cap, root, "apps");
    path_join(live, live_cap, apps, id);
    path_join(staging, staging_cap, apps, staging_name);
    path_join(backup, backup_cap, apps, backup_name);
    return true;
}

static bool copy_install_path(char *out, int out_cap, const char *path,
                              char *err, int err_cap)
{
    if(!out || out_cap <= 0) return true;
    int n = snprintf(out, (size_t)out_cap, "%s", path);
    if(n < 0 || n >= out_cap) {
        out[0] = 0;
        set_err(err, err_cap, "path too long");
        return false;
    }
    return true;
}

bool lz_store_prepare_app_install(const char *id, char *staging_path, int staging_cap,
                                  char *live_path, int live_cap,
                                  char *err, int err_cap)
{
    if(staging_path && staging_cap > 0) staging_path[0] = 0;
    if(live_path && live_cap > 0) live_path[0] = 0;
    set_err(err, err_cap, "");

    char apps[128], live[160], staging[160], backup[160];
    if(!app_install_paths(id, apps, sizeof apps, live, sizeof live,
                          staging, sizeof staging, backup, sizeof backup,
                          err, err_cap))
        return false;
    if(!path_mkdir(apps) || !path_is_dir(apps)) {
        set_err(err, err_cap, "apps mkdir failed");
        return false;
    }
    if(!remove_tree(staging, err, err_cap)) return false;
    if(!path_mkdir(staging) || !path_is_dir(staging)) {
        set_err(err, err_cap, "staging mkdir failed");
        return false;
    }
    if(!copy_install_path(staging_path, staging_cap, staging, err, err_cap)) return false;
    if(!copy_install_path(live_path, live_cap, live, err, err_cap)) return false;
    return true;
}

bool lz_store_discard_app_install(const char *id, char *err, int err_cap)
{
    set_err(err, err_cap, "");
    char apps[128], live[160], staging[160], backup[160];
    if(!app_install_paths(id, apps, sizeof apps, live, sizeof live,
                          staging, sizeof staging, backup, sizeof backup,
                          err, err_cap))
        return false;
    return remove_tree(staging, err, err_cap);
}

bool lz_store_promote_app_install(const char *id, char *err, int err_cap)
{
    set_err(err, err_cap, "");
    char apps[128], live[160], staging[160], backup[160];
    if(!app_install_paths(id, apps, sizeof apps, live, sizeof live,
                          staging, sizeof staging, backup, sizeof backup,
                          err, err_cap))
        return false;
    if(!path_is_dir(staging)) {
        set_err(err, err_cap, "staging missing");
        return false;
    }

    lz_local_app_t app;
    char reason[48] = "invalid manifest";
    if(!load_app_manifest(staging, &app, reason, sizeof reason)) {
        set_err(err, err_cap, reason);
        return false;
    }
    if(strcmp(app.id, id) != 0) {
        set_err(err, err_cap, "id mismatch");
        return false;
    }
    if(!path_mkdir(apps) || !path_is_dir(apps)) {
        set_err(err, err_cap, "apps mkdir failed");
        return false;
    }
    if(path_is_file(live)) {
        set_err(err, err_cap, "live not dir");
        return false;
    }
    if(!remove_tree(backup, err, err_cap)) return false;

    bool had_live = path_is_dir(live);
    if(had_live && rename(live, backup) != 0) {
        set_err(err, err_cap, "backup failed");
        return false;
    }
    if(rename(staging, live) != 0) {
        if(had_live) rename(backup, live);
        set_err(err, err_cap, "promote failed");
        return false;
    }
    if(had_live) {
        char cleanup_err[32];
        remove_tree(backup, cleanup_err, sizeof cleanup_err);
    }
    return true;
}

#define LZ_ZIP_LOCAL_SIG 0x04034b50u
#define LZ_ZIP_CENTRAL_SIG 0x02014b50u
#define LZ_ZIP_EOCD_SIG 0x06054b50u
#define LZ_ZIP_METHOD_STORED 0u
#define LZ_ZIP_FLAG_ENCRYPTED 0x0001u
#define LZ_ZIP_FLAG_DATA_DESCRIPTOR 0x0008u
#define LZ_APP_PACKAGE_MAX_FILES 24
#define LZ_APP_PACKAGE_MAX_FILE_BYTES (256u * 1024u)

static bool app_package_file_path_ok(const char *path)
{
    if(!safe_entry(path)) return false;
    if(path[0] == '.' || path[0] == 0) return false;
    if(strcmp(path, "data") == 0 || strncmp(path, "data/", 5) == 0) return false;
    const char *seg = path;
    while(*seg) {
        if(*seg == '/' || *seg == '.') return false;
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if(len == 0 || (len == 1 && seg[0] == '.')) return false;
        seg = slash ? slash + 1 : seg + len;
    }
    return true;
}

static bool app_package_dir_path_ok(const char *path)
{
    if(!path || !path[0]) return false;
    char tmp[80];
    if(strlen(path) >= sizeof tmp) return false;
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    while(len > 0 && tmp[len - 1] == '/') tmp[--len] = 0;
    return len > 0 && app_package_file_path_ok(tmp);
}

static uint16_t zip_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t zip_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool app_package_read_exact(FILE *f, void *dst, size_t len,
                                   char *err, int err_cap)
{
    if(len == 0) return true;
    if(!f || !dst || fread(dst, 1, len, f) != len) {
        set_err(err, err_cap, "package read failed");
        return false;
    }
    return true;
}

static bool app_package_skip_bytes(FILE *f, uint32_t bytes, char *err, int err_cap)
{
    uint8_t buf[128];
    uint32_t left = bytes;
    while(left > 0) {
        size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
        if(fread(buf, 1, want, f) != want) {
            set_err(err, err_cap, "package truncated");
            return false;
        }
        left -= (uint32_t)want;
    }
    return true;
}

static bool app_package_mkdir_parents(const char *path, char *err, int err_cap)
{
    char tmp[180];
    snprintf(tmp, sizeof tmp, "%s", path);
    for(char *p = tmp; *p; p++) {
        if(*p != '/' && *p != '\\') continue;
        char saved = *p;
        *p = 0;
        if(tmp[0] && !path_mkdir(tmp)) {
            set_err(err, err_cap, "mkdir failed");
            return false;
        }
        *p = saved;
    }
    return true;
}

static bool app_package_copy_bytes(FILE *in, const char *dest, uint32_t bytes,
                                   uint32_t *total, char *err, int err_cap)
{
    if(!app_package_mkdir_parents(dest, err, err_cap)) return false;
    FILE *out = fopen(dest, "wb");
    if(!out) {
        set_err(err, err_cap, "file create failed");
        return false;
    }
    uint8_t buf[256];
    uint32_t left = bytes;
    while(left > 0) {
        size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
        size_t got = fread(buf, 1, want, in);
        if(got != want) {
            fclose(out);
            set_err(err, err_cap, "package truncated");
            return false;
        }
        if(fwrite(buf, 1, got, out) != got) {
            fclose(out);
            set_err(err, err_cap, "file write failed");
            return false;
        }
        left -= (uint32_t)got;
        if(total) *total += (uint32_t)got;
    }
    fclose(out);
    return true;
}

static bool lz_store_extract_app_package(const char *package_path, const char *staging,
                                         lz_app_package_install_t *out,
                                         char *err, int err_cap)
{
    FILE *f = fopen(package_path, "rb");
    if(!f) {
        set_err(err, err_cap, "package missing");
        return false;
    }

    int files = 0;
    uint32_t total = 0;
    bool saw_manifest = false;
    for(;;) {
        uint8_t sigb[4];
        size_t got = fread(sigb, 1, sizeof sigb, f);
        if(got == 0) break;
        if(got != sizeof sigb) {
            fclose(f);
            set_err(err, err_cap, "package truncated");
            return false;
        }
        uint32_t sig = zip_u32(sigb);
        if(sig == LZ_ZIP_CENTRAL_SIG || sig == LZ_ZIP_EOCD_SIG) break;
        if(sig != LZ_ZIP_LOCAL_SIG) {
            fclose(f);
            set_err(err, err_cap, "bad zip header");
            return false;
        }

        uint8_t hdr[26];
        if(!app_package_read_exact(f, hdr, sizeof hdr, err, err_cap)) {
            fclose(f);
            return false;
        }
        uint16_t flags = zip_u16(hdr + 2);
        uint16_t method = zip_u16(hdr + 4);
        uint32_t comp_size = zip_u32(hdr + 14);
        uint32_t uncomp_size = zip_u32(hdr + 18);
        uint16_t name_len = zip_u16(hdr + 22);
        uint16_t extra_len = zip_u16(hdr + 24);

        if((flags & (LZ_ZIP_FLAG_ENCRYPTED | LZ_ZIP_FLAG_DATA_DESCRIPTOR)) != 0) {
            fclose(f);
            set_err(err, err_cap, "unsupported zip flags");
            return false;
        }
        if(method != LZ_ZIP_METHOD_STORED) {
            fclose(f);
            set_err(err, err_cap, "unsupported zip method");
            return false;
        }
        if(comp_size != uncomp_size || uncomp_size > LZ_APP_PACKAGE_MAX_FILE_BYTES) {
            fclose(f);
            set_err(err, err_cap, "bad file size");
            return false;
        }
        if(name_len == 0 || name_len >= 80 || extra_len > 1024) {
            fclose(f);
            set_err(err, err_cap, "bad zip name");
            return false;
        }

        char rel[80];
        if(!app_package_read_exact(f, rel, name_len, err, err_cap)) {
            fclose(f);
            return false;
        }
        rel[name_len] = 0;
        if(!app_package_skip_bytes(f, extra_len, err, err_cap)) {
            fclose(f);
            return false;
        }

        bool is_dir = rel[name_len - 1] == '/';
        if(is_dir) {
            if(comp_size != 0 || !app_package_dir_path_ok(rel)) {
                fclose(f);
                set_err(err, err_cap, "bad file path");
                return false;
            }
            char clean[80];
            snprintf(clean, sizeof clean, "%s", rel);
            size_t clen = strlen(clean);
            while(clen > 0 && clean[clen - 1] == '/') clean[--clen] = 0;
            char dir[180];
            path_join(dir, sizeof dir, staging, clean);
            if(!app_package_mkdir_parents(dir, err, err_cap) || !path_mkdir(dir)) {
                fclose(f);
                set_err(err, err_cap, "mkdir failed");
                return false;
            }
            continue;
        }

        if(!app_package_file_path_ok(rel)) {
            fclose(f);
            set_err(err, err_cap, "bad file path");
            return false;
        }
        if(++files > LZ_APP_PACKAGE_MAX_FILES) {
            fclose(f);
            set_err(err, err_cap, "too many files");
            return false;
        }
        if(total > LZ_APP_PACKAGE_MAX_BYTES || comp_size > LZ_APP_PACKAGE_MAX_BYTES - total) {
            fclose(f);
            set_err(err, err_cap, "package too large");
            return false;
        }
        char dest[180];
        path_join(dest, sizeof dest, staging, rel);
        if(!app_package_copy_bytes(f, dest, comp_size, &total, err, err_cap)) {
            fclose(f);
            return false;
        }
        if(strcmp(rel, "manifest.json") == 0) saw_manifest = true;
    }
    fclose(f);
    if(!saw_manifest) {
        set_err(err, err_cap, "missing manifest");
        return false;
    }
    if(out) {
        out->file_count = (uint16_t)files;
        out->extracted_bytes = total;
    }
    return true;
}

bool lz_store_install_app_package(const char *id, const char *package_path,
                                  const char *sha256, uint32_t package_bytes,
                                  lz_app_package_install_t *out)
{
    lz_app_package_install_t r;
    memset(&r, 0, sizeof r);
    if(id) snprintf(r.id, sizeof r.id, "%s", id);
    r.package_bytes = package_bytes;
    if(package_path) snprintf(r.package_path, sizeof r.package_path, "%s", package_path);
    char err[48] = "";

    if(!id || !package_path || !sha256 || package_bytes == 0) {
        set_err(err, sizeof err, "missing package args");
        goto fail;
    }
    if(package_bytes > LZ_APP_PACKAGE_MAX_BYTES) {
        set_err(err, sizeof err, "package too large");
        goto fail;
    }
    struct stat st;
    if(stat(package_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        set_err(err, sizeof err, "package missing");
        goto fail;
    }
    if(st.st_size < 0 || (uint32_t)st.st_size != package_bytes) {
        set_err(err, sizeof err, "size mismatch");
        goto fail;
    }
    if(!lz_store_verify_file_sha256(package_path, sha256, err, sizeof err))
        goto fail;

    char staging[160], live[160];
    if(!lz_store_prepare_app_install(id, staging, sizeof staging,
                                     live, sizeof live, err, sizeof err))
        goto fail;
    if(!lz_store_extract_app_package(package_path, staging, &r, err, sizeof err)) {
        char discard[32];
        lz_store_discard_app_install(id, discard, sizeof discard);
        goto fail;
    }
    if(!lz_store_promote_app_install(id, err, sizeof err)) {
        char discard[32];
        lz_store_discard_app_install(id, discard, sizeof discard);
        goto fail;
    }

    lz_local_app_t app;
    char reason[48] = "";
    if(load_app_manifest(live, &app, reason, sizeof reason)) {
        snprintf(r.version, sizeof r.version, "%s", app.version);
    }
    r.ok = true;
    if(out) *out = r;
    return true;

fail:
    snprintf(r.error, sizeof r.error, "%s", err[0] ? err : "install failed");
    if(out) *out = r;
    return false;
}

static bool app_catalog_find_entry(const char *id, lz_app_catalog_entry_t *entry,
                                   char *err, int err_cap)
{
    if(!id || !id[0] || !safe_id(id)) {
        set_err(err, err_cap, "bad catalog id");
        return false;
    }
    lz_app_catalog_entry_t *catalog =
        (lz_app_catalog_entry_t *)calloc(LZ_APP_CATALOG_MAX_APPS, sizeof(lz_app_catalog_entry_t));
    if(!catalog) {
        set_err(err, err_cap, "catalog buffer unavailable");
        return false;
    }
    lz_app_catalog_report_t report;
    memset(&report, 0, sizeof report);
    int n = lz_store_load_app_catalog(catalog, LZ_APP_CATALOG_MAX_APPS, &report);
    if(n <= 0) {
        set_err(err, err_cap, report.first_error[0] ? report.first_error : "catalog missing");
        free(catalog);
        return false;
    }
    for(int i = 0; i < n; i++) {
        if(strcmp(catalog[i].id, id) == 0) {
            if(entry) *entry = catalog[i];
            free(catalog);
            return true;
        }
    }
    free(catalog);
    set_err(err, err_cap, "catalog app missing");
    return false;
}

static bool app_catalog_package_paths(const lz_app_catalog_entry_t *entry,
                                      char *path, int path_cap,
                                      char *tmp, int tmp_cap,
                                      char *err, int err_cap)
{
    if(!g_persist) {
        set_err(err, err_cap, "storage unavailable");
        return false;
    }
    char dir[128];
    path_join(dir, sizeof dir, g_dir, "packages");
    if(!path_mkdir(dir)) {
        set_err(err, err_cap, "package cache mkdir failed");
        return false;
    }
    char name[40];
    snprintf(name, sizeof name, "%s.zip", entry->id);
    if((int)strlen(name) >= (int)sizeof name) {
        set_err(err, err_cap, "package name too long");
        return false;
    }
    path_join(path, (size_t)path_cap, dir, name);
    if((int)strlen(path) + 4 >= tmp_cap) {
        set_err(err, err_cap, "package path too long");
        return false;
    }
    snprintf(tmp, (size_t)tmp_cap, "%s.tmp", path);
    return true;
}

bool lz_store_install_app_catalog_entry(const char *id, const char *package_path,
                                        lz_app_package_install_t *out)
{
    lz_app_package_install_t *r = (lz_app_package_install_t *)calloc(1, sizeof *r);
    lz_app_catalog_entry_t *entry = (lz_app_catalog_entry_t *)malloc(sizeof *entry);
    char *selected = (char *)malloc(160);
    char *tmp = (char *)malloc(168);
    char err[48] = "";
    bool result = false;
    if(!r || !entry || !selected || !tmp) {
        set_err(err, sizeof err, "catalog install buffer unavailable");
        goto fail;
    }
    if(id) snprintf(r->id, sizeof r->id, "%s", id);
    selected[0] = 0;
    tmp[0] = 0;

    if(!app_catalog_find_entry(id, entry, err, sizeof err))
        goto fail;

    bool fetched = false;
    if(package_path && package_path[0]) {
        if(snprintf(selected, 160, "%s", package_path) >= 160) {
            set_err(err, sizeof err, "package path too long");
            goto fail;
        }
    } else {
        if(!app_catalog_package_paths(entry, selected, 160, tmp, 168, err, sizeof err))
            goto fail;
        remove(tmp);
        uint32_t fetched_bytes = 0;
        if(!lz_app_package_fetch(entry->package_url, tmp, entry->package_bytes,
                                 LZ_APP_PACKAGE_MAX_BYTES, &fetched_bytes,
                                 err, sizeof err)) {
            remove(tmp);
            goto fail;
        }
        if(fetched_bytes != entry->package_bytes) {
            remove(tmp);
            set_err(err, sizeof err, "size mismatch");
            goto fail;
        }
        remove(selected);
        if(rename(tmp, selected) != 0) {
            remove(tmp);
            set_err(err, sizeof err, "package commit failed");
            goto fail;
        }
        fetched = true;
    }

    if(!lz_store_install_app_package(entry->id, selected, entry->package_sha256,
                                     entry->package_bytes, r)) {
        r->fetched = fetched;
        snprintf(r->package_path, sizeof r->package_path, "%s", selected);
        goto done;
    }
    r->fetched = fetched;
    snprintf(r->package_path, sizeof r->package_path, "%s", selected);
    result = true;
    goto done;

fail:
    if(r) snprintf(r->error, sizeof r->error, "%s",
                   err[0] ? err : "catalog install failed");
done:
    if(out && r) *out = *r;
    free(tmp);
    free(selected);
    free(entry);
    free(r);
    return result;
}

static bool app_package_write_u16(FILE *f, uint16_t v)
{
    return fputc(v & 0xff, f) != EOF && fputc((v >> 8) & 0xff, f) != EOF;
}

static bool app_package_write_u32(FILE *f, uint32_t v)
{
    return fputc(v & 0xff, f) != EOF && fputc((v >> 8) & 0xff, f) != EOF &&
           fputc((v >> 16) & 0xff, f) != EOF && fputc((v >> 24) & 0xff, f) != EOF;
}

static bool app_package_write_local(FILE *f, const char *name, const char *body,
                                    uint16_t method)
{
    size_t name_len = name ? strlen(name) : 0;
    size_t body_len = body ? strlen(body) : 0;
    if(!f || name_len == 0 || name_len > 0xffffu || body_len > 0xffffffffu) return false;
    if(!app_package_write_u32(f, LZ_ZIP_LOCAL_SIG) ||
       !app_package_write_u16(f, 20) || !app_package_write_u16(f, 0) ||
       !app_package_write_u16(f, method) ||
       !app_package_write_u16(f, 0) || !app_package_write_u16(f, 0) ||
       !app_package_write_u32(f, 0) ||
       !app_package_write_u32(f, (uint32_t)body_len) ||
       !app_package_write_u32(f, (uint32_t)body_len) ||
       !app_package_write_u16(f, (uint16_t)name_len) ||
       !app_package_write_u16(f, 0))
        return false;
    if(fwrite(name, 1, name_len, f) != name_len) return false;
    if(body_len > 0 && fwrite(body, 1, body_len, f) != body_len) return false;
    return true;
}

static bool app_package_selftest_write_zip(const char *path, const char *id,
                                           const char *version,
                                           const char *extra_path,
                                           uint16_t main_method)
{
    FILE *f = fopen(path, "wb");
    if(!f) return false;
    char manifest[360];
    snprintf(manifest, sizeof manifest,
             "{\"id\":\"%s\",\"name\":\"Package Selftest\",\"version\":\"%s\","
             "\"author\":\"Limitless\",\"entry\":\"main.lua\","
             "\"icon\":\"package\",\"hue\":84,\"api_version\":\"0.1\","
             "\"permissions\":[\"display\"],\"summary\":\"Installer selftest\"}",
             id, version);
    bool ok = app_package_write_local(f, "manifest.json", manifest, LZ_ZIP_METHOD_STORED) &&
              app_package_write_local(f, "main.lua",
                                      "-- title: Package Selftest\n"
                                      "-- body: Installed from package\n"
                                      "return true\n",
                                      main_method);
    if(ok && extra_path) {
        ok = app_package_write_local(f, extra_path, "extra package payload\n",
                                     LZ_ZIP_METHOD_STORED);
    }
    ok = ok && app_package_write_u32(f, LZ_ZIP_EOCD_SIG) &&
         app_package_write_u16(f, 0) && app_package_write_u16(f, 0) &&
         app_package_write_u16(f, 0) && app_package_write_u16(f, 0) &&
         app_package_write_u32(f, 0) && app_package_write_u32(f, 0) &&
         app_package_write_u16(f, 0);
    fclose(f);
    if(!ok) remove(path);
    return ok;
}

static bool app_package_file_size_u32(const char *path, uint32_t *out)
{
    struct stat st;
    if(stat(path, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size < 0 ||
       st.st_size > (long)LZ_APP_PACKAGE_MAX_BYTES)
        return false;
    if(out) *out = (uint32_t)st.st_size;
    return true;
}

static bool app_package_hash_and_size(const char *path, char *sha, int sha_cap,
                                      uint32_t *bytes, char *err, int err_cap)
{
    if(!app_package_file_size_u32(path, bytes)) {
        set_err(err, err_cap, "size failed");
        return false;
    }
    return lz_store_file_sha256(path, sha, sha_cap, err, err_cap);
}

static bool app_package_selftest_live_version(const char *id, const char *version)
{
    char apps[128], live[160], staging[160], backup[160], err[48];
    if(!app_install_paths(id, apps, sizeof apps, live, sizeof live,
                          staging, sizeof staging, backup, sizeof backup,
                          err, sizeof err))
        return false;
    lz_local_app_t app;
    char reason[48];
    return load_app_manifest(live, &app, reason, sizeof reason) &&
           strcmp(app.id, id) == 0 && strcmp(app.version, version) == 0;
}

static void app_package_selftest_cleanup(const char *id, const char *package_path)
{
    if(package_path && package_path[0]) remove(package_path);
    char apps[128], live[160], staging[160], backup[160], err[48];
    if(app_install_paths(id, apps, sizeof apps, live, sizeof live,
                         staging, sizeof staging, backup, sizeof backup,
                         err, sizeof err)) {
        remove_tree(staging, err, sizeof err);
        remove_tree(backup, err, sizeof err);
        remove_tree(live, err, sizeof err);
    }
}

static bool app_catalog_selftest_save_catalog(const char *id, const char *version,
                                              const char *sha, uint32_t bytes,
                                              char *err, int err_cap)
{
    size_t cap_json = 1024;
    char *json = (char *)malloc(cap_json);
    if(!json) {
        set_err(err, err_cap, "catalog buffer unavailable");
        return false;
    }
    int n = snprintf(json, cap_json,
        "{\"schema\":\"%s\",\"apps\":[{"
        "\"id\":\"%s\",\"name\":\"Package Selftest\",\"version\":\"%s\","
        "\"author\":\"Limitless\",\"summary\":\"Catalog installer selftest\","
        "\"description\":\"Catalog installer selftest\",\"icon\":\"package\","
        "\"hue\":84,\"api_version\":\"0.1\","
        "\"package_url\":\"https://apps.example.invalid/%s.zip\","
        "\"package_sha256\":\"%s\",\"package_bytes\":%lu,"
        "\"compatibility\":{\"api_versions\":[\"0.1\"],\"targets\":[\"tdeck\",\"sim\"]},"
        "\"permissions\":[\"display\"]}]}",
        LZ_APP_CATALOG_SCHEMA, id, version, id, sha, (unsigned long)bytes);
    if(n <= 0 || n >= (int)cap_json) {
        set_err(err, err_cap, "catalog too large");
        free(json);
        return false;
    }
    bool ok = lz_store_save_app_catalog_cache(json, n, err, err_cap);
    free(json);
    return ok;
}

int lz_store_app_catalog_install_selftest(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    const char *id = "lz.cat.selftest";
    char root[128], err[48] = "";
    if(!app_install_root(root, sizeof root, err, sizeof err))
        return snprintf(buf, (size_t)n, "App catalog install selftest: SKIP %s\n",
                        err[0] ? err : "storage unavailable");
    if(!path_mkdir(root))
        return snprintf(buf, (size_t)n, "App catalog install selftest: SKIP root unavailable\n");

    char *prev_catalog = (char *)malloc(LZ_APP_CATALOG_CACHE_MAX + 1);
    if(!prev_catalog)
        return snprintf(buf, (size_t)n,
                        "App catalog install selftest: SKIP catalog buffer unavailable\n");
    int prev_len = 0;
    char prev_err[48] = "";
    bool had_prev_catalog = lz_store_load_app_catalog_cache(prev_catalog,
                                                            LZ_APP_CATALOG_CACHE_MAX + 1,
                                                            &prev_len, prev_err, sizeof prev_err);

    char package_path[160];
    path_join(package_path, sizeof package_path, root, ".lz-cat-selftest.zip");
    app_package_selftest_cleanup(id, package_path);

    bool ok = true;
    char detail[64] = "";
    char sha[65];
    uint32_t bytes = 0;
    lz_app_package_install_t r;

    #define CAT_INSTALL_CHECK(cond, msg) do { \
        if(ok && !(cond)) { ok = false; snprintf(detail, sizeof detail, "%s", msg); } \
    } while(0)

    CAT_INSTALL_CHECK(app_package_selftest_write_zip(package_path, id, "1.0.0", NULL,
                                                     LZ_ZIP_METHOD_STORED),
                      "write v1");
    CAT_INSTALL_CHECK(app_package_hash_and_size(package_path, sha, sizeof sha, &bytes,
                                                err, sizeof err),
                      "hash v1");
    CAT_INSTALL_CHECK(app_catalog_selftest_save_catalog(id, "1.0.0", sha, bytes,
                                                        err, sizeof err),
                      "catalog v1");
    memset(&r, 0, sizeof r);
    CAT_INSTALL_CHECK(lz_store_install_app_catalog_entry(id, package_path, &r) &&
                      r.ok && strcmp(r.version, "1.0.0") == 0 && !r.fetched &&
                      app_package_selftest_live_version(id, "1.0.0"),
                      "install v1");

    static const char zero_sha[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    CAT_INSTALL_CHECK(app_package_selftest_write_zip(package_path, id, "1.1.0", NULL,
                                                     LZ_ZIP_METHOD_STORED),
                      "write bad update");
    CAT_INSTALL_CHECK(app_package_hash_and_size(package_path, sha, sizeof sha, &bytes,
                                                err, sizeof err),
                      "hash bad update");
    CAT_INSTALL_CHECK(app_catalog_selftest_save_catalog(id, "1.1.0", zero_sha, bytes,
                                                        err, sizeof err),
                      "catalog bad hash");
    memset(&r, 0, sizeof r);
    CAT_INSTALL_CHECK(!lz_store_install_app_catalog_entry(id, package_path, &r) &&
                      strcmp(r.error, "sha mismatch") == 0 &&
                      app_package_selftest_live_version(id, "1.0.0"),
                      "bad hash rollback");

    CAT_INSTALL_CHECK(app_package_selftest_write_zip(package_path, id, "2.0.0",
                                                     "assets/readme.txt",
                                                     LZ_ZIP_METHOD_STORED),
                      "write v2");
    CAT_INSTALL_CHECK(app_package_hash_and_size(package_path, sha, sizeof sha, &bytes,
                                                err, sizeof err),
                      "hash v2");
    CAT_INSTALL_CHECK(app_catalog_selftest_save_catalog(id, "2.0.0", sha, bytes,
                                                        err, sizeof err),
                      "catalog v2");
    memset(&r, 0, sizeof r);
    CAT_INSTALL_CHECK(lz_store_install_app_catalog_entry(id, package_path, &r) &&
                      r.ok && strcmp(r.version, "2.0.0") == 0 && r.file_count == 3 &&
                      app_package_selftest_live_version(id, "2.0.0"),
                      "update v2");

    #undef CAT_INSTALL_CHECK

    app_package_selftest_cleanup(id, package_path);
    if(had_prev_catalog)
        lz_store_save_app_catalog_cache(prev_catalog, prev_len, err, sizeof err);
    else
        lz_store_clear_app_catalog_cache(err, sizeof err);
    free(prev_catalog);

    return snprintf(buf, (size_t)n,
                    "App catalog install selftest: %s version=%s files=%u%s%s\n",
                    ok ? "PASS" : "FAIL",
                    ok ? "2.0.0" : "-",
                    ok ? (unsigned)r.file_count : 0u,
                    detail[0] ? " error=" : "",
                    detail);
}

int lz_store_app_package_selftest(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    const char *id = "lz.pkg.selftest";
    char root[128], err[48] = "";
    if(!app_install_root(root, sizeof root, err, sizeof err))
        return snprintf(buf, (size_t)n, "App package selftest: SKIP %s\n",
                        err[0] ? err : "storage unavailable");
    if(!path_mkdir(root))
        return snprintf(buf, (size_t)n, "App package selftest: SKIP root unavailable\n");

    char package_path[160];
    path_join(package_path, sizeof package_path, root, ".lz-pkg-selftest.zip");
    app_package_selftest_cleanup(id, package_path);

    bool ok = true;
    char detail[64] = "";
    char sha[65];
    uint32_t bytes = 0;
    lz_app_package_install_t r;

    #define PKG_CHECK(cond, msg) do { \
        if(ok && !(cond)) { ok = false; snprintf(detail, sizeof detail, "%s", msg); } \
    } while(0)

    PKG_CHECK(app_package_selftest_write_zip(package_path, id, "1.0.0", NULL,
                                             LZ_ZIP_METHOD_STORED),
              "write v1");
    PKG_CHECK(app_package_hash_and_size(package_path, sha, sizeof sha, &bytes,
                                        err, sizeof err),
              "hash v1");
    PKG_CHECK(lz_store_install_app_package(id, package_path, sha, bytes, &r) &&
              r.ok && strcmp(r.version, "1.0.0") == 0 && r.file_count == 2,
              "install v1");
    PKG_CHECK(app_package_selftest_live_version(id, "1.0.0"),
              "live v1");

    static const char zero_sha[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    memset(&r, 0, sizeof r);
    PKG_CHECK(!lz_store_install_app_package(id, package_path, zero_sha, bytes, &r) &&
              strcmp(r.error, "sha mismatch") == 0 &&
              app_package_selftest_live_version(id, "1.0.0"),
              "bad hash rollback");

    PKG_CHECK(app_package_selftest_write_zip(package_path, "lz.pkg.wrong", "9.9.9",
                                             NULL, LZ_ZIP_METHOD_STORED),
              "write wrong id");
    PKG_CHECK(app_package_hash_and_size(package_path, sha, sizeof sha, &bytes,
                                        err, sizeof err),
              "hash wrong id");
    memset(&r, 0, sizeof r);
    PKG_CHECK(!lz_store_install_app_package(id, package_path, sha, bytes, &r) &&
              strcmp(r.error, "id mismatch") == 0 &&
              app_package_selftest_live_version(id, "1.0.0"),
              "id mismatch rollback");

    PKG_CHECK(app_package_selftest_write_zip(package_path, id, "1.1.0",
                                             "data/cache.bin", LZ_ZIP_METHOD_STORED),
              "write bad path");
    PKG_CHECK(app_package_hash_and_size(package_path, sha, sizeof sha, &bytes,
                                        err, sizeof err),
              "hash bad path");
    memset(&r, 0, sizeof r);
    PKG_CHECK(!lz_store_install_app_package(id, package_path, sha, bytes, &r) &&
              strcmp(r.error, "bad file path") == 0 &&
              app_package_selftest_live_version(id, "1.0.0"),
              "path rollback");

    PKG_CHECK(app_package_selftest_write_zip(package_path, id, "1.2.0", NULL, 8),
              "write deflated");
    PKG_CHECK(app_package_hash_and_size(package_path, sha, sizeof sha, &bytes,
                                        err, sizeof err),
              "hash deflated");
    memset(&r, 0, sizeof r);
    PKG_CHECK(!lz_store_install_app_package(id, package_path, sha, bytes, &r) &&
              strcmp(r.error, "unsupported zip method") == 0 &&
              app_package_selftest_live_version(id, "1.0.0"),
              "method rollback");

    PKG_CHECK(app_package_selftest_write_zip(package_path, id, "2.0.0",
                                             "assets/readme.txt", LZ_ZIP_METHOD_STORED),
              "write v2");
    PKG_CHECK(app_package_hash_and_size(package_path, sha, sizeof sha, &bytes,
                                        err, sizeof err),
              "hash v2");
    memset(&r, 0, sizeof r);
    PKG_CHECK(lz_store_install_app_package(id, package_path, sha, bytes, &r) &&
              r.ok && strcmp(r.version, "2.0.0") == 0 && r.file_count == 3 &&
              app_package_selftest_live_version(id, "2.0.0"),
              "update v2");

    #undef PKG_CHECK

    app_package_selftest_cleanup(id, package_path);
    return snprintf(buf, (size_t)n,
                    "App package selftest: %s version=%s files=%u%s%s\n",
                    ok ? "PASS" : "FAIL",
                    ok ? "2.0.0" : "-",
                    ok ? (unsigned)r.file_count : 0u,
                    detail[0] ? " error=" : "",
                    detail);
}

static void scan_app_root(const char *apps_dir, lz_local_app_t *out, int cap, int *count)
{
    if(!apps_dir || !out || !count || *count >= cap) return;
    DIR *d = opendir(apps_dir);
    if(!d) return;

    struct dirent *e;
    while(*count < cap && (e = readdir(d)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if(app_install_temp_name(e->d_name)) continue;
        if(app_retained_name(e->d_name)) continue;
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
    if(!out || cap <= 0) return 0;
    int count = 0;

    char dir[128];
    if(g_persist) {
        path_join(dir, sizeof dir, g_dir, "apps");
        scan_app_root(dir, out, cap, &count);

        const char *root = lz_store_file_root();
        if(root && strcmp(root, g_dir) != 0 && count < cap) {
            path_join(dir, sizeof dir, root, "apps");
            scan_app_root(dir, out, cap, &count);
        }
    }

    if(g_appfs_dir[0] && count < cap) {
        path_join(dir, sizeof dir, g_appfs_dir, "apps");
        scan_app_root(dir, out, cap, &count);
    }

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
        if(app_install_temp_name(e->d_name)) continue;
        if(app_retained_name(e->d_name)) continue;
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
    if(!out || cap <= 0) return 0;
    int count = 0;

    char dir[128];
    if(g_persist) {
        path_join(dir, sizeof dir, g_dir, "apps");
        scan_app_issue_root(dir, out, cap, &count);

        const char *root = lz_store_file_root();
        if(root && strcmp(root, g_dir) != 0 && count < cap) {
            path_join(dir, sizeof dir, root, "apps");
            scan_app_issue_root(dir, out, cap, &count);
        }
    }

    if(g_appfs_dir[0] && count < cap) {
        path_join(dir, sizeof dir, g_appfs_dir, "apps");
        scan_app_issue_root(dir, out, cap, &count);
    }

    return count;
}

/* ---- OTA firmware manifest diagnostics ---- */

static bool ota_fail(lz_ota_manifest_t *out, const char *msg)
{
    if(out) {
        out->valid = false;
        snprintf(out->error, sizeof out->error, "%s", msg ? msg : "invalid manifest");
    }
    return false;
}

static bool safe_ota_token(const char *s)
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

static bool safe_http_url(const char *s)
{
    if(!s || !s[0]) return false;
    bool ok_scheme = strncmp(s, "https://", 8) == 0 || strncmp(s, "http://", 7) == 0;
    if(!ok_scheme) return false;
    for(int i = 0; s[i]; i++)
        if((unsigned char)s[i] <= 32 || s[i] == '"' || s[i] == '\\') return false;
    return true;
}

static bool hex64(const char *s)
{
    if(!s) return false;
    for(int i = 0; i < 64; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if(!ok) return false;
    }
    return s[64] == 0;
}

static bool parse_ota_manifest_json(const char *json, lz_ota_manifest_t *out)
{
    char schema[36];
    if(!json || !out) return false;
    out->found = true;
    out->valid = false;
    out->error[0] = 0;

    if(!json_get_string(json, "schema", schema, sizeof schema))
        return ota_fail(out, "missing schema");
    if(strcmp(schema, LZ_OTA_MANIFEST_SCHEMA) != 0)
        return ota_fail(out, "unsupported schema");
    if(!json_get_string(json, "version", out->version, sizeof out->version))
        return ota_fail(out, "missing version");
    if(!json_get_string(json, "channel", out->channel, sizeof out->channel))
        return ota_fail(out, "missing channel");
    if(!json_get_string(json, "board", out->board, sizeof out->board))
        return ota_fail(out, "missing board");
    if(!json_get_string(json, "firmware_url", out->firmware_url, sizeof out->firmware_url))
        return ota_fail(out, "missing firmware_url");
    if(!json_get_string(json, "sha256", out->sha256, sizeof out->sha256))
        return ota_fail(out, "missing sha256");
    if(!json_get_u32(json, "size", &out->size_bytes))
        return ota_fail(out, "missing size");

    json_get_string(json, "min_version", out->min_version, sizeof out->min_version);
    json_get_string(json, "notes_url", out->notes_url, sizeof out->notes_url);

    if(!safe_ota_token(out->version)) return ota_fail(out, "bad version");
    if(!safe_ota_token(out->channel)) return ota_fail(out, "bad channel");
    if(strcmp(out->board, LZ_OTA_BOARD_TDECK) != 0) return ota_fail(out, "unsupported board");
    if(!safe_http_url(out->firmware_url)) return ota_fail(out, "bad firmware_url");
    if(!hex64(out->sha256)) return ota_fail(out, "bad sha256");
    if(out->size_bytes == 0 || out->size_bytes > LZ_OTA_SLOT_MAX_BYTES)
        return ota_fail(out, "bad size");
    if(out->min_version[0] && !safe_ota_token(out->min_version))
        return ota_fail(out, "bad min_version");
    if(out->notes_url[0] && !safe_http_url(out->notes_url))
        return ota_fail(out, "bad notes_url");

    out->valid = true;
    return true;
}

static bool load_ota_manifest_path(const char *path, lz_ota_manifest_t *out)
{
    if(!path || !out || !path_is_file(path)) return false;
    memset(out, 0, sizeof *out);
    out->found = true;
    snprintf(out->source, sizeof out->source, "%s", path);

    FILE *f = fopen(path, "rb");
    if(!f) return ota_fail(out, "manifest unreadable");
    char json[2049];
    size_t n = fread(json, 1, sizeof json - 1, f);
    fclose(f);
    json[n] = 0;
    if(n == 0) return ota_fail(out, "empty manifest");
    if(n >= sizeof json - 1) return ota_fail(out, "manifest too large");

    parse_ota_manifest_json(json, out);
    return true;
}

bool lz_store_ota_manifest_status(lz_ota_manifest_t *out)
{
    if(!out) return false;
    memset(out, 0, sizeof *out);

    char path[160];
    if(g_persist) {
        path_join(path, sizeof path, g_dir, "ota/manifest.json");
        if(load_ota_manifest_path(path, out)) return out->valid;

        const char *root = lz_store_file_root();
        if(root && strcmp(root, g_dir) != 0) {
            path_join(path, sizeof path, root, "ota/manifest.json");
            if(load_ota_manifest_path(path, out)) return out->valid;
        }
    }

    if(g_appfs_dir[0]) {
        path_join(path, sizeof path, g_appfs_dir, "ota/manifest.json");
        if(load_ota_manifest_path(path, out)) return out->valid;
    }

    snprintf(out->error, sizeof out->error, "no cached manifest");
    return false;
}

int lz_store_ota_manifest_selftest(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    static const char *good =
        "{\"schema\":\"limitlezz.ota_manifest.v1\",\"version\":\"0.97.0\","
        "\"channel\":\"beta\",\"board\":\"tdeck\","
        "\"firmware_url\":\"https://updates.limitlezz.example/tdeck/0.97.0/firmware.bin\","
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":1539920,\"min_version\":\"0.96.0\","
        "\"notes_url\":\"https://updates.limitlezz.example/tdeck/0.97.0/notes\"}";
    static const char *bad_sha =
        "{\"schema\":\"limitlezz.ota_manifest.v1\",\"version\":\"0.97.0\","
        "\"channel\":\"beta\",\"board\":\"tdeck\","
        "\"firmware_url\":\"https://updates.limitlezz.example/tdeck/0.97.0/firmware.bin\","
        "\"sha256\":\"bad\",\"size\":1539920}";
    static const char *bad_size =
        "{\"schema\":\"limitlezz.ota_manifest.v1\",\"version\":\"0.97.0\","
        "\"channel\":\"beta\",\"board\":\"tdeck\","
        "\"firmware_url\":\"https://updates.limitlezz.example/tdeck/0.97.0/firmware.bin\","
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":5242881}";

    lz_ota_manifest_t m;
    memset(&m, 0, sizeof m);
    bool ok_valid = parse_ota_manifest_json(good, &m) && m.valid &&
                    m.size_bytes == 1539920u && strcmp(m.board, LZ_OTA_BOARD_TDECK) == 0;

    memset(&m, 0, sizeof m);
    bool ok_sha = !parse_ota_manifest_json(bad_sha, &m) &&
                  strcmp(m.error, "bad sha256") == 0;
    char sha_error[48];
    snprintf(sha_error, sizeof sha_error, "%s", m.error);

    memset(&m, 0, sizeof m);
    bool ok_size = !parse_ota_manifest_json(bad_size, &m) &&
                   strcmp(m.error, "bad size") == 0;

    bool pass = ok_valid && ok_sha && ok_size;
    return snprintf(buf, (size_t)n, "OTA manifest selftest: %s valid=%d invalid_error=\"%s\"",
                    pass ? "PASS" : "FAIL", ok_valid ? 1 : 0,
                    sha_error[0] ? sha_error : "-");
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
    if(!path_is_dir(data)) {
        char retained[128], retained_data[160];
        char restore_err[32];
        restore_err[0] = 0;
        if(app_retained_paths(app->id, retained, sizeof retained,
                              retained_data, sizeof retained_data,
                              restore_err, sizeof restore_err) &&
           path_is_dir(retained_data)) {
            if(rename(retained_data, data) != 0) {
                if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "data restore failed");
                return false;
            }
            path_rmdir(retained);
        }
    }
    if(!path_mkdir(data) || !path_is_dir(data)) {
        if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "data mkdir failed");
        return false;
    }
    if(path_out && path_cap > 0) snprintf(path_out, (size_t)path_cap, "%s", data);
    return true;
}

bool lz_store_uninstall_local_app(const lz_local_app_t *app, bool keep_data,
                                  char *err, int err_cap)
{
    set_err(err, err_cap, "");
    if(!g_persist) {
        set_err(err, err_cap, "storage unavailable");
        return false;
    }
    if(!app || !app->id[0] || !app->path[0] || !path_is_dir(app->path)) {
        set_err(err, err_cap, "package missing");
        return false;
    }
    if(!safe_id(app->id) || !app_path_in_primary_apps(app->path)) {
        set_err(err, err_cap, "not removable");
        return false;
    }

    char data[128];
    path_join(data, sizeof data, app->path, "data");
    if(keep_data && path_is_dir(data)) {
        char retained[128], retained_data[160];
        if(!app_retained_paths(app->id, retained, sizeof retained,
                               retained_data, sizeof retained_data,
                               err, err_cap))
            return false;
        if(!remove_tree(retained, err, err_cap)) return false;
        if(!path_mkdir(retained) || !path_is_dir(retained)) {
            set_err(err, err_cap, "retain mkdir failed");
            return false;
        }
        if(rename(data, retained_data) != 0) {
            set_err(err, err_cap, "retain failed");
            return false;
        }
    }

    return remove_tree(app->path, err, err_cap);
}

bool lz_store_app_data_usage(const lz_local_app_t *app, uint32_t *used, uint32_t *quota,
                             char *err, int err_cap)
{
    if(used) *used = 0;
    if(quota) *quota = LZ_LOCAL_APP_DATA_QUOTA_BYTES;
    if(err && err_cap > 0) err[0] = 0;
    if(!app || !app->path[0] || !path_is_dir(app->path)) {
        set_err(err, err_cap, "package missing");
        return false;
    }
    char data[128];
    path_join(data, sizeof data, app->path, "data");
    if(!path_is_dir(data)) return true;
    int entries = 0;
    uint32_t total = 0;
    if(!app_data_usage_walk(data, 0, &entries, &total, err, err_cap)) return false;
    if(used) *used = total;
    return true;
}

bool lz_store_clear_app_data(const lz_local_app_t *app, char *err, int err_cap)
{
    if(err && err_cap > 0) err[0] = 0;
    if(!app || !app->path[0] || !path_is_dir(app->path)) {
        set_err(err, err_cap, "package missing");
        return false;
    }
    if((app->permissions & LZ_APP_PERM_STORAGE) == 0) {
        set_err(err, err_cap, "storage not requested");
        return false;
    }

    char data[128];
    if(!lz_store_prepare_app_data(app, data, sizeof data, err, err_cap))
        return false;
    int entries = 0;
    if(!app_data_clear_walk(data, 0, &entries, err, err_cap))
        return false;
    if(!path_mkdir(data) || !path_is_dir(data)) {
        set_err(err, err_cap, "data mkdir failed");
        return false;
    }
    return true;
}

/* SDK 0.1 launch shell: load bounded display metadata from the app entry file
 * without executing script code or granting hardware access. */
bool lz_store_start_local_app(const lz_local_app_t *app, lz_local_app_session_t *out)
{
    if(!out) return false;
    memset(out, 0, sizeof *out);
    out->runtime_budget_bytes = LZ_LOCAL_APP_RUNTIME_BUDGET_BYTES;
    if(!app || !app->id[0]) return app_session_fail(out, app, "missing app");
    out->permissions = app->permissions;
    snprintf(out->title, sizeof out->title, "%s", app->name[0] ? app->name : app->id);
    snprintf(out->status, sizeof out->status, "SDK %s foreground sandbox",
             app->api_version[0] ? app->api_version : "0.1");

    if((app->permissions & LZ_APP_PERM_DISPLAY) == 0)
        return app_session_fail(out, app, "display permission missing");
    if(!app->path[0] || !path_is_dir(app->path))
        return app_session_fail(out, app, "package missing");
    if(!safe_entry(app->entry))
        return app_session_fail(out, app, "unsafe entry");

    if(app->permissions & LZ_APP_PERM_STORAGE) {
        char err[48];
        if(!lz_store_prepare_app_data(app, out->data_path, sizeof out->data_path,
                                      err, sizeof err))
            return app_session_fail(out, app, err[0] ? err : "storage unavailable");
        out->storage_ready = true;
        if(!lz_store_app_data_usage(app, &out->data_used_bytes, &out->data_quota_bytes,
                                    err, sizeof err))
            return app_session_fail(out, app, err[0] ? err : "data scan failed");
        if(out->data_used_bytes > out->data_quota_bytes)
            return app_session_fail(out, app, "data quota exceeded");
    }

    char entry_path[160];
    path_join(entry_path, sizeof entry_path, app->path, app->entry);
    struct stat st;
    if(stat(entry_path, &st) != 0 || S_ISDIR(st.st_mode))
        return app_session_fail(out, app, "missing entry file");
    if(st.st_size < 0 || (unsigned long long)st.st_size > LZ_LOCAL_APP_ENTRY_MAX)
        return app_session_fail(out, app, "entry too large");

    FILE *f = fopen(entry_path, "rb");
    if(!f) return app_session_fail(out, app, "missing entry file");

    char raw[LZ_LOCAL_APP_ENTRY_MAX + 1];
    size_t n = fread(raw, 1, sizeof raw - 1, f);
    fclose(f);
    raw[n] = 0;
    if(n == 0) return app_session_fail(out, app, "empty entry");
    out->entry_source_bytes = (uint32_t)n;
    out->entry_loaded = true;

    bool have_body = false;
    const char *p = raw;
    while(*p) {
        char line[128];
        size_t j = 0;
        while(p[j] && p[j] != '\n' && j + 1 < sizeof line) { line[j] = p[j]; j++; }
        line[j] = 0;

        const char *v;
        if((v = entry_value_for(line, "title")) != NULL) {
            clean_line_copy(out->title, sizeof out->title, v);
        } else if((v = entry_value_for(line, "status")) != NULL) {
            clean_line_copy(out->status, sizeof out->status, v);
        } else if((v = entry_value_for(line, "body")) != NULL ||
                  (v = entry_value_for(line, "text")) != NULL) {
            if(append_body_line(out->body, sizeof out->body, v)) have_body = true;
        } else if((v = entry_value_for(line, "action")) != NULL) {
            (void)add_session_action(out, v);
        } else {
            const char *q = skip_ws(line);
            if(!have_body && q[0] && strncmp(q, "--", 2) != 0 && q[0] != '#' &&
               strncmp(q, "return", 6) != 0) {
                if(append_body_line(out->body, sizeof out->body, q)) have_body = true;
            }
        }

        while(*p && *p != '\n') p++;
        if(*p == '\n') p++;
    }

    if(!out->body[0]) {
        if(app->summary[0]) snprintf(out->body, sizeof out->body, "%s", app->summary);
        else snprintf(out->body, sizeof out->body, "This local app opened in the safe SDK shell.");
    }
    if(!lz_local_app_runtime_within_budget(out)) return false;
    if(out->action_count > 0 && (app->permissions & LZ_APP_PERM_INPUT) == 0)
        return app_session_fail(out, app, "input permission missing");
    char effect_err[48];
    if(action_effect_error(out, effect_err, sizeof effect_err))
        return app_session_fail(out, app, effect_err);
    if(action_needs_storage(out) && !out->storage_ready)
        return app_session_fail(out, app, "storage permission missing");
    if(action_needs_notifications(out) && (out->permissions & LZ_APP_PERM_NOTIFICATIONS) == 0)
        return app_session_fail(out, app, "notifications permission missing");
    return true;
}

bool lz_store_local_app_action(lz_local_app_session_t *session, int idx)
{
    if(!session) return false;
    if(session->error[0]) return false;
    if(idx < 0 || idx >= session->action_count) {
        app_session_fault(session, "action", "invalid action");
        return false;
    }
    session->fault[0] = 0;
    lz_local_app_action_t *a = &session->actions[idx];
    char key[20];
    uint32_t count = 0;
    if(effect_counter_key(a->effect, key, sizeof key)) {
        if(session->data_quota_bytes &&
           session->data_used_bytes + 16u > session->data_quota_bytes) {
            app_session_fault(session, "action", "storage quota exceeded");
            snprintf(session->status, sizeof session->status, "Storage quota exceeded");
            snprintf(session->body, sizeof session->body,
                     "Action could not write scoped app data.");
            session->action_last = (uint8_t)(idx + 1);
            return true;
        }
        if(!session->storage_ready || !counter_read_write(session->data_path, key, &count)) {
            app_session_fault(session, "action", "storage write failed");
            snprintf(session->status, sizeof session->status, "Action failed");
            snprintf(session->body, sizeof session->body,
                     "Action could not write scoped app data.");
            session->action_last = (uint8_t)(idx + 1);
            return true;
        }
        if(a->status[0]) template_count(session->status, sizeof session->status, a->status, count);
        if(a->body[0]) template_count(session->body, sizeof session->body, a->body, count);
        refresh_session_data_usage(session);
    } else {
        if(a->status[0]) snprintf(session->status, sizeof session->status, "%s", a->status);
        if(a->body[0]) snprintf(session->body, sizeof session->body, "%s", a->body);
    }
    session->action_last = (uint8_t)(idx + 1);
    (void)lz_local_app_runtime_within_budget(session);
    return true;
}

void lz_store_stop_local_app(lz_local_app_session_t *session)
{
    if(session) memset(session, 0, sizeof *session);
}

/* addr strings can contain '!' etc; keep alnum only in filenames */
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

extern bool lz_store_secret_save_wifi(const char *ssid, const char *pass, int autoconnect) __attribute__((weak));
extern bool lz_store_secret_load_wifi(char *ssid, int sn, char *pass, int pn, int *autoconnect) __attribute__((weak));

static void remove_legacy_wifi_file(void)
{
    if(!g_persist) return;
    char path[128];
    path_for(path, sizeof path, "wifi.cfg");
    remove(path);
}

/* ---- device PIN verifier ----
 *
 * First Phase 12 security increment. This stores only a salted, iterated
 * verifier for an optional device PIN. It does not encrypt user data yet.
 */

static bool is_hex_n(const char *s, int n)
{
    if(!s) return false;
    for(int i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if(!ok) return false;
    }
    return s[n] == 0;
}

static void hex32(char out[65], const uint8_t in[32])
{
    static const char *h = "0123456789abcdef";
    for(int i = 0; i < 32; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 15];
    }
    out[64] = 0;
}

static void set_security_err(char *err, int cap, const char *msg)
{
    if(err && cap > 0) snprintf(err, (size_t)cap, "%s", msg ? msg : "security error");
}

static bool valid_pin(const char *pin, char *err, int cap)
{
    if(!pin) {
        set_security_err(err, cap, "missing PIN");
        return false;
    }
    int n = (int)strlen(pin);
    if(n < LZ_SECURITY_PIN_MIN || n > LZ_SECURITY_PIN_MAX) {
        set_security_err(err, cap, "PIN length must be 4-12 digits");
        return false;
    }
    for(int i = 0; i < n; i++) {
        if(pin[i] < '0' || pin[i] > '9') {
            set_security_err(err, cap, "PIN must use digits only");
            return false;
        }
    }
    set_security_err(err, cap, "");
    return true;
}

static uint32_t security_random32(void)
{
#ifdef LZ_TARGET_TDECK
    return esp_random();
#else
    static uint32_t ctr = 0x6c7a5049u;
    uintptr_t mix = (uintptr_t)&ctr ^ (uintptr_t)g_dir;
    uint32_t t = (uint32_t)time(NULL);
    ctr = ctr * 1664525u + 1013904223u + t + (uint32_t)mix;
    return ctr ^ (uint32_t)(mix >> 16);
#endif
}

static void security_make_salt(char out[17])
{
    uint8_t raw[8];
    uint32_t a = security_random32();
    uint32_t b = security_random32();
    memcpy(raw, &a, 4);
    memcpy(raw + 4, &b, 4);
    static const char *h = "0123456789abcdef";
    for(int i = 0; i < 8; i++) {
        out[i * 2] = h[raw[i] >> 4];
        out[i * 2 + 1] = h[raw[i] & 15];
    }
    out[16] = 0;
}

static void security_pin_kdf(const char *pin, const char *salt, uint32_t rounds, char out_hex[65])
{
    uint8_t digest[32];
    lz_sha256_ctx c;
    lz_sha256_init(&c);
    lz_sha256_update(&c, (const uint8_t *)"limitlezz.pin.v1", 16);
    lz_sha256_update(&c, (const uint8_t *)salt, strlen(salt));
    lz_sha256_update(&c, (const uint8_t *)pin, strlen(pin));
    lz_sha256_final(&c, digest);

    if(rounds < 1) rounds = 1;
    for(uint32_t i = 1; i < rounds; i++) {
        uint8_t ctr[4];
        ctr[0] = (uint8_t)(i & 0xFF);
        ctr[1] = (uint8_t)((i >> 8) & 0xFF);
        ctr[2] = (uint8_t)((i >> 16) & 0xFF);
        ctr[3] = (uint8_t)((i >> 24) & 0xFF);
        lz_sha256_init(&c);
        lz_sha256_update(&c, digest, sizeof digest);
        lz_sha256_update(&c, (const uint8_t *)salt, strlen(salt));
        lz_sha256_update(&c, ctr, sizeof ctr);
        lz_sha256_final(&c, digest);
    }
    hex32(out_hex, digest);
}

static bool security_load(char salt[17], char verifier[65], uint32_t *rounds,
                          lz_security_status_t *status)
{
    if(status) {
        memset(status, 0, sizeof *status);
        status->valid = true;
        snprintf(status->error, sizeof status->error, "not configured");
    }
    if(!g_persist) {
        if(status) snprintf(status->error, sizeof status->error, "storage unavailable");
        return false;
    }

    char path[128];
    path_for(path, sizeof path, "security.cfg");
    FILE *f = fopen(path, "r");
    if(!f) return false;
    char line[180];
    bool have_line = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if(!have_line) {
        if(status) {
            status->configured = true;
            status->valid = false;
            snprintf(status->error, sizeof status->error, "empty verifier");
        }
        return false;
    }
    line[strcspn(line, "\r\n")] = 0;
    char *cur = line;
    char *ver = field(&cur), *kind = field(&cur), *roundf = field(&cur),
         *saltf = field(&cur), *hashf = cur;
    char *rend = NULL;
    unsigned long r = roundf ? strtoul(roundf, &rend, 10) : 0;
    bool ok = ver && strcmp(ver, "1") == 0 &&
              kind && strcmp(kind, "pin-sha256") == 0 &&
              roundf && saltf && hashf &&
              is_hex_n(saltf, 16) && is_hex_n(hashf, 64) &&
              rend && *rend == 0 &&
              r >= 1 && r <= 65536u;
    if(!ok) {
        if(status) {
            status->configured = true;
            status->valid = false;
            snprintf(status->error, sizeof status->error, "corrupt verifier");
        }
        return false;
    }
    if(salt) snprintf(salt, 17, "%s", saltf);
    if(verifier) snprintf(verifier, 65, "%s", hashf);
    if(rounds) *rounds = (uint32_t)r;
    if(status) {
        status->configured = true;
        status->valid = true;
        status->rounds = (uint32_t)r;
        snprintf(status->salt, sizeof status->salt, "%s", saltf);
        snprintf(status->error, sizeof status->error, "ok");
    }
    return true;
}

bool lz_store_security_status(lz_security_status_t *out)
{
    return security_load(NULL, NULL, NULL, out);
}

bool lz_store_security_set_pin(const char *pin, char *err, int err_cap)
{
    if(!valid_pin(pin, err, err_cap)) return false;
    if(!g_persist) {
        set_security_err(err, err_cap, "storage unavailable");
        return false;
    }
    char salt[17], verifier[65];
    security_make_salt(salt);
    security_pin_kdf(pin, salt, LZ_SECURITY_KDF_ROUNDS, verifier);

    char path[128], tmp[132];
    path_for(path, sizeof path, "security.cfg");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if(!f) {
        set_security_err(err, err_cap, "verifier write failed");
        return false;
    }
    fprintf(f, "1|pin-sha256|%u|%s|%s\n",
            (unsigned)LZ_SECURITY_KDF_ROUNDS, salt, verifier);
    fclose(f);
    remove(path);
    if(rename(tmp, path) != 0) {
        remove(tmp);
        set_security_err(err, err_cap, "verifier save failed");
        return false;
    }
    set_security_err(err, err_cap, "ok");
    return true;
}

bool lz_store_security_check_pin(const char *pin)
{
    if(!valid_pin(pin, NULL, 0)) return false;
    char salt[17], verifier[65], probe[65];
    uint32_t rounds = 0;
    if(!security_load(salt, verifier, &rounds, NULL)) return false;
    security_pin_kdf(pin, salt, rounds, probe);
    unsigned diff = 0;
    for(int i = 0; i < 64; i++) diff |= (unsigned char)(probe[i] ^ verifier[i]);
    return diff == 0;
}

bool lz_store_security_clear_pin(const char *pin, char *err, int err_cap)
{
    lz_security_status_t st;
    bool configured = security_load(NULL, NULL, NULL, &st);
    if(st.configured && !st.valid) {
        set_security_err(err, err_cap, st.error);
        return false;
    }
    if(!configured) {
        set_security_err(err, err_cap, "not configured");
        return true;
    }
    if(!lz_store_security_check_pin(pin)) {
        set_security_err(err, err_cap, "PIN rejected");
        return false;
    }
    char path[128];
    path_for(path, sizeof path, "security.cfg");
    remove(path);
    set_security_err(err, err_cap, "ok");
    return true;
}

int lz_store_security_selftest(char *buf, int n)
{
    if(!buf || n <= 0) return 0;
    char err[64] = {0};
    char a[65], b[65], c[65];
    security_pin_kdf("123456", "0011223344556677", 32, a);
    security_pin_kdf("123456", "0011223344556677", 32, b);
    security_pin_kdf("123457", "0011223344556677", 32, c);
    bool pass = valid_pin("1234", err, sizeof err) &&
                !valid_pin("12ab", err, sizeof err) &&
                strcmp(a, b) == 0 && strcmp(a, c) != 0 && is_hex_n(a, 64);
    return snprintf(buf, (size_t)n, "PIN verifier selftest: %s min=%d max=%d rounds=%u",
                    pass ? "PASS" : "FAIL", LZ_SECURITY_PIN_MIN,
                    LZ_SECURITY_PIN_MAX, (unsigned)LZ_SECURITY_KDF_ROUNDS);
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
    fprintf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
            LZ_SETTINGS_SCHEMA_VERSION,
            s->net_mt ? 1 : 0, s->net_mc ? 1 : 0, lz_airtime_mode_clamp(s->airtime),
            s->tx, s->gps ? 1 : 0,
            s->bright, s->timeout, s->kb_light, s->tz_idx,
            s->clock24 ? 1 : 0, s->save ? 1 : 0, s->developer ? 1 : 0,
            lz_app_source_clamp(s->app_source));
    fclose(f);
    remove(path);
    rename(tmp, path);
}

static bool settings_parse_line(const char *line, lz_user_settings_t *s)
{
    if(!line || !s) return false;
    int ver = 0;
    if(sscanf(line, "%d", &ver) != 1) return false;
    int mt, mc, airtime = LZ_AIRTIME_DEFAULT, tx, gps, bright, timeout, kb, tz, clock24, save;
    int developer = 0, app_source = LZ_APP_SOURCE_OFFICIAL;
    if(ver == LZ_SETTINGS_SCHEMA_VERSION) {
        int got = sscanf(line, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                         &ver, &mt, &mc, &airtime, &tx, &gps, &bright, &timeout, &kb, &tz,
                         &clock24, &save, &developer, &app_source);
        if(got != 14) return false;
    } else if(ver == 3) {
        int got = sscanf(line, "%d %d %d %d %d %d %d %d %d %d %d %d %d",
                         &ver, &mt, &mc, &airtime, &tx, &gps, &bright, &timeout, &kb, &tz,
                         &clock24, &save, &developer);
        if(got != 13) return false;
    } else if(ver == 1 || ver == 2) {
        int got = sscanf(line, "%d %d %d %d %d %d %d %d %d %d %d %d",
                         &ver, &mt, &mc, &tx, &gps, &bright, &timeout, &kb, &tz,
                         &clock24, &save, &developer);
        if(!((ver == 1 && got == 11) || (ver == 2 && got == 12))) return false;
    } else {
        return false;
    }
    s->net_mt = mt != 0;
    s->net_mc = mc != 0;
    s->airtime = lz_airtime_mode_clamp(airtime);
    s->tx = tx;
    s->gps = gps != 0;
    s->bright = bright;
    s->timeout = timeout;
    s->kb_light = kb;
    s->tz_idx = tz;
    s->clock24 = clock24 != 0;
    s->save = save != 0;
    s->developer = ver >= 2 && developer != 0;
    s->app_source = lz_app_source_clamp(app_source);
    return true;
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
    return settings_parse_line(line, s);
}

static bool settings_test_check(bool ok, char *err, int err_cap, const char *msg)
{
    if(ok) return true;
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
    return false;
}

bool lz_store_settings_selftest(char *err, int err_cap)
{
    if(err && err_cap > 0) err[0] = 0;
    lz_user_settings_t s;
    memset(&s, 0, sizeof s);
    if(!settings_test_check(settings_parse_line("1 1 1 2 1 74 3 2 4 1 1", &s) &&
                            s.net_mt && s.net_mc &&
                            s.airtime == LZ_AIRTIME_DEFAULT &&
                            s.tx == 2 && s.gps && s.bright == 74 &&
                            s.timeout == 3 && s.kb_light == 2 &&
                            s.tz_idx == 4 && s.clock24 && s.save &&
                            !s.developer,
                            err, err_cap, "v1 migration failed"))
        return false;

    memset(&s, 0, sizeof s);
    if(!settings_test_check(settings_parse_line("2 1 0 3 0 88 4 1 7 0 1 1", &s) &&
                            s.net_mt && !s.net_mc &&
                            s.airtime == LZ_AIRTIME_DEFAULT &&
                            s.tx == 3 && !s.gps && s.bright == 88 &&
                            s.timeout == 4 && s.kb_light == 1 &&
                            s.tz_idx == 7 && !s.clock24 && s.save &&
                            s.developer,
                            err, err_cap, "v2 migration failed"))
        return false;

    memset(&s, 0, sizeof s);
    if(!settings_test_check(settings_parse_line("3 0 1 2 1 1 42 0 2 9 1 0 1", &s) &&
                            !s.net_mt && s.net_mc &&
                            s.airtime == lz_airtime_mode_clamp(2) &&
                            s.tx == 1 && s.gps && s.bright == 42 &&
                            s.timeout == 0 && s.kb_light == 2 &&
                            s.tz_idx == 9 && s.clock24 && !s.save &&
                            s.developer,
                            err, err_cap, "v3 parse failed"))
        return false;

    memset(&s, 0, sizeof s);
    if(!settings_test_check(settings_parse_line("4 1 0 2 1 1 42 0 2 9 1 0 1 1", &s) &&
                            s.net_mt && !s.net_mc &&
                            s.airtime == lz_airtime_mode_clamp(2) &&
                            s.tz_idx == 9 && s.developer &&
                            s.app_source == LZ_APP_SOURCE_COMMUNITY,
                            err, err_cap, "v4 parse failed"))
        return false;

    if(!settings_test_check(!settings_parse_line("5 1 1 0 3 0 74 1 0 0 0 0 0 0", &s),
                            err, err_cap, "future version accepted"))
        return false;
    if(!settings_test_check(!settings_parse_line("3 1 1", &s),
                            err, err_cap, "truncated settings accepted"))
        return false;
    return true;
}

/* ---- saved Wi-Fi (one network: ssid|password|autoconnect) ---- */

void lz_store_save_wifi(const char *ssid, const char *pass, int autoconnect)
{
    if(lz_store_secret_save_wifi &&
       lz_store_secret_save_wifi(ssid, pass, autoconnect)) {
        remove_legacy_wifi_file();
        return;
    }
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
    if(lz_store_secret_load_wifi &&
       lz_store_secret_load_wifi(ssid, sn, pass, pn, autoconnect))
        return true;
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
    if(ok && lz_store_secret_save_wifi &&
       lz_store_secret_save_wifi(ssid, pass, autoconnect ? *autoconnect : 1))
        remove_legacy_wifi_file();
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

#define LZ_NODE_DB_SCHEMA "v2"

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
        fprintf(f, "%s|%u|%s|%d|%s|%s|%d|%d|%u|%.1f|%s|%s|%s|%s|%u|%ld|%ld|%ld|%u|%u|%u|%u|%d|%u|%u|%u\n",
                LZ_NODE_DB_SCHEMA, (unsigned)nodes[i].num, nodes[i].id, (int)nodes[i].net,
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

static bool node_parse_line(char *line, lz_node_rt *nd)
{
    if(!line || !nd || line[0] == '#') return false;
    char *cur = line;
    char *first = field(&cur);
    if(first && strcmp(first, LZ_NODE_DB_SCHEMA) == 0) first = field(&cur);  /* skip schema tag */
    else if(first && first[0] == 'v') return false;   /* unknown future schema */
    char *num = first, *id = field(&cur), *net = field(&cur),
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
        return false;
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
    return true;
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
        if(node_parse_line(line, &out[n])) n++;
    }
    fclose(f);
    return n;
}

static bool node_test_check(bool ok, char *err, int err_cap, const char *msg)
{
    if(ok) return true;
    if(err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
    return false;
}

bool lz_store_nodes_selftest(char *err, int err_cap)
{
    if(err && err_cap > 0) err[0] = 0;
    lz_node_rt n;
    char legacy[] = "123456|!0001e240|0|Client|TDeck|87|1|1700000000|-8.5|direct|BASE|Base-01";
    if(!node_test_check(node_parse_line(legacy, &n) &&
                        n.num == 123456u && strcmp(n.id, "!0001e240") == 0 &&
                        n.net == LZ_NET_MT && strcmp(n.role, "Client") == 0 &&
                        n.batt == 87 && n.contact && n.last_heard == 1700000000u &&
                        n.snr < -8.4f && n.snr > -8.6f &&
                        strcmp(n.shortcode, "BASE") == 0 &&
                        strcmp(n.name, "Base-01") == 0 &&
                        !n.has_key && n.pos_flags == 0 && n.telem_flags == 0,
                        err, err_cap, "legacy node row failed"))
        return false;

    char v2[] = "v2|305419896|MC-0001|1|Chat|MeshCore|66|0|1700000100|-4.0|2 hops|0001|Limitlezz|000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f|7|451234567|-751234567|123|1700000001|28|31|3940|215|612|10132|4567";
    if(!node_test_check(node_parse_line(v2, &n) &&
                        n.num == 305419896u && n.net == LZ_NET_MC &&
                        strcmp(n.id, "MC-0001") == 0 &&
                        n.has_key && n.pubkey[0] == 0 && n.pubkey[31] == 31 &&
                        n.pos_flags == 7 && n.lat_i == 451234567 &&
                        n.lon_i == -751234567 && n.alt_m == 123 &&
                        n.pos_time == 1700000001u && n.precision_bits == 28 &&
                        n.telem_flags == 31 && n.voltage_mv == 3940 &&
                        n.temp_c10 == 215 && n.humidity10 == 612 &&
                        n.pressure10 == 10132 && n.uptime_s == 4567u,
                        err, err_cap, "v2 node row failed"))
        return false;

    char header[] = "# lz_nodes 2";
    if(!node_test_check(!node_parse_line(header, &n),
                        err, err_cap, "schema header parsed as node"))
        return false;
    return true;
}
