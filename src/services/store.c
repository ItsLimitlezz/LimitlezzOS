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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char g_dir[96];
static bool g_persist;

void lz_store_init(const char *datadir)
{
    g_persist = datadir && datadir[0];
    if(g_persist) {
        snprintf(g_dir, sizeof g_dir, "%s", datadir);
    }
}

static void path_for(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", g_dir, name);
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

/* ---- per-thread logs: [u8 self][u32 ts][u16 len][text] ---- */

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
    uint8_t rec[1 + 4 + 2 + LZ_TEXT_MAX];
    size_t n = 0;
    rec[n++] = m->self ? 1 : 0;
    memcpy(rec + n, &m->ts, 4); n += 4;
    memcpy(rec + n, &len, 2);   n += 2;
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
        uint16_t len;
        if(fread(&self, 1, 1, f) != 1) break;
        if(fread(&ts, 4, 1, f) != 1) break;
        if(fread(&len, 2, 1, f) != 1) break;
        lz_msg_rt *slot = &ring[head];
        head = (head + 1) % cap;
        if(count < cap) count++;
        slot->self = self != 0;
        slot->ts = ts;
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
        fprintf(f, "%u|%s|%d|%s|%s|%d|%d|%u|%.1f|%s|%s|%s|%s\n",
                (unsigned)nodes[i].num, nodes[i].id, (int)nodes[i].net,
                nodes[i].role, nodes[i].hw, nodes[i].batt,
                nodes[i].contact ? 1 : 0, (unsigned)nodes[i].last_heard,
                (double)nodes[i].snr, nodes[i].dist,
                nodes[i].shortcode, nodes[i].name, pk);
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
    char line[400];     /* room for the trailing 64-hex public key */
    int n = 0;
    while(n < cap && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *cur = line;
        char *num = field(&cur), *id = field(&cur), *net = field(&cur),
             *role = field(&cur), *hw = field(&cur), *batt = field(&cur),
             *contact = field(&cur), *heard = field(&cur), *snr = field(&cur),
             *dist = field(&cur), *sc = field(&cur), *name = field(&cur),
             *pk = cur;       /* pubkey hex or "-" (NULL on old files = no key) */
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
    }
    fclose(f);
    return n;
}
