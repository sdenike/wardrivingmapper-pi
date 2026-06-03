#include "hwid.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

void wdm_hwid_from_mac(const char *mac, char *out12, size_t outsz)
{
    size_t j = 0;
    for (const char *p = mac; *p && j + 1 < outsz; p++) {
        if (isxdigit((unsigned char)*p)) out12[j++] = (char)toupper((unsigned char)*p);
    }
    out12[j] = '\0';
}

#ifdef __linux__
static int read_mac(const char *iface, char *out, size_t outsz)
{
    char path[64];
    snprintf(path, sizeof path, "/sys/class/net/%s/address", iface);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char raw[32] = {0};
    char *got = fgets(raw, sizeof raw, f);
    fclose(f);
    if (!got) return 0;
    wdm_hwid_from_mac(raw, out, outsz);
    return out[0] != '\0';
}

const char *wdm_hwid(void)
{
    static char id[13] = {0};
    if (id[0] == '\0') {
        /* eth0 first: always present + stable. wlan0 (onboard WiFi) can be
         * absent if its firmware fails to load on boot. */
        if (!read_mac("eth0", id, sizeof id))
            read_mac("wlan0", id, sizeof id);
    }
    return id;
}
#else
const char *wdm_hwid(void) { return ""; }
#endif
