#include "upload.h"
#include "pipeline.h"
#include "config.h"
#include "hwid.h"
#include "wdm_platform.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#define TAG          "upload"
#define BATCH        100
#define UPLOADING    WDM_LOG_PATH ".uploading"
#define ARCHIVE      WDM_DATA_DIR "/wardrive.archive.jsonl"

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static upload_status_t s_st;
static bool s_running;

/* ── status plumbing ──────────────────────────────────────────── */
static void st_set(const upload_status_t *v) { pthread_mutex_lock(&s_mu); s_st = *v; pthread_mutex_unlock(&s_mu); }
static void st_progress(uint32_t done, uint32_t total, uint32_t bi, uint32_t bt, const char *host)
{
    pthread_mutex_lock(&s_mu);
    s_st.active = true; s_st.finished = false;
    s_st.done = done; s_st.total = total; s_st.batch_idx = bi; s_st.batch_total = bt;
    if (host) { strncpy(s_st.host, host, sizeof s_st.host - 1); s_st.host[sizeof s_st.host - 1] = '\0'; }
    pthread_mutex_unlock(&s_mu);
}
static void st_finish(bool ok, const char *msg)
{
    pthread_mutex_lock(&s_mu);
    s_st.active = false; s_st.finished = true; s_st.ok = ok;
    if (msg) { strncpy(s_st.msg, msg, sizeof s_st.msg - 1); s_st.msg[sizeof s_st.msg - 1] = '\0'; }
    pthread_mutex_unlock(&s_mu);
    g_system_state.upload_active = false;
}

bool upload_poll(upload_status_t *out)
{
    pthread_mutex_lock(&s_mu);
    *out = s_st;
    bool report = s_st.active || s_st.finished;
    s_st.finished = false;            /* consume the result edge */
    pthread_mutex_unlock(&s_mu);
    return report;
}

/* ── helpers ──────────────────────────────────────────────────── */
static void parse_host(const char *url, char *out, size_t sz)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < sz) { out[i] = p[i]; i++; }
    out[i] = '\0';
}

/* Append device_name as a JSON string value, escaping " and \. */
static size_t append_escaped(char *dst, size_t cap, size_t off, const char *s)
{
    for (; *s && off + 2 < cap; s++) {
        if (*s == '"' || *s == '\\') dst[off++] = '\\';
        dst[off++] = *s;
    }
    return off;
}

struct resp_buf { char data[4096]; size_t len; };
static size_t resp_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
    struct resp_buf *r = ud;
    size_t n = sz * nm;
    size_t room = sizeof r->data - 1 - r->len;
    if (n > room) n = room;
    memcpy(r->data + r->len, ptr, n);
    r->len += n;
    r->data[r->len] = '\0';
    return sz * nm;   /* tell curl we consumed it all (avoid abort) */
}

static int parse_int_field(const char *json, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    return atoi(p + strlen(needle));
}

/* Read all newline-delimited records from `path`. Returns malloc'd array of
 * malloc'd strings (no trailing newline) and sets *count; NULL on empty. */
static char **read_lines(const char *path, size_t *count)
{
    *count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 256, n = 0;
    char **lines = malloc(cap * sizeof *lines);
    if (!lines) { fclose(f); return NULL; }
    char *line = NULL; size_t lcap = 0; ssize_t len;
    while ((len = getline(&line, &lcap, f)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len < 10) continue;                       /* skip blanks/garbage */
        if (n == cap) { cap *= 2; char **nl = realloc(lines, cap * sizeof *lines); if (!nl) break; lines = nl; }
        lines[n] = strdup(line);
        if (!lines[n]) break;
        n++;
    }
    free(line);
    fclose(f);
    *count = n;
    if (n == 0) { free(lines); return NULL; }
    return lines;
}

static void free_lines(char **lines, size_t n)
{
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* Rewrite UPLOADING with lines[from..n) so a partial upload resumes later. */
static void rewrite_remaining(char **lines, size_t from, size_t n)
{
    FILE *f = fopen(UPLOADING, "w");
    if (!f) return;
    for (size_t i = from; i < n; i++) { fputs(lines[i], f); fputc('\n', f); }
    fclose(f);
}

static void archive_lines(char **lines, size_t n)
{
    FILE *f = fopen(ARCHIVE, "a");
    if (!f) return;
    for (size_t i = 0; i < n; i++) { fputs(lines[i], f); fputc('\n', f); }
    fclose(f);
}

/* POST one batch [start,end). Returns HTTP status (200 = ok), 0 on transport error. */
static long post_batch(CURL *c, struct curl_slist *hdrs, const char *device_id,
                       const char *hw_id, char **lines, size_t start, size_t end)
{
    size_t cap = 2048;
    for (size_t i = start; i < end; i++) cap += strlen(lines[i]) + 2;
    char *body = malloc(cap);
    if (!body) return 0;

    size_t off = 0;
    off += (size_t)snprintf(body + off, cap - off, "{\"device_id\":\"");
    off = append_escaped(body, cap, off, device_id);
    off += (size_t)snprintf(body + off, cap - off, "\",\"hw_id\":\"%s\",\"name_ts\":0,\"records\":[", hw_id);
    for (size_t i = start; i < end; i++) {
        if (i > start) body[off++] = ',';
        size_t L = strlen(lines[i]);
        memcpy(body + off, lines[i], L); off += L;
    }
    off += (size_t)snprintf(body + off, cap - off, "]}");

    struct resp_buf rb = {0};
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)off);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, resp_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        int acc = parse_int_field(rb.data, "accepted");
        int dup = parse_int_field(rb.data, "duplicates");
        ESP_LOGI(TAG, "batch %zu..%zu -> %ld accepted=%d dup=%d", start, end, status, acc, dup);
    } else {
        ESP_LOGW(TAG, "curl error: %s", curl_easy_strerror(rc));
    }
    free(body);
    return status;
}

static void *upload_thread(void *arg)
{
    (void)arg;
    const wdm_config_t *cfg = config_get();
    char host[80]; parse_host(cfg->server_url, host, sizeof host);

    if (cfg->api_key[0] == '\0') { st_finish(false, "No API key set"); pthread_mutex_lock(&s_mu); s_running=false; pthread_mutex_unlock(&s_mu); return NULL; }

    /* Rotate the live log aside so scanning can keep appending to a fresh one.
     * A leftover .uploading from a failed run is processed as-is. */
    log_lock();
    if (access(UPLOADING, F_OK) != 0) rename(WDM_LOG_PATH, UPLOADING);
    log_unlock();

    size_t n = 0;
    char **lines = read_lines(UPLOADING, &n);
    if (!lines || n == 0) {
        unlink(UPLOADING);
        st_finish(true, "Nothing to upload");
        pthread_mutex_lock(&s_mu); s_running = false; pthread_mutex_unlock(&s_mu);
        return NULL;
    }

    uint32_t total = (uint32_t)n;
    uint32_t batch_total = (uint32_t)((n + BATCH - 1) / BATCH);
    st_progress(0, total, 0, batch_total, host);

    CURL *c = curl_easy_init();
    struct curl_slist *hdrs = NULL;
    char auth[128];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", cfg->api_key);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, auth);
    curl_easy_setopt(c, CURLOPT_URL, cfg->server_url);

    const char *hw = wdm_hwid();
    size_t sent = 0;
    bool ok = true;
    char msg[80] = "Upload complete";

    for (size_t start = 0; start < n; start += BATCH) {
        size_t end = start + BATCH; if (end > n) end = n;
        long status = post_batch(c, hdrs, cfg->device_name, hw, lines, start, end);
        if (status == 200 || status == 201) {
            sent = end;
            st_progress((uint32_t)sent, total, (uint32_t)(start / BATCH + 1), batch_total, host);
        } else {
            ok = false;
            if (status == 401)      snprintf(msg, sizeof msg, "Invalid API key");
            else if (status == 0)   snprintf(msg, sizeof msg, "Connection error");
            else                    snprintf(msg, sizeof msg, "Server error (%ld)", status);
            break;
        }
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (ok) {
        if (cfg->keep_uploaded) archive_lines(lines, n);
        unlink(UPLOADING);
        snprintf(msg, sizeof msg, "Uploaded %u records", total);
    } else {
        rewrite_remaining(lines, sent, n);            /* keep the unsent tail */
    }
    free_lines(lines, n);

    st_finish(ok, msg);
    pthread_mutex_lock(&s_mu); s_running = false; pthread_mutex_unlock(&s_mu);
    return NULL;
}

void upload_start(void)
{
    pthread_mutex_lock(&s_mu);
    if (s_running) { pthread_mutex_unlock(&s_mu); return; }
    s_running = true;
    memset(&s_st, 0, sizeof s_st);
    s_st.active = true;
    pthread_mutex_unlock(&s_mu);

    g_system_state.current_screen = SCREEN_UPLOAD;
    g_system_state.upload_active = true;

    pthread_t th;
    if (pthread_create(&th, NULL, upload_thread, NULL) != 0) {
        st_finish(false, "thread error");
        pthread_mutex_lock(&s_mu); s_running = false; pthread_mutex_unlock(&s_mu);
        return;
    }
    pthread_detach(th);
}
