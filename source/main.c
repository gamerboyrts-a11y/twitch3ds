#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include "log.h"
#include <curl/curl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <unistd.h>
#include <sys/stat.h>
#include <citro3d.h>
#include <citro2d.h>
#include "video.h"
#include <tex3ds.h>
#include <malloc.h>
#include <arpa/inet.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

/* ── App defines ─────────────────────────────────────────── */
#define CLIENT_ID       "kimne78kx3ncx6brgo4mv6wki5h1ko"
#define DEFAULT_CHANNEL "xqc"
#define TOKEN_FILE      "/config/twitch_token.txt"
#define SETTINGS_FILE   "/config/twitch_settings.txt"
#define HISTORY_FILE    "/config/twitch_channels.txt"

#define THUMB_REFRESH_S 30
#define THUMB_W         400
#define THUMB_H         225
#define THUMB_TEX_W     512
#define THUMB_TEX_H     256

#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240

#define IRC_HOST "irc.chat.twitch.tv"
#define IRC_PORT 6697
#define IRC_BUF  2048

#define TAB_H          22
#define CHAT_TOP       (TAB_H)
#define INPUT_BAR_H    26
#define INPUT_BAR_Y    (BOT_H - INPUT_BAR_H)
#define CHAT_BOT       (INPUT_BAR_Y - 1)
#define CHAT_LEFT      4
#define LINE_H         20
#define MAX_LINES      64
#define MAX_LINE_LEN   55
#define VISIBLE_LINES  ((CHAT_BOT - CHAT_TOP) / LINE_H)
#define MAX_HISTORY    10

/* ── DCF defines ─────────────────────────────────────────── */
#define DCF_DEVICE_URL      "https://id.twitch.tv/oauth2/device"
#define DCF_TOKEN_URL       "https://id.twitch.tv/oauth2/token"
#define DCF_SCOPES          "chat:read+chat:edit"
#define DCF_MAX_INTERVAL    30
#define DCF_DEVICE_CODE_LEN 64
#define DCF_TOKEN_LEN       128

/* ── Colour palette ──────────────────────────────────────── */
#define COL_BG_TOP      C2D_Color32(10,10,18,255)
#define COL_BG_BOT      C2D_Color32(14,14,22,255)
#define COL_PURPLE      C2D_Color32(100,65,165,255)
#define COL_PURPLE_DK   C2D_Color32(60,35,110,255)
#define COL_PURPLE_LT   C2D_Color32(130,95,200,255)
#define COL_WHITE       C2D_Color32(255,255,255,255)
#define COL_GRAY        C2D_Color32(150,150,150,255)
#define COL_GRAY_DK     C2D_Color32(60,60,80,255)
#define COL_GREEN       C2D_Color32(80,200,120,255)
#define COL_RED         C2D_Color32(220,80,80,255)
#define COL_YELLOW      C2D_Color32(240,200,60,255)
#define COL_CYAN        C2D_Color32(80,200,220,255)
#define COL_DIVIDER     C2D_Color32(50,50,80,255)
#define COL_INPUT_BG    C2D_Color32(24,24,42,255)
#define COL_CHAT_BG     C2D_Color32(16,16,28,255)
#define COL_TAB_ACT     C2D_Color32(100,65,165,255)
#define COL_TAB_INACT   C2D_Color32(35,30,55,255)
#define COL_OVERLAY     C2D_Color32(0,0,0,180)
#define COL_ROW_SEL     C2D_Color32(60,40,100,255)
#define COL_BTN         C2D_Color32(80,50,140,255)
#define COL_BTN_RED     C2D_Color32(140,40,40,255)
#define COL_TOGGLE_ON   C2D_Color32(40,160,80,255)
#define COL_TOGGLE_OFF  C2D_Color32(80,80,80,255)
#define COL_CODE_BG     C2D_Color32(30,20,60,255)
#define COL_BAR_GREEN   C2D_Color32(80,200,120,200)
#define COL_BAR_BG      C2D_Color32(50,50,80,150)

/* ── Types ───────────────────────────────────────────────── */
typedef enum { TAB_CHAT=0, TAB_CHANNELS=1, TAB_SETTINGS=2 } TabID;
typedef enum { QUAL_160P=0, QUAL_360P=1, QUAL_480P=2 } VideoQuality;
static const char *quality_labels[] = {"160p","360p","480p"};
typedef enum { STATE_WATCHING, STATE_ERROR, STATE_LOGIN_DEVICE } AppState;
typedef struct { char nick[32]; char text[MAX_LINE_LEN]; } ChatLine;

/* ── App struct ──────────────────────────────────────────── */
typedef struct {
    char oauth[128];
    char nick[32];
    bool logged_in;
    char channel[48];
    char history[MAX_HISTORY][48];
    int  history_count;
    int  history_sel;
    char stream_title[128];
    char stream_game[64];
    int  viewer_count;
    int  sock;
    bool irc_connected;
    char irc_buf[IRC_BUF];
    int  irc_buf_len;
    ChatLine lines[MAX_LINES];
    int  line_head;
    int  line_count;
    int  scroll_offset;
    bool scroll_locked;
    char input[128];
    AppState state;
    TabID    tab;
    char     status_msg[96];
    bool     show_overlay;
    VideoQuality quality;
    char user_code[32];
    char verify_url[256];
    C3D_RenderTarget *top;
    C3D_RenderTarget *bot;
    C2D_Font  font;
    C2D_TextBuf tbuf;

    /* Device Code Flow state */
    struct {
        bool polling;
        char device_code[DCF_DEVICE_CODE_LEN];
        int  interval;
        int  expires_in;
        u64  last_poll_tick;
        u64  start_tick;
    } dcf;

    /* Live thumbnail */
    C3D_Tex          thumb_tex;
    Tex3DS_SubTexture thumb_st;
    C2D_Image        thumb_img;
    bool             thumb_loaded;
    u64              thumb_last_tick;

    /* Video state tracking — separate from IRC */
    bool vid_ever_started;
} App;

static App app;

/* ═══════════════════════════════════════════════════════════
 * UTILITY / DRAW HELPERS
 * ═══════════════════════════════════════════════════════════ */

static void begin_text_frame(void) { C2D_TextBufClear(app.tbuf); }

static void set_status(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(app.status_msg, sizeof(app.status_msg), fmt, va);
    va_end(va);
}

static void draw_text(float x, float y, float sz, u32 col, const char *s) {
    C2D_Text t;
    C2D_TextFontParse(&t, app.font, app.tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, sz, sz, col);
}

static void draw_rect(float x, float y, float w, float h, u32 col) {
    C2D_DrawRectSolid(x, y, 0, w, h, col);
}

static bool touch_in(touchPosition *t, int x, int y, int w, int h) {
    return t->px >= x && t->px < x+w && t->py >= y && t->py < y+h;
}

/* ═══════════════════════════════════════════════════════════
 * FILESYSTEM HELPERS
 * ═══════════════════════════════════════════════════════════ */

static void mkdir_config(void) { mkdir("/config", 0777); }

static void save_history(void) {
    mkdir_config();
    FILE *f = fopen(HISTORY_FILE, "w");
    if (!f) return;
    for (int i = 0; i < app.history_count; i++)
        fprintf(f, "%s\n", app.history[i]);
    fclose(f);
}

static void load_history(void) {
    FILE *f = fopen(HISTORY_FILE, "r");
    app.history_count = 0;
    if (!f) { strcpy(app.history[0], "#xqc"); app.history_count = 1; return; }
    char line[48];
    while (fgets(line, sizeof(line), f) && app.history_count < MAX_HISTORY) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) { strncpy(app.history[app.history_count], line, 47); app.history_count++; }
    }
    fclose(f);
    if (app.history_count == 0) { strcpy(app.history[0], "#xqc"); app.history_count = 1; }
}

static void history_push(const char *chan) {
    for (int i = 0; i < app.history_count; i++) {
        if (strcmp(app.history[i], chan) == 0) {
            memmove(&app.history[i], &app.history[i+1],
                    (app.history_count-i-1)*sizeof(app.history[0]));
            app.history_count--;
            break;
        }
    }
    if (app.history_count >= MAX_HISTORY) app.history_count = MAX_HISTORY-1;
    memmove(&app.history[1], &app.history[0],
            app.history_count*sizeof(app.history[0]));
    strncpy(app.history[0], chan, 47); app.history[0][47] = 0;
    app.history_count++;
    save_history();
}

static void save_settings(void) {
    mkdir_config();
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "channel:%s\nquality:%d\noverlay:%d\n",
            app.channel, (int)app.quality, app.show_overlay ? 1 : 0);
    fclose(f);
}

static void load_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    snprintf(app.channel, sizeof(app.channel), "#%s", DEFAULT_CHANNEL);
    app.quality     = QUAL_360P;
    app.show_overlay = false;
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if      (strncmp(line,"channel:", 8) == 0) strncpy(app.channel, line+8, 47);
        else if (strncmp(line,"quality:", 8) == 0) app.quality = (VideoQuality)atoi(line+8);
        else if (strncmp(line,"overlay:", 8) == 0) app.show_overlay = atoi(line+8) != 0;
    }
    fclose(f);
}

static void load_token(void) {
    FILE *f = fopen(TOKEN_FILE, "r");
    if (!f) {
        snprintf(app.oauth, sizeof(app.oauth), "PASS schmoopiie");
        snprintf(app.nick,  sizeof(app.nick),  "justinfan%d",
                 (int)(osGetTime()%90000+10000));
        app.logged_in = false;
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if      (strncmp(line,"oauth:",6) == 0)
            snprintf(app.oauth, sizeof(app.oauth), "PASS oauth:%.*s",
                     (int)(sizeof(app.oauth)-12), line+6);
        else if (strncmp(line,"nick:", 5) == 0) {
            strncpy(app.nick, line+5, sizeof(app.nick)-1);
            app.nick[sizeof(app.nick)-1] = 0;
        }
    }
    fclose(f);
    app.logged_in = (strncmp(app.nick,"justinfan",9) != 0);
}

static void save_token(const char *oauth_token, const char *nick) {
    mkdir_config();
    FILE *f = fopen(TOKEN_FILE, "w");
    if (!f) return;
    fprintf(f, "oauth:%s\n", oauth_token);
    fprintf(f, "nick:%s\n",  nick);
    fclose(f);
}

static void do_logout(void) {
    remove(TOKEN_FILE);
    snprintf(app.oauth, sizeof(app.oauth), "PASS schmoopiie");
    snprintf(app.nick,  sizeof(app.nick),  "justinfan%d",
             (int)(osGetTime()%90000+10000));
    app.logged_in = false;
    set_status("Logged out");
}

/* ═══════════════════════════════════════════════════════════
 * CHAT / IRC
 * ═══════════════════════════════════════════════════════════ */

static void chat_push(const char *nick, const char *text) {
    ChatLine *cl = &app.lines[app.line_head % MAX_LINES];
    strncpy(cl->nick, nick, 31); cl->nick[31] = 0;
    strncpy(cl->text, text, MAX_LINE_LEN-1); cl->text[MAX_LINE_LEN-1] = 0;
    app.line_head++;
    if (app.line_count < MAX_LINES) app.line_count++;
    if (!app.scroll_locked) app.scroll_offset = 0;
}

/* ── TLS globals for IRC ─────────────────────────────────── */
static mbedtls_ssl_context     g_ssl;
static mbedtls_ssl_config      g_conf;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static bool g_tls_ok = false;

static int tls_send(void *ctx, const unsigned char *buf, size_t len) {
    int r = send(*(int*)ctx, (const char*)buf, len, 0);
    return r < 0 ? -1 : r;
}
static int tls_recv(void *ctx, unsigned char *buf, size_t len) {
    int r = recv(*(int*)ctx, (char*)buf, len, 0);
    if (r < 0) return (errno==EAGAIN||errno==EWOULDBLOCK)
               ? MBEDTLS_ERR_SSL_WANT_READ : -1;
    return r;
}

static bool irc_connect(void) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    snprintf(port, sizeof(port), "%d", IRC_PORT);
    if (getaddrinfo(IRC_HOST, port, &hints, &res) != 0) {
        LOG("IRC DNS failed"); set_status("DNS failed"); return false;
    }
    app.sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (app.sock < 0) {
        freeaddrinfo(res); LOG("IRC socket error"); set_status("Socket error"); return false;
    }
    if (connect(app.sock, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res); close(app.sock); app.sock = -1;
        LOG("IRC connect failed errno=%d", errno); set_status("Connect failed"); return false;
    }
    freeaddrinfo(res);

    if (g_tls_ok) {
        mbedtls_ssl_free(&g_ssl); mbedtls_ssl_config_free(&g_conf);
        mbedtls_entropy_free(&g_entropy); mbedtls_ctr_drbg_free(&g_ctr_drbg);
        g_tls_ok = false;
    }
    mbedtls_ssl_init(&g_ssl);        mbedtls_ssl_config_init(&g_conf);
    mbedtls_entropy_init(&g_entropy); mbedtls_ctr_drbg_init(&g_ctr_drbg);
    mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy, NULL, 0);
    mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);
    mbedtls_ssl_setup(&g_ssl, &g_conf);
    mbedtls_ssl_set_hostname(&g_ssl, IRC_HOST);
    mbedtls_ssl_set_bio(&g_ssl, &app.sock, tls_send, tls_recv, NULL);
    LOG("IRC TCP connected, starting TLS handshake");
    int tls_ret = mbedtls_ssl_handshake(&g_ssl);
    if (tls_ret != 0) {
        close(app.sock); app.sock = -1;
        mbedtls_ssl_free(&g_ssl); mbedtls_ssl_config_free(&g_conf);
        mbedtls_entropy_free(&g_entropy); mbedtls_ctr_drbg_free(&g_ctr_drbg);
        LOG("IRC TLS handshake failed: -0x%04X", (unsigned)(-tls_ret));
        set_status("TLS err -0x%04X", (unsigned)(-tls_ret)); return false;
    }
    LOG("IRC TLS ok, sending auth");
    g_tls_ok = true;
    fcntl(app.sock, F_SETFL, O_NONBLOCK);

    char msg[256];
    if (app.logged_in) {
        snprintf(msg, sizeof(msg), "%s\r\n", app.oauth);
        mbedtls_ssl_write(&g_ssl, (const unsigned char*)msg, strlen(msg));
    }
    snprintf(msg, sizeof(msg), "NICK %s\r\n", app.nick);
    mbedtls_ssl_write(&g_ssl, (const unsigned char*)msg, strlen(msg));
    snprintf(msg, sizeof(msg), "JOIN %s\r\n", app.channel);
    mbedtls_ssl_write(&g_ssl, (const unsigned char*)msg, strlen(msg));
    app.irc_connected = true;
    app.irc_buf_len   = 0;
    set_status("Connected to %s", app.channel);
    chat_push("System", "Joined channel!");
    return true;
}

/*
 * irc_disconnect: tears down IRC only.
 * Does NOT touch video — video runs independently.
 */
static void irc_disconnect(void) {
    if (g_tls_ok) {
        mbedtls_ssl_close_notify(&g_ssl);
        mbedtls_ssl_free(&g_ssl); mbedtls_ssl_config_free(&g_conf);
        mbedtls_entropy_free(&g_entropy); mbedtls_ctr_drbg_free(&g_ctr_drbg);
        g_tls_ok = false;
    }
    if (app.sock >= 0) { close(app.sock); app.sock = -1; }
    app.irc_connected = false;
    app.irc_buf_len   = 0;
}

static void irc_send_msg(const char *text) {
    if (!app.irc_connected || !g_tls_ok) return;
    if (!app.logged_in) { chat_push("System","Login to send messages"); return; }
    char msg[256];
    snprintf(msg, sizeof(msg), "PRIVMSG %s :%s\r\n", app.channel, text);
    mbedtls_ssl_write(&g_ssl, (const unsigned char*)msg, strlen(msg));
    chat_push(app.nick, text);
}

static void irc_poll(void) {
    if (!app.irc_connected || !g_tls_ok) return;
    char tmp[512];
    int n = mbedtls_ssl_read(&g_ssl, (unsigned char*)tmp, sizeof(tmp)-1);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == 0) return;
    if (n < 0) { LOG("IRC poll read err %d, reconnecting", n); irc_disconnect(); return; }
    tmp[n] = 0;
    LOG("IRC rx: %.120s", tmp);
    int copy = n;
    if (app.irc_buf_len + copy >= IRC_BUF-1) copy = IRC_BUF-1-app.irc_buf_len;
    memcpy(app.irc_buf + app.irc_buf_len, tmp, copy);
    app.irc_buf_len += copy;
    app.irc_buf[app.irc_buf_len] = 0;
    char *start = app.irc_buf, *end;
    while ((end = strstr(start, "\r\n"))) {
        *end = 0;
        if (strncmp(start, "PING", 4) == 0) {
            char pong[64];
            snprintf(pong, sizeof(pong), "PONG %s\r\n", start+5);
            mbedtls_ssl_write(&g_ssl, (const unsigned char*)pong, strlen(pong));
        } else if (strstr(start, "PRIVMSG")) {
            char *ne = strchr(start+1,'!'), *ms = strstr(start," :");
            if (ne && ms) {
                char nn[32] = {0};
                int nl = (int)(ne-(start+1)); if (nl > 31) nl = 31;
                strncpy(nn, start+1, nl);
                chat_push(nn, ms+2);
            }
        }
        start = end+2;
    }
    int rem = app.irc_buf_len - (int)(start - app.irc_buf);
    if (rem > 0) memmove(app.irc_buf, start, rem);
    app.irc_buf_len = rem;
    app.irc_buf[rem] = 0;
}

/* ═══════════════════════════════════════════════════════════
 * SOFTWARE KEYBOARD HELPER
 * ═══════════════════════════════════════════════════════════ */

static bool swkbd_get(char *out, size_t len, const char *hint, bool password) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, len-1);
    swkbdSetHintText(&kb, hint);
    if (password) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT,  "Cancel", false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK",     true);
    char tmp[len];
    SwkbdButton btn = swkbdInputText(&kb, tmp, len);
    if (btn == SWKBD_BUTTON_CONFIRM && tmp[0]) {
        strncpy(out, tmp, len-1); out[len-1] = 0; return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════
 * CHANNEL / SESSION HELPERS
 * ═══════════════════════════════════════════════════════════ */

static void join_channel(const char *chan) {
    /* Stop old video before starting a new channel */
    video_stop();
    app.vid_ever_started = false;

    irc_disconnect();
    app.line_head = 0; app.line_count = 0;
    app.scroll_offset = 0; app.scroll_locked = false;
    memset(app.lines, 0, sizeof(app.lines));

    strncpy(app.channel, chan, 47); app.channel[47] = 0;
    if (app.channel[0] != '#') {
        char tmp[48]; snprintf(tmp, sizeof(tmp), "#%s", app.channel);
        strncpy(app.channel, tmp, 47);
    }

    history_push(app.channel);
    save_settings();
    chat_push("System", "Welcome to Twitch3DS!");
    app.thumb_loaded = false;
    strcpy(app.stream_game, "Twitch");
    app.viewer_count = 0;

    if (irc_connect()) {
        app.state = STATE_WATCHING;
        const char *ch = app.channel[0]=='#' ? app.channel+1 : app.channel;
        video_start(ch, app.oauth, CLIENT_ID);
        app.vid_ever_started = true;
    } else {
        app.state = STATE_ERROR;
    }
    app.tab = TAB_CHAT;
}

/* ═══════════════════════════════════════════════════════════
 * DEVICE CODE FLOW (DCF)
 * ═══════════════════════════════════════════════════════════ */

typedef struct { char *data; size_t len; size_t cap; } CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuf *buf = (CurlBuf *)userdata;
    size_t incoming = size * nmemb;
    if (buf->len + incoming + 1 > buf->cap) {
        size_t new_cap = buf->cap + incoming + 512;
        char *tmp = realloc(buf->data, new_cap);
        if (!tmp) return 0;
        buf->data = tmp; buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, incoming);
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return incoming;
}

static bool json_get_string(const char *json, const char *key,
                             char *out, size_t out_len) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (!end) return false;
        size_t len = (size_t)(end-p);
        if (len >= out_len) len = out_len-1;
        strncpy(out, p, len); out[len] = '\0';
    } else {
        const char *end = p;
        while (*end && *end != ',' && *end != '}' && *end != '\n') end++;
        size_t len = (size_t)(end-p);
        if (len >= out_len) len = out_len-1;
        strncpy(out, p, len); out[len] = '\0';
    }
    return true;
}

static char *http_post(const char *url, const char *post_body) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    CurlBuf buf = { NULL, 0, 0 };
    buf.data = malloc(512); if (!buf.data) { curl_easy_cleanup(curl); return NULL; }
    buf.cap = 512; buf.data[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    post_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data;
}

static void do_device_login_start(void) {
    set_status("Contacting Twitch...");
    app.state = STATE_LOGIN_DEVICE;

    char post[256];
    snprintf(post, sizeof(post), "client_id=%s&scopes=%s", CLIENT_ID, DCF_SCOPES);

    char *resp = http_post(DCF_DEVICE_URL, post);
    if (!resp) {
        set_status("DCF: network error");
        chat_push("System","Device login failed - check WiFi.");
        app.state = STATE_ERROR; return;
    }

    char device_code[DCF_DEVICE_CODE_LEN] = {0};
    char user_code[32]   = {0};
    char verify_uri[256] = {0};
    char expires_str[16] = {0};
    char interval_str[16]= {0};

    bool ok =
        json_get_string(resp,"device_code",    device_code, sizeof(device_code)) &&
        json_get_string(resp,"user_code",      user_code,   sizeof(user_code))   &&
        json_get_string(resp,"verification_uri",verify_uri, sizeof(verify_uri));
    json_get_string(resp,"expires_in",  expires_str,  sizeof(expires_str));
    json_get_string(resp,"interval",    interval_str, sizeof(interval_str));
    free(resp);

    if (!ok) {
        set_status("DCF: bad response from Twitch");
        chat_push("System","Device login failed - unexpected response.");
        app.state = STATE_ERROR; return;
    }

    strncpy(app.dcf.device_code, device_code, DCF_DEVICE_CODE_LEN-1);
    strncpy(app.user_code, user_code,  sizeof(app.user_code)-1);
    strncpy(app.verify_url, verify_uri, sizeof(app.verify_url)-1);

    int interval = interval_str[0] ? atoi(interval_str) : 5;
    if (interval < 1 || interval > DCF_MAX_INTERVAL) interval = 5;
    app.dcf.interval   = interval;
    app.dcf.expires_in = expires_str[0] ? atoi(expires_str) : 1800;
    app.dcf.polling    = true;
    app.dcf.last_poll_tick = osGetTime();
    app.dcf.start_tick     = osGetTime();

    set_status("Waiting for authorization...");
    chat_push("System","Open the URL on your phone/PC and enter:");
    chat_push("System", app.user_code);
}

static void dcf_poll_tick(void) {
    if (!app.dcf.polling) return;
    u64 now = osGetTime();

    if ((int)((now - app.dcf.start_tick) / 1000) >= app.dcf.expires_in) {
        app.dcf.polling = false;
        set_status("DCF: code expired - try again");
        chat_push("System","Login code expired. Tap Device Code to retry.");
        app.state = STATE_ERROR; return;
    }
    if ((int)((now - app.dcf.last_poll_tick) / 1000) < app.dcf.interval) return;
    app.dcf.last_poll_tick = now;

    char post[512];
    snprintf(post, sizeof(post),
        "client_id=%s"
        "&scopes=%s"
        "&device_code=%s"
        "&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
        CLIENT_ID, DCF_SCOPES, app.dcf.device_code);

    char *resp = http_post(DCF_TOKEN_URL, post);
    if (!resp) { set_status("DCF: poll failed, retrying..."); return; }

    if (strstr(resp,"authorization_pending")) {
        free(resp); set_status("Waiting... open URL on your phone."); return;
    }
    if (strstr(resp,"slow_down")) {
        free(resp); app.dcf.interval += 5; return;
    }
    if (strstr(resp,"\"message\"") || strstr(resp,"\"error\"")) {
        char msg[128] = {0};
        if (!json_get_string(resp,"message",msg,sizeof(msg)))
            json_get_string(resp,"error",msg,sizeof(msg));
        free(resp);
        app.dcf.polling = false;
        set_status("DCF error: %.60s", msg);
        chat_push("System","Login failed - try again.");
        app.state = STATE_ERROR; return;
    }

    char access_token[DCF_TOKEN_LEN] = {0};
    bool got = json_get_string(resp,"access_token",access_token,sizeof(access_token));
    free(resp);

    if (!got || access_token[0] == '\0') {
        app.dcf.polling = false;
        set_status("DCF: no token in response");
        chat_push("System","Login failed - unexpected response.");
        app.state = STATE_ERROR; return;
    }

    char nick[32] = {0};
    CURL *curl = curl_easy_init();
    if (curl) {
        CurlBuf ubuf = { NULL, 0, 0 };
        ubuf.data = malloc(512); ubuf.cap = 512; ubuf.data[0] = '\0';
        char auth_hdr[DCF_TOKEN_LEN+32], cid_hdr[64];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", access_token);
        snprintf(cid_hdr,  sizeof(cid_hdr),  "Client-Id: %s", CLIENT_ID);
        struct curl_slist *hdrs = NULL;
        hdrs = curl_slist_append(hdrs, auth_hdr);
        hdrs = curl_slist_append(hdrs, cid_hdr);
        curl_easy_setopt(curl, CURLOPT_URL,           "https://api.twitch.tv/helix/users");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &ubuf);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
        if (curl_easy_perform(curl) == CURLE_OK)
            json_get_string(ubuf.data,"login",nick,sizeof(nick));
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        free(ubuf.data);
    }
    if (nick[0] == '\0')
        snprintf(nick, sizeof(nick), "twitchuser%u", (unsigned)(osGetTime()%9999));

    app.dcf.polling = false;
    save_token(access_token, nick);
    load_token();
    set_status("Logged in as %s!", app.nick);
    chat_push("System","Device login successful!");
    chat_push("System", app.nick);
    irc_disconnect();
    if (irc_connect()) app.state = STATE_WATCHING;
    else app.state = STATE_ERROR;
}

/* ═══════════════════════════════════════════════════════════
 * LIVE THUMBNAIL
 * ═══════════════════════════════════════════════════════════ */

static void fetch_thumbnail(void) {
    const char *ch = (app.channel[0] == '#') ? app.channel+1 : app.channel;
    char url[256];
    snprintf(url, sizeof(url),
        "https://static-cdn.jtvnw.net/previews-ttv/live_user_%s-%dx%d.jpg",
        ch, THUMB_W, THUMB_H);

    CURL *curl = curl_easy_init();
    if (!curl) return;
    CurlBuf buf = { NULL, 0, 0 };
    buf.data = malloc(128*1024); buf.cap = 128*1024;
    if (!buf.data) { curl_easy_cleanup(curl); return; }
    buf.data[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || buf.len == 0) { free(buf.data); return; }

    int w, h, comp;
    u8 *pixels = stbi_load_from_memory((const stbi_uc*)buf.data, (int)buf.len,
                                        &w, &h, &comp, 4);
    free(buf.data);
    if (!pixels) return;

    u8 *linear = linearAlloc(THUMB_TEX_W * THUMB_TEX_H * 4);
    if (!linear) { stbi_image_free(pixels); return; }
    memset(linear, 0, THUMB_TEX_W * THUMB_TEX_H * 4);
    int cw  = w < THUMB_TEX_W ? w : THUMB_TEX_W;
    int ch2 = h < THUMB_TEX_H ? h : THUMB_TEX_H;
    for (int y = 0; y < ch2; y++)
        memcpy(linear + y*THUMB_TEX_W*4, pixels + y*w*4, cw*4);
    stbi_image_free(pixels);

    GSPGPU_FlushDataCache(linear, THUMB_TEX_W * THUMB_TEX_H * 4);
    C3D_SyncDisplayTransfer(
        (u32*)linear,            GX_BUFFER_DIM(THUMB_TEX_W, THUMB_TEX_H),
        (u32*)app.thumb_tex.data,GX_BUFFER_DIM(THUMB_TEX_W, THUMB_TEX_H),
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)  |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_FLIP_VERT(1) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    linearFree(linear);
    app.thumb_loaded     = true;
    app.thumb_last_tick  = osGetTime();
}

/* ═══════════════════════════════════════════════════════════
 * DRAW — TOP SCREEN
 * ═══════════════════════════════════════════════════════════ */

static void draw_top(void) {
    C2D_TargetClear(app.top, COL_BG_TOP);
    C2D_SceneBegin(app.top);
    begin_text_frame();
    draw_rect(0, 0, TOP_W, TOP_H, C2D_Color32(8,8,16,255));

    if (app.state == STATE_LOGIN_DEVICE) {
        draw_text(10, 14, 0.52f, COL_PURPLE_LT, "Device Login");

        draw_rect(10, 38, TOP_W-20, 44, COL_CODE_BG);
        draw_text(TOP_W/2-60, 50, 0.72f, COL_WHITE, app.user_code);

        draw_text(10,  96, 0.38f, COL_WHITE,  "1. Open on your phone or PC:");
        draw_text(10, 114, 0.40f, COL_CYAN,   app.verify_url);
        draw_text(10, 142, 0.36f, COL_YELLOW, "2. Enter the code shown above.");
        draw_text(10, 158, 0.36f, COL_GRAY,   "3. This screen updates automatically.");

        if (app.dcf.polling) {
            int elapsed = (int)((osGetTime() - app.dcf.start_tick) / 1000);
            int total   = app.dcf.expires_in > 0 ? app.dcf.expires_in : 1800;
            float pct   = 1.0f - (float)elapsed / (float)total;
            if (pct < 0.0f) pct = 0.0f;
            draw_rect(10, 196, TOP_W-20, 8, COL_BAR_BG);
            draw_rect(10, 196, (int)((TOP_W-20)*pct), 8, COL_BAR_GREEN);
            char timer[32];
            int remaining = total - elapsed;
            if (remaining < 0) remaining = 0;
            snprintf(timer, sizeof(timer), "Expires in %ds", remaining);
            draw_text(10, 208, 0.32f, COL_GRAY, timer);
        }

    } else if (app.state == STATE_ERROR) {
        draw_text( 80, 100, 0.50f, COL_RED,    "Connection Error");
        draw_text(  4, 122, 0.36f, COL_YELLOW,
                  app.status_msg[0] ? app.status_msg : "Open Channels tab to retry");
        draw_text(  4, 142, 0.34f, COL_GRAY,   "Open Channels tab and try again");

    } else {
        if (video_is_offline()) {
            draw_text(60,  98, 0.55f, COL_RED,  "Channel is Offline");
            draw_text(60, 122, 0.40f, COL_GRAY, "No live stream right now");
        } else if (app.vid_ever_started) {
            /*
             * Draw video whenever we've ever started it for this channel.
             * video_draw_top keeps displaying the last decoded frame
             * even after the polling thread sleeps between segments.
             */
            video_draw_top(0, 0);
        } else if (app.thumb_loaded) {
            C2D_DrawImageAt(app.thumb_img, 0, (TOP_H - THUMB_H) / 2.0f,
                            0.5f, NULL, 1.0f, 1.0f);
        } else {
            draw_text(120, 108, 0.55f, COL_GRAY, "[ LIVE VIDEO ]");
            draw_text( 60, 126, 0.40f, COL_GRAY, "Connecting...");
        }
    }

    /* Overlay */
    if (app.show_overlay &&
        (app.state == STATE_WATCHING || app.state == STATE_ERROR)) {
        draw_rect(0, TOP_H-48, TOP_W, 48, COL_OVERLAY);
        draw_rect(0, TOP_H-48, TOP_W,  1, COL_DIVIDER);
        char info1[96], info2[128], vc[32];
        snprintf(info1, sizeof(info1), "%s | %s",
                 app.channel[0] ? app.channel : "No channel",
                 app.stream_game[0] ? app.stream_game : "Twitch");
        snprintf(info2, sizeof(info2), "%s",
                 app.stream_title[0] ? app.stream_title : "No stream info");
        snprintf(vc, sizeof(vc), "Viewers: %d", app.viewer_count);
        draw_text(8,         TOP_H-46, 0.42f, COL_YELLOW, info1);
        draw_text(8,         TOP_H-30, 0.38f, COL_WHITE,  info2);
        draw_text(TOP_W-110, TOP_H-46, 0.36f, COL_CYAN,   vc);
        C2D_DrawCircleSolid(TOP_W-18, TOP_H-14, 0, 6,
            app.irc_connected ? COL_RED : COL_GRAY);
        draw_text(4, TOP_H-14, 0.34f, COL_GRAY, "SELECT: hide");
    } else {
        C2D_DrawCircleSolid(TOP_W-14, 10, 0, 5,
            app.irc_connected ? COL_RED : COL_GRAY);
        if (app.irc_connected)
            draw_text(TOP_W-90, 3, 0.30f, COL_GRAY, "SELECT: info");
    }
}

/* ═══════════════════════════════════════════════════════════
 * DRAW — BOTTOM SCREEN
 * ═══════════════════════════════════════════════════════════ */

static void draw_tab_bar(void) {
    int tw = BOT_W / 3;
    const char *labels[] = {" CHAT", " CHANNELS", " SETTINGS"};
    for (int i = 0; i < 3; i++) {
        u32 bg = (app.tab == (TabID)i) ? COL_TAB_ACT : COL_TAB_INACT;
        draw_rect(i*tw, 0, tw-1, TAB_H-1, bg);
        draw_text(i*tw+4, 4, 0.40f, COL_WHITE, labels[i]);
    }
    draw_rect(0, TAB_H-1, BOT_W, 1, COL_DIVIDER);
}

static void draw_chat_tab(void) {
    draw_rect(0, CHAT_TOP, BOT_W, CHAT_BOT-CHAT_TOP, COL_CHAT_BG);
    int vis   = VISIBLE_LINES; if (vis > MAX_LINES) vis = MAX_LINES;
    int total = app.line_count;
    int max_off = (total > vis) ? (total-vis) : 0;
    if (app.scroll_offset > max_off) app.scroll_offset = max_off;
    int draw_start = total - vis - app.scroll_offset;
    if (draw_start < 0) draw_start = 0;
    int draw_count = total - draw_start;
    if (draw_count > vis) draw_count = vis;
    for (int i = 0; i < draw_count; i++) {
        int idx = (app.line_head - app.line_count + draw_start + i + MAX_LINES*2) % MAX_LINES;
        float y = CHAT_TOP + 2 + i*LINE_H;
        ChatLine *cl = &app.lines[idx];
        u32 nc = COL_PURPLE_LT;
        if      (strcmp(cl->nick,"System")   == 0) nc = COL_GREEN;
        else if (strcmp(cl->nick, app.nick)  == 0) nc = COL_YELLOW;
        char nd[36]; snprintf(nd, sizeof(nd), "%s:", cl->nick);
        draw_text(CHAT_LEFT, y,    0.34f, nc,        nd);
        draw_text(CHAT_LEFT, y+11, 0.34f, COL_WHITE, cl->text);
    }

    if (app.line_count > vis) {
        draw_rect(BOT_W-6, CHAT_TOP, 5, CHAT_BOT-CHAT_TOP, COL_GRAY_DK);
        int track_h = CHAT_BOT - CHAT_TOP;
        int thumb_h = (vis * track_h) / app.line_count; if (thumb_h < 8) thumb_h = 8;
        int max_off2 = app.line_count - vis; if (max_off2 < 1) max_off2 = 1;
        int thumb_y  = CHAT_TOP + ((max_off2 - app.scroll_offset) * (track_h-thumb_h)) / max_off2;
        draw_rect(BOT_W-6, thumb_y, 5, thumb_h, COL_PURPLE);
    }
    if (app.scroll_locked) {
        draw_rect(BOT_W-70, CHAT_TOP+2, 62, 14, COL_PURPLE_DK);
        draw_text(BOT_W-68, CHAT_TOP+3, 0.32f, COL_YELLOW, "PAUSED");
    }

    draw_rect(0, CHAT_BOT, BOT_W, 1, COL_DIVIDER);
    draw_rect(0, INPUT_BAR_Y, BOT_W, INPUT_BAR_H, COL_INPUT_BG);
    draw_rect(BOT_W-46, INPUT_BAR_Y+2, 44, INPUT_BAR_H-4, COL_BTN);
    draw_text(BOT_W-40, INPUT_BAR_Y+6, 0.38f, COL_WHITE, "SEND");
    char disp[140];
    snprintf(disp, sizeof(disp), app.input[0] ? "> %s_" :
             (app.status_msg[0] ? app.status_msg : "> Tap SEND or press A..."),
             app.input);
    draw_text(4, INPUT_BAR_Y+6, 0.38f,
              app.input[0] ? COL_WHITE : COL_GRAY, disp);
}

static void draw_channels_tab(void) {
    float y = CHAT_TOP + 4;
    draw_rect(4, y, BOT_W-54, 18, COL_INPUT_BG);
    draw_text(8, y+3, 0.36f, COL_GRAY, "Search channel...");
    draw_rect(BOT_W-48, y, 44, 18, COL_BTN);
    draw_text(BOT_W-44, y+3, 0.36f, COL_WHITE, "Go");
    y += 22;
    draw_rect(4, y, BOT_W-8, 1, COL_DIVIDER); y += 4;
    draw_text(4, y, 0.36f, COL_GRAY, "Recent Channels:"); y += 14;
    for (int i = 0; i < app.history_count; i++) {
        bool sel = (i == app.history_sel);
        draw_rect(4, y, BOT_W-8, 18, sel ? COL_ROW_SEL : COL_CHAT_BG);
        draw_text(8, y+3, 0.38f, sel ? COL_YELLOW : COL_WHITE, app.history[i]);
        draw_rect(BOT_W-22, y+3, 16, 12, COL_BTN);
        draw_text(BOT_W-18, y+4, 0.34f, COL_WHITE, ">");
        y += 20;
        if (y > CHAT_BOT-20) break;
    }
    draw_rect(4, CHAT_BOT-18, 90, 16, COL_BTN_RED);
    draw_text(8, CHAT_BOT-16, 0.34f, COL_WHITE, "Clear History");
    draw_rect(0, INPUT_BAR_Y, BOT_W, INPUT_BAR_H, COL_INPUT_BG);
    char cur[64];
    snprintf(cur, sizeof(cur), "Now: %s | A=Join X=Search",
             app.channel[0] ? app.channel : "none");
    draw_text(4, INPUT_BAR_Y+6, 0.36f, COL_GRAY, cur);
}

static void draw_settings_tab(void) {
    float y = CHAT_TOP + 6;
    draw_text(4, y, 0.40f, COL_YELLOW, "LOGIN"); y += 16;
    if (app.logged_in) {
        char lbl[48]; snprintf(lbl, sizeof(lbl), "Logged in: %s", app.nick);
        draw_text(4, y, 0.36f, COL_GREEN, lbl); y += 14;
        draw_rect(4, y, 80, 16, COL_BTN_RED);
        draw_text(8, y+3, 0.34f, COL_WHITE, "Logout"); y += 20;
    } else {
        draw_text(4, y, 0.34f, COL_GRAY, "Not logged in (read-only)"); y += 14;
        draw_rect(4, y, BOT_W-8, 16, COL_BTN);
        draw_text(8, y+3, 0.34f, COL_WHITE, "Login at twitch.tv/activate");
        y += 20;
    }
    draw_rect(0, y, BOT_W, 1, COL_DIVIDER); y += 6;
    draw_text(4, y, 0.40f, COL_YELLOW, "DISPLAY"); y += 14;
    draw_text(4, y, 0.36f, COL_WHITE, "Stream Info Overlay:");
    draw_rect(BOT_W-54, y-2, 50, 16,
              app.show_overlay ? COL_TOGGLE_ON : COL_TOGGLE_OFF);
    draw_text(BOT_W-50, y+1, 0.36f, COL_WHITE,
              app.show_overlay ? " ON" : " OFF");
    y += 18;
    draw_text(4, y, 0.36f, COL_WHITE, "Video Quality:"); y += 14;
    for (int i = 0; i < 3; i++) {
        u32 bg = (app.quality == (VideoQuality)i) ? COL_PURPLE : COL_GRAY_DK;
        draw_rect(4+i*56, y, 52, 16, bg);
        draw_text(8+i*56, y+3, 0.36f, COL_WHITE, quality_labels[i]);
    }
    y += 20;
    draw_rect(0, INPUT_BAR_Y, BOT_W, INPUT_BAR_H, COL_INPUT_BG);
    draw_text(4, INPUT_BAR_Y+6, 0.34f, COL_GRAY, "Touch buttons or use D-Pad + A");
}

static void draw_bot(void) {
    C2D_TargetClear(app.bot, COL_BG_BOT);
    C2D_SceneBegin(app.bot);
    begin_text_frame();
    draw_tab_bar();
    switch (app.tab) {
        case TAB_CHAT:     draw_chat_tab();     break;
        case TAB_CHANNELS: draw_channels_tab(); break;
        case TAB_SETTINGS: draw_settings_tab(); break;
    }
}

/* ═══════════════════════════════════════════════════════════
 * INPUT HANDLING
 * ═══════════════════════════════════════════════════════════ */

static void handle_touch(touchPosition *t) {
    if (t->px == 0 && t->py == 0) return;
    int tw = BOT_W / 3;
    if (t->py < TAB_H) { app.tab = (TabID)(t->px / tw); return; }
    switch (app.tab) {
        case TAB_CHAT:
            if (touch_in(t, BOT_W-46, INPUT_BAR_Y+2, 44, INPUT_BAR_H-4)) {
                memset(app.input, 0, sizeof(app.input));
                if (swkbd_get(app.input, sizeof(app.input), "Send a message", false))
                    if (app.input[0]) {
                        irc_send_msg(app.input);
                        memset(app.input, 0, sizeof(app.input));
                    }
            }
            if (t->py >= CHAT_TOP && t->py < CHAT_BOT) {
                int mid = (CHAT_TOP + CHAT_BOT) / 2;
                if (t->py < mid) { app.scroll_offset++; app.scroll_locked = true; }
                else if (app.scroll_offset > 0) {
                    app.scroll_offset--;
                    if (app.scroll_offset == 0) app.scroll_locked = false;
                }
            }
            break;
        case TAB_CHANNELS: {
            float y = CHAT_TOP + 4;
            if (touch_in(t, BOT_W-48, (int)y, 44, 18)) {
                char chan[48] = {0};
                if (swkbd_get(chan, sizeof(chan), "Enter channel name (no #)", false))
                    if (chan[0]) join_channel(chan);
            }
            y += 26;
            for (int i = 0; i < app.history_count; i++) {
                if (touch_in(t, 4, (int)y, BOT_W-8, 18)) {
                    join_channel(app.history[i]); return;
                }
                y += 20; if (y > CHAT_BOT-20) break;
            }
            if (touch_in(t, 4, CHAT_BOT-18, 90, 16)) {
                app.history_count = 1;
                strncpy(app.history[0], app.channel, 47);
                save_history();
            }
            break;
        }
        case TAB_SETTINGS: {
            float y = CHAT_TOP + 22;
            if (app.logged_in) {
                if (touch_in(t, 4, (int)y, 80, 16)) do_logout();
            } else {
                if (touch_in(t, 4, (int)y, BOT_W-8, 16)) do_device_login_start();
            }
            y += 48;
            if (touch_in(t, BOT_W-54, (int)y-2, 50, 16)) {
                app.show_overlay = !app.show_overlay; save_settings();
            }
            y += 32;
            for (int i = 0; i < 3; i++)
                if (touch_in(t, 4+i*56, (int)y, 52, 16)) {
                    app.quality = (VideoQuality)i; save_settings();
                }
            break;
        }
    }
}

static void handle_buttons(u32 kDown) {
    if (kDown & KEY_L) app.tab = (TabID)((app.tab + 2) % 3);
    if (kDown & KEY_R) app.tab = (TabID)((app.tab + 1) % 3);
    if (kDown & KEY_SELECT) { app.show_overlay = !app.show_overlay; save_settings(); }
    if (kDown & KEY_Y) app.tab = TAB_CHANNELS;
    switch (app.tab) {
        case TAB_CHAT:
            if (kDown & KEY_DUP)   { app.scroll_offset++; app.scroll_locked = true; }
            if (kDown & KEY_DDOWN) {
                if (app.scroll_offset > 0) {
                    app.scroll_offset--;
                    if (app.scroll_offset == 0) app.scroll_locked = false;
                }
            }
            if (kDown & KEY_A) {
                memset(app.input, 0, sizeof(app.input));
                if (swkbd_get(app.input, sizeof(app.input), "Send a message", false))
                    if (app.input[0]) {
                        irc_send_msg(app.input);
                        memset(app.input, 0, sizeof(app.input));
                    }
            }
            if (kDown & KEY_B) memset(app.input, 0, sizeof(app.input));
            break;
        case TAB_CHANNELS:
            if (kDown & KEY_DUP)   { if (app.history_sel > 0) app.history_sel--; }
            if (kDown & KEY_DDOWN) { if (app.history_sel < app.history_count-1) app.history_sel++; }
            if (kDown & KEY_A)     { if (app.history_count > 0) join_channel(app.history[app.history_sel]); }
            if (kDown & KEY_X) {
                char chan[48] = {0};
                if (swkbd_get(chan, sizeof(chan), "Enter channel name (no #)", false))
                    if (chan[0]) join_channel(chan);
            }
            break;
        case TAB_SETTINGS:
            break;
    }
}

/* ═══════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════ */

int main(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    romfsInit();
    cfguInit();
    acInit();
    socInit((u32*)memalign(0x1000, 0x100000), 0x100000);
    curl_global_init(CURL_GLOBAL_ALL);
    video_init();

    app.top  = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    app.bot  = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    app.font = C2D_FontLoadSystem(CFG_REGION_USA);
    app.tbuf = C2D_TextBufNew(16384);

    app.sock              = -1;
    app.irc_connected     = false;
    app.tab               = TAB_CHAT;
    app.state             = STATE_ERROR;
    app.line_head         = 0;
    app.line_count        = 0;
    app.scroll_offset     = 0;
    app.scroll_locked     = false;
    app.history_sel       = 0;
    app.vid_ever_started  = false;
    memset(app.lines,  0, sizeof(app.lines));
    memset(app.input,  0, sizeof(app.input));
    app.thumb_loaded     = false;
    app.thumb_last_tick  = 0;
    C3D_TexInit(&app.thumb_tex, THUMB_TEX_W, THUMB_TEX_H, GPU_RGBA8);
    C3D_TexSetFilter(&app.thumb_tex, GPU_LINEAR, GPU_LINEAR);
    app.thumb_st  = (Tex3DS_SubTexture){ THUMB_W, THUMB_H,
        0.0f, (float)THUMB_H/THUMB_TEX_H,
        (float)THUMB_W/THUMB_TEX_W, 0.0f };
    app.thumb_img = (C2D_Image){ &app.thumb_tex, &app.thumb_st };

    memset(&app.dcf, 0, sizeof(app.dcf));
    strcpy(app.stream_title, "No stream metadata yet");
    strcpy(app.stream_game,  "Twitch");
    app.viewer_count = 0;

    mkdir_config();
    { FILE *f = fopen("/config/twitch3ds.log","w"); if(f) fclose(f); }
    LOG("=== twitch3ds start ===");
    load_token();
    load_settings();
    load_history();

    chat_push("System","Welcome to Twitch3DS!");
    chat_push("System","Device Code login now available.");

    if (irc_connect()) {
        app.state = STATE_WATCHING;
        const char *ch = app.channel[0]=='#' ? app.channel+1 : app.channel;
        video_start(ch, app.oauth, CLIENT_ID);
        app.vid_ever_started = true;
    } else {
        app.state = STATE_ERROR;
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        touchPosition touch;
        hidTouchRead(&touch);

        if (kDown & KEY_START) break;

        handle_buttons(kDown);

        static bool was_touching = false;
        bool is_touching = (touch.px != 0 || touch.py != 0);
        if (is_touching && !was_touching) handle_touch(&touch);
        was_touching = is_touching;

        if (app.dcf.polling) dcf_poll_tick();

        /* IRC reconnect — does NOT restart video */
        if (app.state == STATE_WATCHING && !app.irc_connected) {
            LOG("main: IRC not connected, reconnecting...");
            if (irc_connect()) set_status("IRC reconnected");
        }

        if (app.state == STATE_WATCHING) irc_poll();

        video_upload_frame();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        draw_top();
        draw_bot();
        C3D_FrameEnd(0);
    }

    save_settings();
    irc_disconnect();
    video_exit();
    curl_global_cleanup();
    C2D_TextBufDelete(app.tbuf);
    C2D_FontFree(app.font);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    socExit();
    acExit();
    cfguExit();
    romfsExit();
    return 0;
}
