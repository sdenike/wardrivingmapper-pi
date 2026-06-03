#include "recordfmt.h"

#include <stdio.h>
#include <string.h>

/* Append `s` to dst[off..cap) as a JSON string body (no surrounding quotes),
 * escaping the characters JSON requires. Returns the new offset. */
static size_t json_escape_into(char *dst, size_t cap, size_t off, const char *s)
{
    for (; *s && off + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  dst[off++] = '\\'; dst[off++] = '"';  break;
            case '\\': dst[off++] = '\\'; dst[off++] = '\\'; break;
            case '\n': dst[off++] = '\\'; dst[off++] = 'n';  break;
            case '\r': dst[off++] = '\\'; dst[off++] = 'r';  break;
            case '\t': dst[off++] = '\\'; dst[off++] = 't';  break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    off += (size_t)snprintf(dst + off, cap - off, "\\u%04x", c);
                } else {
                    dst[off++] = (char)c;
                }
        }
    }
    return off;
}

int wdm_record_to_json(char *buf, size_t sz, const wdm_ap_t *ap)
{
    if (!buf || sz == 0 || !ap) return -1;

    size_t off = 0;
    int n = snprintf(buf + off, sz - off, "{\"bssid\":\"%s\",\"ssid\":\"", ap->bssid);
    if (n < 0 || (size_t)n >= sz - off) return -1;
    off += (size_t)n;

    off = json_escape_into(buf, sz, off, ap->ssid);

    n = snprintf(buf + off, sz - off,
                 "\",\"rssi\":%d,\"ch\":%d,\"enc\":\"%s\","
                 "\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,\"spd\":%.1f,"
                 "\"hdop\":%.1f,\"sats\":%d,\"ts\":\"%s\"}",
                 ap->rssi, ap->channel, ap->enc,
                 ap->lat, ap->lon, ap->alt_m, ap->speed_kmh,
                 ap->hdop, ap->sats, ap->iso_time);
    if (n < 0 || (size_t)n >= sz - off) return -1;
    off += (size_t)n;
    return (int)off;
}
