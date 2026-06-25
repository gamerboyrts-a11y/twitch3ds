#include "video.h"
#include "log.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <3ds/services/mvd.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <citro3d.h>


/* sizing */
#define OUT_W   400
#define OUT_H   240
#define TEX_W   512
#define TEX_H   256
#define NAL_MAX (256*1024)
#define PES_MAX (512*1024)
#define SEG_MAX (4*1024*1024)


/* state */
static struct {
    bool   active, offline, has_frame, mvd_ok, tex_valid;
    Thread thread;
    LightLock lock;
    u8    *outbuf;
    u8    *nalbuf;
    u8    *stgbuf;
    C3D_Tex           tex;
    Tex3DS_SubTexture subtex;
    C2D_Image         img;
    MVDSTD_Config     cfg;
    char channel[48];
    char oauth[128];
    char client_id[48];
    char hls_url[4096];
    char last_seg[2048];
    char usher_resolve[64];
} V;


/* bearer token helper — strips "PASS " / "oauth:" prefix */
static const char *bearer_token(void) {
    const char *t = V.oauth;
    if (strncmp(t, "PASS ", 5) == 0) t += 5;
    if (strncmp(t, "oauth:", 6) == 0) t += 6;
    return (t[0] && strcmp(t, "schmoopiie") != 0) ? t : NULL;
}


/* curl write callback */
typedef struct { char *d; size_t len, cap; } Buf;
static size_t cb(void *p, size_t s, size_t n, void *u) {
    Buf *b = u; size_t in = s*n;
    if (b->len+in+1 > b->cap) {
        char *t = realloc(b->d, b->cap+in+4096);
        if (!t) return 0;
        b->d = t; b->cap += in+4096;
    }
    memcpy(b->d+b->len, p, in); b->len += in; b->d[b->len] = 0;
    return in;
}


/* make relative URL absolute using a base URL */
static char *resolve_url(const char *url, const char *base_url) {
    if (!url || !url[0]) return NULL;
    if (strncmp(url, "http", 4) == 0) return strdup(url);
    char base[2048] = {0};
    if (base_url) {
        const char *last_slash = strrchr(base_url, '/');
        if (last_slash) {
            size_t l = (size_t)(last_slash - base_url) + 1;
            if (l >= sizeof(base)) l = sizeof(base)-1;
            strncpy(base, base_url, l);
        }
    }
    char *full = malloc(2048);
    if (!full) return NULL;
    snprintf(full, 2048, "%s%s", base, url);
    return full;
}


/* Generic HTTPS GET */
static char *http_get(const char *url, const char *auth_hdr) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    Buf b = {malloc(4096), 0, 4096};
    if (!b.d) { curl_easy_cleanup(c); return NULL; }
    b.d[0] = 0;


    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_PATH_AS_IS, 1L);


    struct curl_slist *resolve_list = NULL;
    if (V.usher_resolve[0] && strstr(url, "usher.ttvnw.net")) {
        resolve_list = curl_slist_append(NULL, V.usher_resolve);
        curl_easy_setopt(c, CURLOPT_RESOLVE, resolve_list);
    }


    struct curl_slist *headers = NULL;
    if (auth_hdr)
        headers = curl_slist_append(headers, auth_hdr);
    if (strstr(url, "usher.ttvnw.net")) {
        char cid_hdr[80];
        snprintf(cid_hdr, sizeof(cid_hdr), "Client-ID: %s", V.client_id);
        headers = curl_slist_append(headers, cid_hdr);
    }
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);


    CURLcode res = curl_easy_perform(c);
    if (headers) curl_slist_free_all(headers);


    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (resolve_list) curl_slist_free_all(resolve_list);


    LOG("http_get code=%ld curl=%d url=%.80s", code, (int)res, url);


    if (code != 200) {
        LOG("http_get FAILED body(%.120s)", b.d ? b.d : "(null)");
        free(b.d); return NULL;
    }
    return b.d;
}


/* Fetch GQL playback token and build HLS URL */
static bool fetch_hls_url(void) {
    char body[512];
    snprintf(body, sizeof(body),
        "[{\"operationName\":\"PlaybackAccessToken\","
        "\"query\":\"query PlaybackAccessToken($login:String!,$playerType:String!)"
        "{streamPlaybackAccessToken(channelName:$login,"
        "params:{platform:\\\"web\\\",playerBackend:\\\"mediaplayer\\\","
        "playerType:$playerType}){value signature}}\","
        "\"variables\":{\"login\":\"%s\",\"playerType\":\"site\"}}]",
        V.channel);


    CURL *c = curl_easy_init(); if (!c) return false;
    Buf buf = {malloc(8192), 0, 8192};
    if (!buf.d) { curl_easy_cleanup(c); return false; }
    buf.d[0] = 0;


    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "Content-Type: application/json");
    char gql_cid_hdr[80];
    snprintf(gql_cid_hdr, sizeof(gql_cid_hdr), "Client-ID: %s", V.client_id);
    h = curl_slist_append(h, gql_cid_hdr);


    const char *tok = bearer_token();
    if (tok) {
        char ahdr[4200];
        snprintf(ahdr, sizeof(ahdr), "Authorization: Bearer %s", tok);
        h = curl_slist_append(h, ahdr);
    }


    curl_easy_setopt(c, CURLOPT_URL, "https://gql.twitch.tv/gql");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    CURLcode gql_res = curl_easy_perform(c);
    long gql_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &gql_code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    LOG("vid: GQL curl=%d http=%ld resp(%.200s)", (int)gql_res, gql_code, buf.d ? buf.d : "(null)");


    if (!buf.d || !buf.d[0]) {
        free(buf.d);
        LOG("vid: GQL empty response");
        return false;
    }


    char sig[128]={0}, token_raw[2048]={0};


    /* Parse signature */
    const char *ps = strstr(buf.d, "\"signature\"");
    if (ps) {
        ps += 11;
        while (*ps==':'||*ps==' ') ps++;
        if (*ps=='"') {
            ps++;
            const char *e = strchr(ps, '"');
            if (e) {
                size_t l = (size_t)(e-ps);
                if (l >= sizeof(sig)) l = sizeof(sig)-1;
                strncpy(sig, ps, l); sig[l] = 0;
            }
        }
    }


    const char *pv = strstr(buf.d, "\"value\"");
    if (pv) {
        pv += 7;
        while (*pv==':'||*pv==' ') pv++;
        if (*pv=='"') {
            pv++;
            int i = 0;
            while (*pv && i < (int)sizeof(token_raw)-1) {
                if (*pv=='"' && (i==0 || *(pv-1)!='\\')) break;
                token_raw[i++] = *pv++;
            }
            token_raw[i] = 0;
        }
    }


    free(buf.d);


    if (!sig[0] || !token_raw[0]) {
        LOG("vid: GQL missing sig or token");
        return false;
    }


    char token_unesc[2048] = {0};
    {
        const char *src = token_raw;
        int di = 0;
        while (*src && di < (int)sizeof(token_unesc)-1) {
            if (*src == '\\' && *(src+1) == '"') {
                token_unesc[di++] = '"'; src += 2;
            } else if (*src == '\\' && *(src+1) == '\\') {
                token_unesc[di++] = '\\'; src += 2;
            } else {
                token_unesc[di++] = *src++;
            }
        }
        token_unesc[di] = 0;
    }


    CURL *ce = curl_easy_init(); if (!ce) return false;
    char *etok = curl_easy_escape(ce, token_unesc, 0);
    curl_easy_cleanup(ce);
    if (!etok) return false;


    const char *raw_oauth = bearer_token();


    char lc[48] = {0};
    for (int i = 0; V.channel[i] && i < (int)sizeof(lc)-1; i++)
        lc[i] = (V.channel[i]>='A' && V.channel[i]<='Z') ?
                  V.channel[i]+32 : V.channel[i];


    if (raw_oauth && raw_oauth[0]) {
        snprintf(V.hls_url, sizeof(V.hls_url),
            "https://usher.ttvnw.net/api/channel/hls/%s.m3u8"
            "?sig=%s&token=%s&oauth_token=%s"
            "&allow_source=true&allow_spectre=true"
            "&p=%d&play_session_id=twitch3ds&fast_breadcrumbs=true",
            lc, sig, etok, raw_oauth,
            (int)(svcGetSystemTick() & 0x7fffffff));
    } else {
        snprintf(V.hls_url, sizeof(V.hls_url),
            "https://usher.ttvnw.net/api/channel/hls/%s.m3u8"
            "?sig=%s&token=%s"
            "&allow_source=true&allow_spectre=true"
            "&p=%d&play_session_id=twitch3ds&fast_breadcrumbs=true",
            lc, sig, etok,
            (int)(svcGetSystemTick() & 0x7fffffff));
    }
    curl_free(etok);


    LOG("vid: token_unesc(%.60s...)", token_unesc);


    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("usher.ttvnw.net", NULL, &hints, &res) == 0) {
        char ip[32] = {0};
        inet_ntop(AF_INET,
            &((struct sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
        freeaddrinfo(res);
        snprintf(V.usher_resolve, sizeof(V.usher_resolve),
            "usher.ttvnw.net:443:%s", ip);
        LOG("vid: usher resolved to %s", ip);
    } else {
        LOG("vid: usher getaddrinfo FAILED");
        V.usher_resolve[0] = 0;
    }
    return true;
}


/* Lowest-bandwidth variant URL from master m3u8; fills out_w/out_h if non-NULL */
static char *m3u8_lowest_variant(const char *body, const char *base_url,
                                  int *out_w, int *out_h) {
    long best_bw = LONG_MAX;
    char best_url[2048] = {0};
    int  best_w = 0, best_h = 0;
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-STREAM-INF:"))) {
        long bw = 0;
        const char *bwp = strstr(p, "BANDWIDTH=");
        if (bwp) bw = atol(bwp+10);
        int w = 0, h = 0;
        const char *rp = strstr(p, "RESOLUTION=");
        if (rp) { rp += 11; w = atoi(rp); const char *x = strchr(rp,'x'); if (x) h = atoi(x+1); }
        const char *nl = strchr(p, '\n'); if (!nl) break;
        nl++;
        while (*nl=='\r'||*nl=='\n') nl++;
        if (*nl && *nl!='#') {
            const char *eol = nl;
            while (*eol && *eol!='\r' && *eol!='\n') eol++;
            if (bw < best_bw) {
                best_bw = bw; best_w = w; best_h = h;
                size_t l = (size_t)(eol-nl);
                if (l >= sizeof(best_url)) l = sizeof(best_url)-1;
                strncpy(best_url, nl, l); best_url[l] = 0;
            }
        }
        p++;
    }
    if (!best_url[0]) return NULL;
    if (out_w) *out_w = best_w;
    if (out_h) *out_h = best_h;
    return resolve_url(best_url, base_url);
}


/* Last .ts segment URL from media m3u8 */
static char *m3u8_last_segment(const char *body, const char *base_url) {
    char last[2048] = {0};
    const char *p = body;
    while ((p = strstr(p, ".ts"))) {
        const char *s = p;
        while (s > body && *(s-1)!='\n' && *(s-1)!='\r') s--;
        const char *e = p+3;
        while (*e && *e!='\r' && *e!='\n') e++;
        size_t l = (size_t)(e-s);
        if (l >= sizeof(last)) l = sizeof(last)-1;
        strncpy(last, s, l); last[l]=0;
        p++;
    }
    if (!last[0]) return NULL;
    return resolve_url(last, base_url);
}


/* MPEG-TS demuxer */
static struct {
    int  pmt_pid, vid_pid;
    u8   pes[PES_MAX];
    int  pes_len;
    bool pes_valid;
} TS;


static void ts_reset(void) {
    TS.pmt_pid = -1; TS.vid_pid = -1; TS.pes_len = 0; TS.pes_valid = false;
}


static void nal_feed(const u8 *data, int len) {
    if (len <= 3 || len > NAL_MAX) return;
    LOG("nal len=%d type=%d", len, data[0] & 0x1f);
    memcpy(V.nalbuf, data, len);
    GSPGPU_FlushDataCache(V.nalbuf, len);
    Result r = mvdstdProcessVideoFrame(V.nalbuf, len, 0, NULL);
    if (MVD_CHECKNALUPROC_SUCCESS(r) && (r == MVD_STATUS_FRAMEREADY)) {
        mvdstdRenderVideoFrame(NULL, true);
        LightLock_Lock(&V.lock);
        V.has_frame = true;
        LightLock_Unlock(&V.lock);
        LOG("nal: FRAMEREADY r=0x%lx", r);
    }
}


static void pes_flush(void) {
    if (!TS.pes_valid || TS.pes_len < 9) return;
    int hdr = 9 + TS.pes[8];
    if (hdr >= TS.pes_len) return;
    const u8 *h = TS.pes + hdr;
    int hlen = TS.pes_len - hdr;
    int i = 0;
    while (i < hlen-3) {
        bool sc4 = (i+3 < hlen) &&
                   h[i]==0 && h[i+1]==0 && h[i+2]==0 && h[i+3]==1;
        bool sc3 = h[i]==0 && h[i+1]==0 && h[i+2]==1;
        if (!sc3 && !sc4) { i++; continue; }
        int start = sc4 ? i+4 : i+3;
        int j = start+1;
        while (j < hlen-2) {
            if (h[j]==0 && h[j+1]==0 && h[j+2]==1) break;
            if (j+3 < hlen &&
                h[j]==0 && h[j+1]==0 && h[j+2]==0 && h[j+3]==1) break;
            j++;
        }
        nal_feed(h+start, j-start);
        i = j;
    }
    TS.pes_len = 0; TS.pes_valid = false;
}


static void ts_packet(const u8 *p) {
    if (p[0] != 0x47) return;
    int pid   = ((p[1]&0x1F)<<8) | p[2];
    bool pusi = (p[1]>>6) & 1;
    int afc   = (p[3]>>4) & 3;
    int off   = 4;
    if (afc & 2) off += 1+p[4];
    if (!(afc & 1) || off >= 188) return;
    const u8 *pay = p+off; int plen = 188-off;


    if (pid==0 && TS.pmt_pid<0) {
        const u8 *s = pay+1+pay[0];
        if (s[0]==0x00) {
            const u8 *e = s+3+(((s[1]&0x0f)<<8)|s[2])-4;
            const u8 *q = s+8;
            while (q+3 < e) {
                int pn = (q[0]<<8)|q[1];
                int pp = ((q[2]&0x1f)<<8)|q[3];
                if (pn) { TS.pmt_pid = pp; break; }
                q += 4;
            }
        }
    } else if (pid==TS.pmt_pid && TS.vid_pid<0) {
        const u8 *s = pay+1+pay[0];
        if (s[0]==0x02) {
            int sl  = ((s[1]&0x0f)<<8)|s[2];
            int pil = ((s[10]&0x0f)<<8)|s[11];
            const u8 *es = s+12+pil;
            const u8 *e  = s+3+sl-4;
            while (es+4 < e) {
                if (es[0]==0x1B) {
                    TS.vid_pid = ((es[1]&0x1f)<<8)|es[2]; break;
                }
                es += 5 + (((es[3]&0x0f)<<8) | es[4]);
            }
        }
    } else if (pid==TS.vid_pid) {
        if (pusi) pes_flush();
        if (pusi) TS.pes_valid = true;
        if (TS.pes_valid) {
            int copy = plen;
            if (TS.pes_len+copy > PES_MAX) copy = PES_MAX-TS.pes_len;
            if (copy > 0) {
                memcpy(TS.pes+TS.pes_len, pay, copy);
                TS.pes_len += copy;
            }
        }
    }
}


static void process_segment(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return;
    Buf b = {malloc(SEG_MAX/4), 0, SEG_MAX/4};
    if (!b.d) { curl_easy_cleanup(c); return; }
    b.d[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    LOG("vid: seg dl code=%ld curl=%d len=%d", code, (int)res, (int)b.len);
    if (b.len >= 188) {
        ts_reset();
        for (size_t i = 0; i+188 <= b.len; i += 188)
            ts_packet((const u8*)b.d+i);
        pes_flush();
        LOG("vid: demux done pmt=%d vid=%d", TS.pmt_pid, TS.vid_pid);
    }
    free(b.d);
}


/* Background video thread */
static void vid_thread(void *arg) {
    (void)arg;
    char variant_url[2048] = {0};


    LOG("vid: fetching token for %s (cid=%.12s...)", V.channel, V.client_id);
    if (!fetch_hls_url()) {
        LOG("vid: fetch_hls_url FAILED");
        LightLock_Lock(&V.lock); V.offline = true; LightLock_Unlock(&V.lock);
        return;
    }
    LOG("vid: hls_url=%.80s", V.hls_url);


    char *master = http_get(V.hls_url, NULL);
    LOG("vid: master m3u8 %s len=%d",
        master ? "OK" : "FAILED", master ? (int)strlen(master) : 0);
    if (!master) {
        LOG("vid: master FAILED - channel offline or 403");
        LightLock_Lock(&V.lock); V.offline = true; LightLock_Unlock(&V.lock);
        return;
    }


    int in_w = 0, in_h = 0;
    char *var = m3u8_lowest_variant(master, V.hls_url, &in_w, &in_h);
    free(master);
    if (!var) {
        LOG("vid: no variant in master m3u8");
        LightLock_Lock(&V.lock); V.offline = true; LightLock_Unlock(&V.lock);
        return;
    }
    strncpy(variant_url, var, sizeof(variant_url)-1);
    free(var);
    if (in_w <= 0 || in_h <= 0) { in_w = OUT_W; in_h = OUT_H; }
    LOG("vid: variant=%.120s res=%dx%d", variant_url, in_w, in_h);

    mvdstdGenerateDefaultConfig(&V.cfg, in_w, in_h, OUT_W, OUT_H,
        NULL, NULL, NULL);
    MVDSTD_SetConfig(&V.cfg);

    /* Route MVD output to V.outbuf via the entry list — passing vaddr_outdata0
       to mvdstdGenerateDefaultConfig does not work; SetupOutputBuffers does. */
    MVDSTD_OutputBuffersEntryList elist;
    memset(&elist, 0, sizeof(elist));
    elist.total_entries = 1;
    elist.entries[0].outdata0 = V.outbuf;
    elist.entries[0].outdata1 = NULL;
    Result msr = mvdstdSetupOutputBuffers(&elist, TEX_W * TEX_H * 2);
    LOG("vid: SetupOutputBuffers r=0x%lx outbuf=%p", msr, V.outbuf);
    LOG("vid: MVD ready, streaming...");


    int seg_count = 0;
    while (V.active) {
        char *media = http_get(variant_url, NULL);
        if (!media) {
            LOG("vid: media m3u8 fetch failed");
            svcSleepThread(2000000000LL);
            continue;
        }


        char *seg = m3u8_last_segment(media, variant_url);
        free(media);


        if (!seg) {
            LOG("vid: no .ts in media m3u8");
            svcSleepThread(2000000000LL);
            continue;
        }


        if (strcmp(seg, V.last_seg) != 0) {
            LOG("vid: seg #%d %.80s", ++seg_count, seg);
            strncpy(V.last_seg, seg, sizeof(V.last_seg)-1);
            process_segment(seg);
        }
        free(seg);
        svcSleepThread(2000000000LL);
    }
    LOG("vid: thread exit");
}


/* Public API */
bool video_init(void) {
    Result r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264,
        MVD_OUTPUT_RGB565, MVD_DEFAULT_WORKBUF_SIZE, NULL);
    if (R_FAILED(r)) { LOG("vid: mvdstdInit FAILED 0x%lx", r); return false; }
    V.mvd_ok = true;
    V.outbuf = linearAlloc(TEX_W * TEX_H * 2);
    V.nalbuf = linearAlloc(NAL_MAX);
    V.stgbuf = linearAlloc(TEX_W * TEX_H * 2);
    if (!V.outbuf || !V.nalbuf || !V.stgbuf) {
        LOG("vid: linearAlloc FAILED"); mvdstdExit(); return false;
    }
    memset(V.outbuf, 0, TEX_W * TEX_H * 2);
    memset(V.stgbuf, 0, TEX_W * TEX_H * 2);
    LightLock_Init(&V.lock);
    C3D_TexInit(&V.tex, TEX_W, TEX_H, GPU_RGB565);
    C3D_TexSetFilter(&V.tex, GPU_LINEAR, GPU_LINEAR);
    V.subtex = (Tex3DS_SubTexture){ OUT_W, OUT_H,
        0.0f, (float)OUT_H/TEX_H, (float)OUT_W/TEX_W, 0.0f };
    V.img = (C2D_Image){ &V.tex, &V.subtex };
    V.tex_valid = false;
    LOG("vid: init OK");
    return true;
}


void video_exit(void) {
    video_stop();
    if (V.mvd_ok)  { mvdstdExit(); V.mvd_ok = false; }
    if (V.outbuf)  { linearFree(V.outbuf); V.outbuf = NULL; }
    if (V.nalbuf)  { linearFree(V.nalbuf); V.nalbuf = NULL; }
    if (V.stgbuf)  { linearFree(V.stgbuf); V.stgbuf = NULL; }
    C3D_TexDelete(&V.tex);
}


void video_start(const char *channel, const char *oauth_pass,
                 const char *client_id) {
    video_stop();
    strncpy(V.channel,   channel,    sizeof(V.channel)-1);
    strncpy(V.oauth,     oauth_pass, sizeof(V.oauth)-1);
    strncpy(V.client_id, client_id,  sizeof(V.client_id)-1);
    V.hls_url[0] = 0; V.last_seg[0] = 0;
    V.has_frame = false; V.offline = false; V.active = true;
    /* Do NOT reset tex_valid here — keep showing last frame during restart */
    /* Run on core 2 (New3DS second app core) so it never preempts the render loop on core 0 */
    V.thread = threadCreate(vid_thread, NULL, 128*1024, 0x19, 2, false);
    LOG("video_start: ch=%s cid=%.12s... thread=%p",
        V.channel, V.client_id, (void*)V.thread);
}


void video_stop(void) {
    if (!V.active) return;
    V.active = false;
    if (V.thread) {
        threadJoin(V.thread, U64_MAX);
        threadFree(V.thread);
        V.thread = NULL;
    }
    /* tex_valid intentionally NOT cleared — hold last frame on screen */
}


/* Call this BEFORE C3D_FrameBegin to upload any pending decoded frame to the texture. */
void video_upload_frame(void) {
    LightLock_Lock(&V.lock);
    bool ready = V.has_frame;
    if (ready) V.has_frame = false;
    LightLock_Unlock(&V.lock);

    if (!ready) return;

    /* Direct GX transfer from MVD outbuf — GX reads physical DRAM, bypasses CPU cache */
    C3D_SyncDisplayTransfer(
        (u32*)V.outbuf,   GX_BUFFER_DIM(OUT_W, OUT_H),
        (u32*)V.tex.data, GX_BUFFER_DIM(TEX_W, TEX_H),
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565)  |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
        GX_TRANSFER_FLIP_VERT(1)                       |
        GX_TRANSFER_OUT_TILED(1)                       |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    C3D_TexFlush(&V.tex);
    V.tex_valid = true;
}

/* Call this inside a C3D frame to draw the last uploaded texture. */
void video_draw_top(float x, float y) {
    if (V.tex_valid)
        C2D_DrawImageAt(V.img, x, y, 0.5f, NULL, 1.0f, 1.0f);
}


bool video_is_offline(void) {
    LightLock_Lock(&V.lock); bool r = V.offline; LightLock_Unlock(&V.lock);
    return r;
}
bool video_is_active(void) { return V.active; }
