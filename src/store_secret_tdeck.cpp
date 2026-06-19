#ifdef LZ_TARGET_TDECK

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

extern "C" bool lz_store_secret_save_wifi(const char *ssid, const char *pass, int autoconnect)
{
    Preferences prefs;
    if(!prefs.begin("lz_wifi", false)) return false;
    bool ok = true;
    if(ssid && ssid[0]) {
        ok = prefs.putString("ssid", ssid) > 0;
        /* Preferences::putString returns the byte count written, which is 0 for an
         * empty (open-network) password even on a fully successful write — so never
         * gate success on it. Store a real password; for an open network drop the
         * key entirely (the loader treats an absent password as empty). */
        if(pass && pass[0]) ok = prefs.putString("pass", pass) > 0 && ok;
        else                prefs.remove("pass");
        ok = prefs.putBool("auto", autoconnect != 0) > 0 && ok;
        /* Don't leave a half-written record: it would "win" over the file fallback
         * on next boot and connect with stale/missing creds. Roll back on failure. */
        if(!ok) prefs.clear();
    } else {
        prefs.remove("ssid");
        prefs.remove("pass");
        ok = prefs.putBool("auto", autoconnect != 0) > 0;
    }
    prefs.end();
    return ok;
}

extern "C" bool lz_store_secret_load_wifi(char *ssid, int sn, char *pass, int pn, int *autoconnect)
{
    if(!ssid || sn <= 0 || !pass || pn <= 0) return false;
    Preferences prefs;
    if(!prefs.begin("lz_wifi", true)) return false;
    char saved[33] = {0};
    size_t n = prefs.getString("ssid", saved, sizeof saved);
    if(autoconnect) *autoconnect = prefs.getBool("auto", true) ? 1 : 0;
    if(n == 0 || saved[0] == 0) {
        prefs.end();
        return false;
    }
    snprintf(ssid, (size_t)sn, "%s", saved);
    pass[0] = 0;
    prefs.getString("pass", pass, (size_t)pn);
    prefs.end();
    return true;
}

#endif /* LZ_TARGET_TDECK */
