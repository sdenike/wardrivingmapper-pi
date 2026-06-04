#define _GNU_SOURCE
#include "setup_ap.h"
#include "config.h"
#include "scan_iw.h"
#include "scan_parse.h"
#include "pipeline.h"
#include "hwid.h"
#include "wdm_platform.h"

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>

#define TAG     "setup"
/* Hotspot SSID — the password is per-device random (ensure_ap_password), shown
 * on the Setup screen + join QR (nvs_config_get reads the same config value). */
#define AP_SSID "WarDrivingMapper"
/* The AP runs on the USB dongle (RT5370 — a reliable AP radio; the onboard
 * brcmfmac AP fails). The scanner pauses while the dongle is the hotspot.
 * Connectivity stays up on the onboard radio (DeNike/eth0). Joins go to it.
 * Stable names (systemd .link / netplan): dongle=wscan, onboard=wmain — they
 * no longer ride the kernel's wlanN namespace, which swapped across reboots. */
#define AP_IFACE     "wscan"
#define CLIENT_IFACE "wmain"

static volatile bool s_active;
static volatile bool s_should_stop;
static volatile bool s_run;
static int       s_srv_fd = -1;
static pthread_t s_srv_thread;

static const char *cfg_path(void)
{
    const char *p = getenv("WDM_CONFIG");
    return (p && *p) ? p : "config.txt";
}

/* Ensure a per-device random hotspot password exists (not the shipped constant).
 * Generated once from /dev/urandom, persisted to config.txt, shown on-screen. */
static void ensure_ap_password(void)
{
    if (config_get()->ap_pass[0]) return;
    static const char alpha[] = "abcdefghjkmnpqrstuvwxyz23456789";  /* no 0/O/1/l/i */
    char pw[11];
    unsigned char r[10];
    int ok = 0, fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { ok = (read(fd, r, sizeof r) == (ssize_t)sizeof r); close(fd); }
    if (ok) { for (int i = 0; i < 10; i++) pw[i] = alpha[r[i] % (sizeof alpha - 1)]; pw[10] = '\0'; }
    else      snprintf(pw, sizeof pw, "wdmsetup00");
    config_set("ap_pass", pw);
    config_save(cfg_path());
    ESP_LOGI(TAG, "generated per-device setup AP password");
}

/* ── shelling out safely ──────────────────────────────────────────
 * run_argv uses execvp (NO shell) so user-supplied SSID/password can't inject. */
static int run_argv(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ── tiny HTTP helpers ────────────────────────────────────────── */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') { *o++ = ' '; }
        else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else *o++ = *s;
    }
    *o = '\0';
}

/* Extract form field `key` from an application/x-www-form-urlencoded body. */
static int form_get(const char *body, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *eq  = strchr(p, '=');
        if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            const char *vs = eq + 1;
            const char *ve = amp ? amp : vs + strlen(vs);
            size_t vlen = (size_t)(ve - vs);
            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, vs, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (outsz) out[0] = '\0';
    return 0;
}

/* HTML-escape into out (truncates safely). */
static void html_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *in && o + 7 < outsz; in++) {
        switch (*in) {
            case '&': o += (size_t)snprintf(out + o, outsz - o, "&amp;");  break;
            case '<': o += (size_t)snprintf(out + o, outsz - o, "&lt;");   break;
            case '>': o += (size_t)snprintf(out + o, outsz - o, "&gt;");   break;
            case '"': o += (size_t)snprintf(out + o, outsz - o, "&quot;"); break;
            default:  out[o++] = *in;
        }
    }
    out[o] = '\0';
}

static void send_head(int fd, const char *status)
{
    dprintf(fd, "HTTP/1.0 %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Connection: close\r\n\r\n", status);
}

static const char *PAGE_CSS =
    "<style>body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;max-width:560px;"
    "margin:0 auto;padding:16px;background:#f4f4f5;color:#18181b}"
    "h1{font-size:20px}h2{font-size:16px;margin-top:24px}"
    "form{background:#fff;border:1px solid #e4e4e7;border-radius:8px;padding:16px;margin:12px 0}"
    "label{display:block;margin:10px 0 4px;font-size:13px;color:#52525b}"
    "input,select{width:100%;box-sizing:border-box;padding:8px;border:1px solid #d4d4d8;"
    "border-radius:6px;font-size:15px}"
    "button{margin-top:14px;width:100%;padding:11px;background:#059669;color:#fff;border:0;"
    "border-radius:6px;font-size:15px;font-weight:600}"
    ".dot{display:inline-block;width:8px;height:8px;border-radius:2px;background:#059669;"
    "vertical-align:middle;margin-right:6px}</style>";

/* First line of a command's stdout (trimmed), for status readouts. */
static void first_line(const char *cmd, char *out, size_t sz)
{
    out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return;
    if (fgets(out, (int)sz, p)) out[strcspn(out, "\r\n")] = '\0';
    pclose(p);
}

/* Count records currently queued in the log (pending upload). */
static long log_line_count(void)
{
    FILE *f = fopen(WDM_LOG_PATH, "r");
    if (!f) return 0;
    long n = 0; int c, last = '\n';
    while ((c = fgetc(f)) != EOF) { if (c == '\n') n++; last = c; }
    if (last != '\n') n++;
    fclose(f);
    return n;
}

/* Live scan on the CLIENT radio (wlan0) for the network picker. The scanner's
 * dongle is the AP during setup, so its snapshot is stale/empty — and wlan0 is
 * the radio that will actually join, so scanning there gives the right, fresh
 * list. Returns the count of unique named SSIDs (signal-ordered as iw returns). */
static int client_scan_ssids(char out[][33], int max)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "sudo iw dev %s scan 2>/dev/null", CLIENT_IFACE);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    size_t cap = 64 * 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(p); return 0; }
    size_t n; char chunk[8192];
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) {
        if (len + n + 1 > cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) { free(buf); pclose(p); return 0; } buf = nb; }
        memcpy(buf + len, chunk, n); len += n;
    }
    buf[len] = '\0';
    pclose(p);

    wdm_ap_t *aps = malloc(sizeof(wdm_ap_t) * 256);
    if (!aps) { free(buf); return 0; }
    int k = wdm_parse_iw_scan(buf, aps, 256);
    free(buf);

    int outn = 0;
    for (int i = 0; i < k && outn < max; i++) {
        if (aps[i].ssid[0] == '\0') continue;                 /* skip hidden */
        int dup = 0;
        for (int j = 0; j < outn; j++) if (strcmp(out[j], aps[i].ssid) == 0) { dup = 1; break; }
        if (!dup) { strncpy(out[outn], aps[i].ssid, 32); out[outn][32] = '\0'; outn++; }
    }
    free(aps);
    return outn;
}

/* GET / — the full config page. */
static void send_config_page(int fd)
{
    const wdm_config_t *c = config_get();
    char esc[256];

    send_head(fd, "200 OK");
    dprintf(fd, "<!doctype html><html><head><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>WarDrivingMapper Setup</title>%s</head><body>", PAGE_CSS);
    dprintf(fd, "<h1><span class=dot></span>WarDrivingMapper</h1>"
                "<p>You're on the device's setup hotspot. Join a Wi-Fi network and/or "
                "edit settings below.</p>");

    /* ── Status ── */
    {
        char wl[160] = "", ip[64] = "", esc2[200];
        first_line("/usr/sbin/iw dev " CLIENT_IFACE " link 2>/dev/null | sed -n 's/.*SSID: //p'", wl, sizeof wl);
        first_line("ip -4 -o addr show " CLIENT_IFACE " 2>/dev/null | awk '{print $4}'", ip, sizeof ip);
        gps_data_t g = g_gps_data; system_state_t st = g_system_state;
        dprintf(fd, "<form><h2>Status</h2>");
        if (wl[0]) { html_escape(wl, esc2, sizeof esc2); dprintf(fd, "<div>Wi-Fi: <b>%s</b> %s</div>", esc2, ip); }
        else        dprintf(fd, "<div>Wi-Fi: <b>not connected</b></div>");
        dprintf(fd, "<div>GPS: <b>%s</b>, %u sats</div>", g.is_valid ? "fix" : "searching", g.satellites);
        dprintf(fd, "<div>Networks logged this session: <b>%u</b></div>", st.ssids_logged);
        dprintf(fd, "<div>Records pending upload: <b>%ld</b></div>", log_line_count());
        html_escape(config_get()->device_name, esc2, sizeof esc2);
        dprintf(fd, "<div>Device: <b>%s</b> &middot; hw %s</div></form>", esc2, wdm_hwid());
    }

    /* ── Wi-Fi join ── */
    dprintf(fd, "<form method=post action=/wifi><h2>Join Wi-Fi</h2><label>Network</label>"
                "<select name=ssid>");
    char ssids[64][33];
    int k = client_scan_ssids(ssids, 64);
    if (k == 0) dprintf(fd, "<option value=''>(no networks found — Refresh to retry)</option>");
    for (int i = 0; i < k; i++) {
        html_escape(ssids[i], esc, sizeof esc);
        dprintf(fd, "<option value=\"%s\">%s</option>", esc, esc);
    }
    dprintf(fd, "</select>"
                "<label>…or type a hidden/other SSID</label><input name=ssid_manual placeholder='(optional)'>"
                "<label>Password</label><input name=password type=password placeholder='network password'>"
                "<button>Join network</button></form>");

    /* ── Device settings (prefilled) ── */
    dprintf(fd, "<form method=post action=/save><h2>Device settings</h2>");
    html_escape(c->device_name, esc, sizeof esc);
    dprintf(fd, "<label>Device name</label><input name=device_name value=\"%s\">", esc);
    /* Never echo the API key back into the page. Blank submit keeps the current one. */
    dprintf(fd, "<label>API key %s</label><input name=api_key value=\"\" placeholder=\"%s\">",
            c->api_key[0] ? "(set — blank keeps it)" : "(not set)",
            c->api_key[0] ? "leave blank to keep current key" : "wdm_…");
    html_escape(c->server_url, esc, sizeof esc);
    dprintf(fd, "<label>Server URL</label><input name=server_url value=\"%s\">", esc);
    dprintf(fd, "<label>Units</label><select name=units>"
                "<option value=imperial%s>Imperial (mph/ft)</option>"
                "<option value=metric%s>Metric (km/h/m)</option></select>",
            c->use_metric ? "" : " selected", c->use_metric ? " selected" : "");
    dprintf(fd, "<label>Scan radio</label><select name=scan_iface>"
                "<option value=wlan1%s>USB dongle (wlan1)</option>"
                "<option value=wlan0%s>Onboard (wlan0)</option></select>",
            strcmp(c->scan_iface, "wlan1") == 0 ? " selected" : "",
            strcmp(c->scan_iface, "wlan0") == 0 ? " selected" : "");
    dprintf(fd, "<label>Scan interval (seconds)</label><input name=scan_interval_s type=number value=%d>",
            c->scan_interval_s);
    dprintf(fd, "<label>Keep uploaded records</label><select name=keep_uploaded>"
                "<option value=false%s>No (archive + remove)</option>"
                "<option value=true%s>Yes (keep a copy)</option></select>",
            c->keep_uploaded ? "" : " selected", c->keep_uploaded ? " selected" : "");
    dprintf(fd, "<button>Save settings</button></form>");

    dprintf(fd, "</body></html>");
}

static void send_message_page(int fd, const char *title, const char *msg)
{
    send_head(fd, "200 OK");
    dprintf(fd, "<!doctype html><html><head><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>%s</title>%s</head><body><h1><span class=dot></span>%s</h1>"
                "<p>%s</p><p><a href=/>&larr; Back to settings</a></p></body></html>",
            title, PAGE_CSS, title, msg);
}

/* POST /save — persist every setting to config.txt. */
static void handle_save(int fd, const char *body)
{
    char v[256];
    if (form_get(body, "device_name", v, sizeof v))     config_set("device_name", v);
    if (form_get(body, "api_key", v, sizeof v) && v[0]) config_set("api_key", v);  /* blank = keep current */
    if (form_get(body, "server_url", v, sizeof v))      config_set("server_url", v);
    if (form_get(body, "units", v, sizeof v))           config_set("units", v);
    if (form_get(body, "scan_iface", v, sizeof v))      config_set("scan_iface", v);
    if (form_get(body, "scan_interval_s", v, sizeof v)) config_set("scan_interval_s", v);
    if (form_get(body, "keep_uploaded", v, sizeof v))   config_set("keep_uploaded", v);
    bool ok = config_save(cfg_path());
    ESP_LOGI(TAG, "settings saved via web UI (%s)", ok ? "ok" : "FAILED");
    send_message_page(fd, ok ? "Settings saved" : "Save failed",
                      ok ? "Your settings are stored and take effect on the next scan/upload."
                         : "Could not write config.txt.");
}

/* POST /wifi — join the chosen network on the client radio (wlan0). The hotspot
 * is on the dongle (wlan1), so it stays up and this page stays reachable. */
static void handle_wifi(int fd, const char *body)
{
    char ssid[64] = "", manual[64] = "", pass[128] = "";
    form_get(body, "ssid", ssid, sizeof ssid);
    form_get(body, "ssid_manual", manual, sizeof manual);
    form_get(body, "password", pass, sizeof pass);
    char *target = manual[0] ? manual : ssid;

    if (!target[0]) { send_message_page(fd, "No network", "Pick a network or type an SSID."); return; }
    /* A leading '-' could be smuggled as an nmcli option (argv flag injection). */
    if (target[0] == '-') { send_message_page(fd, "Invalid network", "SSID can't start with a dash."); return; }

    char *argv[] = { "sudo", "nmcli", "device", "wifi", "connect", target,
                     "password", pass, "ifname", CLIENT_IFACE, NULL };
    if (!pass[0]) argv[6] = NULL;   /* open network: drop the password args */
    int rc = run_argv(argv);
    ESP_LOGI(TAG, "join '%s' on %s -> rc=%d", target, CLIENT_IFACE, rc);

    char esc[128]; html_escape(target, esc, sizeof esc);
    char msg[256];
    if (rc == 0) {
        /* Pin the saved connection to wlan0 so NM never floats it onto the
         * dongle (which would break scanning — that bug bit us once). */
        char *pin[] = { "sudo", "nmcli", "connection", "modify", target,
                        "connection.interface-name", CLIENT_IFACE, NULL };
        run_argv(pin);
        snprintf(msg, sizeof msg, "Connected to <b>%s</b> — saved and will auto-join in range. "
                 "Edit more settings, or tap the device screen to finish setup.", esc);
        send_message_page(fd, "Connected", msg);
    } else {
        snprintf(msg, sizeof msg, "Couldn't join <b>%s</b> (wrong password or out of range). "
                 "Go back and try again.", esc);
        send_message_page(fd, "Join failed", msg);
    }
}

static void handle_client(int fd)
{
    char req[8192];
    size_t len = 0;
    ssize_t r;
    /* read headers */
    while (len < sizeof(req) - 1) {
        r = read(fd, req + len, sizeof(req) - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
        req[len] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (len == 0) return;

    char method[8] = "", path[256] = "";
    sscanf(req, "%7s %255s", method, path);

    char *body = strstr(req, "\r\n\r\n");
    size_t body_off = body ? (size_t)(body - req) + 4 : len;
    body = req + body_off;

    /* finish reading the POST body per Content-Length */
    if (strcmp(method, "POST") == 0) {
        char *cl = strcasestr(req, "Content-Length:");
        int clen = cl ? atoi(cl + 15) : 0;
        while ((int)(len - body_off) < clen && len < sizeof(req) - 1) {
            r = read(fd, req + len, sizeof(req) - 1 - len);
            if (r <= 0) break;
            len += (size_t)r;
            req[len] = '\0';
        }
    }

    if (strcmp(method, "POST") == 0 && strncmp(path, "/wifi", 5) == 0)      handle_wifi(fd, body);
    else if (strcmp(method, "POST") == 0 && strncmp(path, "/save", 5) == 0) handle_save(fd, body);
    else                                                                     send_config_page(fd);
}

/* Bind the web server. Try port 80 (needs CAP_NET_BIND_SERVICE, granted to the
 * service); fall back to 8080 for unprivileged manual runs. */
static int web_listen(void)
{
    int ports[] = { 80, 8080 };
    for (int i = 0; i < 2; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons(ports[i]);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(fd, 4) == 0) {
            ESP_LOGI(TAG, "web UI on port %d", ports[i]);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* Background worker: brings the hotspot up (the slow part — pausing the scanner,
 * letting the in-flight scan finish, nmcli) THEN serves the web UI. Runs off the
 * render thread so the Setup screen + QR appear instantly. */
static void *setup_worker(void *arg)
{
    (void)arg;
    scan_pause(true);          /* free the dongle from the wardriving scanner */
    sleep(4);                  /* let any in-flight `iw scan` on wlan1 finish */
    if (!s_active) return NULL;   /* stopped during the wait */

    char *manage[] = { "sudo", "nmcli", "device", "set", AP_IFACE, "managed", "yes", NULL };
    run_argv(manage);

    char *appw = (char *)config_get()->ap_pass;
    char *argv[] = { "sudo", "nmcli", "device", "wifi", "hotspot",
                     "ifname", AP_IFACE, "ssid", AP_SSID, "password", appw, NULL };
    if (run_argv(argv) != 0) { ESP_LOGW(TAG, "nmcli hotspot failed"); s_should_stop = true; return NULL; }
    if (!s_active) return NULL;

    s_srv_fd = web_listen();
    if (s_srv_fd < 0) { ESP_LOGW(TAG, "web bind failed"); return NULL; }
    s_run = true;
    ESP_LOGI(TAG, "hotspot %s up on %s; web UI serving", AP_SSID, AP_IFACE);
    while (s_run) {
        int c = accept(s_srv_fd, NULL, NULL);
        if (c < 0) { if (!s_run) break; continue; }
        handle_client(c);
        close(c);
    }
    return NULL;
}

void setup_mode_start(void)
{
    if (s_active) return;
    ensure_ap_password();      /* quick — so the screen's QR + password are ready now */
    ESP_LOGI(TAG, "entering setup mode (hotspot coming up in background)");
    s_active = true;
    s_should_stop = false;
    s_run = false;
    s_srv_fd = -1;
    if (pthread_create(&s_srv_thread, NULL, setup_worker, NULL) == 0)
        pthread_detach(s_srv_thread);
    else { s_active = false; scan_pause(false); }
}

void setup_mode_stop(void)
{
    if (!s_active) return;
    s_active = false;          /* signal the worker to abort an in-progress bring-up */
    s_run = false;
    ESP_LOGI(TAG, "stopping hotspot, resuming scan on %s", AP_IFACE);
    if (s_srv_fd >= 0) { shutdown(s_srv_fd, SHUT_RDWR); close(s_srv_fd); s_srv_fd = -1; }

    /* Drop + remove the hotspot connection; the dongle returns to station mode. */
    char *down[] = { "sudo", "nmcli", "connection", "down", "Hotspot", NULL };
    run_argv(down);
    char *del[]  = { "sudo", "nmcli", "connection", "delete", "Hotspot", NULL };
    run_argv(del);

    scan_pause(false);         /* dongle free again — scanner resumes */
    s_should_stop = false;
}

bool setup_mode_active(void) { return s_active; }
bool setup_should_stop(void) { return s_should_stop; }
