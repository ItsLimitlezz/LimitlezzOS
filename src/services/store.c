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

static bool action_needs_storage(const lz_local_app_session_t *s)
{
    if(!s) return false;
    char key[20];
    for(int i = 0; i < s->action_count; i++)
        if(effect_counter_key(s->actions[i].effect, key, sizeof key)) return true;
    return false;
}

static bool action_effect_error(const lz_local_app_session_t *s, char *err, size_t cap)
{
    if(!s) return false;
    char key[20];
    for(int i = 0; i < s->action_count; i++) {
        const char *effect = s->actions[i].effect;
        if(!effect[0]) continue;
        if(strncmp(effect, "counter:", 8) == 0 || strncmp(effect, "count:", 6) == 0) {
            if(effect_counter_key(effect, key, sizeof key)) continue;
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

static bool app_session_fail(lz_local_app_session_t *out, const lz_local_app_t *app,
                             const char *msg)
{
    if(out) {
        if(app && app->name[0]) snprintf(out->title, sizeof out->title, "%s", app->name);
        if(!out->status[0]) snprintf(out->status, sizeof out->status, "Launch blocked");
        snprintf(out->error, sizeof out->error, "%s", msg ? msg : "unknown error");
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
    if(out->action_count > 0 && (app->permissions & LZ_APP_PERM_INPUT) == 0)
        return app_session_fail(out, app, "input permission missing");
    char effect_err[48];
    if(action_effect_error(out, effect_err, sizeof effect_err))
        return app_session_fail(out, app, effect_err);
    if(action_needs_storage(out) && !out->storage_ready)
        return app_session_fail(out, app, "storage permission missing");
    return true;
}

bool lz_store_local_app_action(lz_local_app_session_t *session, int idx)
{
    if(!session || session->error[0] || idx < 0 || idx >= session->action_count)
        return false;
    lz_local_app_action_t *a = &session->actions[idx];
    char key[20];
    uint32_t count = 0;
    if(effect_counter_key(a->effect, key, sizeof key)) {
        if(session->data_quota_bytes &&
           session->data_used_bytes + 16u > session->data_quota_bytes) {
            snprintf(session->status, sizeof session->status, "Storage quota exceeded");
            snprintf(session->body, sizeof session->body,
                     "Action could not write scoped app data.");
            session->action_last = (uint8_t)(idx + 1);
            return true;
        }
        if(!session->storage_ready || !counter_read_write(session->data_path, key, &count))
            return false;
        if(a->status[0]) template_count(session->status, sizeof session->status, a->status, count);
        if(a->body[0]) template_count(session->body, sizeof session->body, a->body, count);
        refresh_session_data_usage(session);
    } else {
        if(a->status[0]) snprintf(session->status, sizeof session->status, "%s", a->status);
        if(a->body[0]) snprintf(session->body, sizeof session->body, "%s", a->body);
    }
    session->action_last = (uint8_t)(idx + 1);
    return true;
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
    fprintf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d\n",
            LZ_SETTINGS_SCHEMA_VERSION,
            s->net_mt ? 1 : 0, s->net_mc ? 1 : 0, lz_airtime_mode_clamp(s->airtime),
            s->tx, s->gps ? 1 : 0,
            s->bright, s->timeout, s->kb_light, s->tz_idx,
            s->clock24 ? 1 : 0, s->save ? 1 : 0, s->developer ? 1 : 0);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

static bool settings_parse_line(const char *line, lz_user_settings_t *s)
{
    if(!line || !s) return false;
    int ver = 0;
    if(sscanf(line, "%d", &ver) != 1) return false;
    int mt, mc, airtime = LZ_AIRTIME_DEFAULT, tx, gps, bright, timeout, kb, tz, clock24, save, developer = 0;
    if(ver == LZ_SETTINGS_SCHEMA_VERSION) {
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

    if(!settings_test_check(!settings_parse_line("4 1 1 0 3 0 74 1 0 0 0 0 0", &s),
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
