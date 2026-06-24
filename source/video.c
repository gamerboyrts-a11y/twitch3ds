#include "video.h"
#include "log.h"
#include <3ds/services/mvd.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <citro3d.h>

/* ── sizing ────────────────────────────────────────────────── */
#define OUT_W     400
#define OUT_H     240
#define TEX_W     512
#define TEX_H     256
#define NAL_MAX   (256*1024)
#define PES_MAX   (512*1024)
#define SEG_MAX   (4*1024*1024)

/* ── state ─────────────────────────────────────────────────── */
static struct {
    bool            active, offline, has_frame, mvd_ok;
    Thread          thread;
    LightLock       lock;
    u8             *outbuf;   /* linearAlloc, RGB565 MVD output     */
    u8             *nalbuf;   /* linearAlloc, NAL unit input        */
    u8             *stgbuf;   /* linearAlloc, pre-allocated staging */
    C3D_Tex         tex;
    Tex3DS_SubTexture subtex;
    C2D_Image       img;
    MVDSTD_Config   cfg;
    char            channel[48];
    char            oauth[128];
    char            client_id[48];
    char            hls_url[4096];
    char            last_seg[512];
} V;

/* ── curl write callback ───────────────────────────────────── */
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

static char *http_get(const char *url, const char *auth_hdr) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    Buf b = {malloc(4096), 0, 4096};
    if (!b.d) { curl_easy_cleanup(c); return NULL; }
    b.d[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &b);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE,    CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION,1L);
    CURLcode res;
    if (auth_hdr) {
        struct curl_slist *h = curl_slist_append(NULL, auth_hdr);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        res = curl_easy_perform(c);
        curl_slist_free_all(h);
    } else {
        res = curl_easy_perform(c);
    }
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (code != 200) {
        LOG("http_get curl=%d http=%ld urllen=%d url=%.80s", (int)res, code, (int)strlen(url), url);
        free(b.d); return NULL;
    }
    return b.d;
}

/* ── Twitch HLS URL via GQL ────────────────────────────────── */
static bool fetch_hls_url(void) {
    char body[512];
    snprintf(body, sizeof(body),
        "[{\"operationName\":\"PlaybackAccessToken\","
        "\"query\":\"query PlaybackAccessToken($login:String!,$playerType:String!)"
        "{streamPlaybackAccessToken(channelName:$login,"
        "params:{platform:\\\"web\\\",playerBackend:\\\"mediaplayer\\\",playerType:$playerType})"
        "{value signature}}\","
        "\"variables\":{\"login\":\"%s\",\"playerType\":\"site\"}}]",
        V.channel);

    CURL *c = curl_easy_init(); if (!c) return false;
    Buf buf = {malloc(8192), 0, 8192};
    if (!buf.d) { curl_easy_cleanup(c); return false; }
    buf.d[0] = 0;
    struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
    h = curl_slist_append(h, "Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko");
    curl_easy_setopt(c, CURLOPT_URL,           "https://gql.twitch.tv/gql");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE,    CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       15L);
    curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    LOG("vid: GQL resp(%.200s)", buf.d ? buf.d : "(null)");

    char sig[128]={0}, token[2048]={0};
    const char *ps = strstr(buf.d, "\"signature\"");
    if (ps) { ps+=12; while(*ps==':'||*ps==' ')ps++;
              if(*ps=='"'){ ps++; const char *e=strchr(ps,'"');
                if(e){ size_t l=e-ps<sizeof(sig)-1?e-ps:sizeof(sig)-1; strncpy(sig,ps,l); }}}
    const char *pv = strstr(buf.d, "\"value\"");
    if (pv) { pv+=8; while(*pv==':'||*pv==' ')pv++;
              if(*pv=='"'){ pv++;
                int i=0; while(*pv&&i<(int)sizeof(token)-1){
                    if(*pv=='"'&&(pv==buf.d||*(pv-1)!='\\'))break;
                    token[i++]=*pv++; } token[i]=0; }}
    free(buf.d);
    if (!sig[0] || !token[0]) return false;

    CURL *ce = curl_easy_init(); if (!ce) return false;
    char *etok = curl_easy_escape(ce, token, 0);
    curl_easy_cleanup(ce);
    if (!etok) return false;
    snprintf(V.hls_url, sizeof(V.hls_url),
        "https://usher.twitchapps.com/api/channel/hls/%s.m3u8"
        "?sig=%s&token=%s&allow_source=true&type=any&fast_breadcrumbs=true",
        V.channel, sig, etok);
    curl_free(etok);
    return true;
}

/* ── M3U8 helpers ──────────────────────────────────────────── */

/* From master m3u8: return url of lowest-bandwidth variant.
   Caller must free. */
static char *m3u8_lowest_variant(const char *body) {
    long   best_bw = LONG_MAX;
    char   best_url[512] = {0};
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-STREAM-INF:"))) {
        long bw = 0;
        const char *bwp = strstr(p, "BANDWIDTH=");
        if (bwp) bw = atol(bwp+10);
        const char *nl = strchr(p, '\n'); if (!nl) break;
        nl++; /* skip blank lines */
        while (*nl=='\r'||*nl=='\n') nl++;
        if (*nl && *nl!='#') {
            const char *eol = nl; while(*eol&&*eol!='\r'&&*eol!='\n') eol++;
            if (bw < best_bw) {
                best_bw = bw;
                size_t l = eol-nl < (int)sizeof(best_url)-1 ? eol-nl : sizeof(best_url)-1;
                strncpy(best_url, nl, l); best_url[l] = 0;
            }
        }
        p++;
    }
    return best_url[0] ? strdup(best_url) : NULL;
}

/* From media m3u8: return last .ts segment url.  Caller must free. */
static char *m3u8_last_segment(const char *body) {
    char last[512] = {0};
    const char *p = body;
    while ((p = strstr(p, ".ts"))) {
        /* walk back to start of this URL */
        const char *s = p;
        while (s > body && *(s-1) != '\n' && *(s-1) != '\r') s--;
        const char *e = p+3;
        while (*e && *e != '\r' && *e != '\n') e++;
        size_t l = e-s < (int)sizeof(last)-1 ? e-s : sizeof(last)-1;
        strncpy(last, s, l); last[l]=0;
        p++;
    }
    return last[0] ? strdup(last) : NULL;
}

/* ── MPEG-TS demuxer ───────────────────────────────────────── */
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
    memcpy(V.nalbuf, data, len);
    GSPGPU_FlushDataCache(V.nalbuf, len);
    Result r = mvdstdProcessVideoFrame(V.nalbuf, len, 0, NULL);
    if (MVD_CHECKNALUPROC_SUCCESS(r) && r == MVD_STATUS_FRAMEREADY) {
        mvdstdRenderVideoFrame(&V.cfg, true); /* render to V.outbuf on thread */
        LightLock_Lock(&V.lock);
        V.has_frame = true;
        LightLock_Unlock(&V.lock);
    }
}

static void pes_flush(void) {
    if (!TS.pes_valid || TS.pes_len < 9) return;
    int hdr = 9 + TS.pes[8];
    if (hdr >= TS.pes_len) return;
    const u8 *h = TS.pes + hdr;
    int hlen = TS.pes_len - hdr;
    /* split on Annex B start codes and feed each NAL */
    int i = 0;
    while (i < hlen-3) {
        bool sc4 = (i+3<hlen && h[i]==0&&h[i+1]==0&&h[i+2]==0&&h[i+3]==1);
        bool sc3 = (h[i]==0&&h[i+1]==0&&h[i+2]==1);
        if (!sc3 && !sc4) { i++; continue; }
        int start = sc4 ? i+1 : i; /* point to 00 00 01 */
        int j = start+3;
        while (j < hlen-2) {
            if (h[j]==0&&h[j+1]==0&&h[j+2]==1) break;
            if (j+3<hlen&&h[j]==0&&h[j+1]==0&&h[j+2]==0&&h[j+3]==1) break;
            j++;
        }
        nal_feed(h+start, j-start);
        i = j;
    }
    TS.pes_len = 0; TS.pes_valid = false;
}

static void ts_packet(const u8 *p) {
    if (p[0] != 0x47) return;
    int pid  = ((p[1]&0x1F)<<8)|p[2];
    bool pusi = (p[1]>>6)&1;
    int afc  = (p[3]>>4)&3;
    int off  = 4;
    if (afc&2) off += 1+p[4];
    if (!(afc&1)||off>=188) return;
    const u8 *pay = p+off; int plen = 188-off;

    if (pid==0 && TS.pmt_pid<0) {
        const u8 *s = pay+1+pay[0]; /* skip pointer */
        if (s[0]==0x00) {
            const u8 *e = s+3+(((s[1]&0x0f)<<8)|s[2])-4, *q=s+8;
            while (q+3<e) { int pn=(q[0]<<8)|q[1],pp=((q[2]&0x1f)<<8)|q[3];
                if (pn) { TS.pmt_pid=pp; break; } q+=4; }
        }
    } else if (pid==TS.pmt_pid && TS.vid_pid<0) {
        const u8 *s = pay+1+pay[0];
        if (s[0]==0x02) {
            int sl=(((s[1]&0x0f)<<8)|s[2]), pil=(((s[10]&0x0f)<<8)|s[11]);
            const u8 *es=s+12+pil, *e=s+3+sl-4;
            while (es+4<e) { if(es[0]==0x1B){TS.vid_pid=((es[1]&0x1f)<<8)|es[2];break;}
                es+=5+(((es[3]&0x0f)<<8)|es[4]); }
        }
    } else if (pid==TS.vid_pid) {
        if (pusi) pes_flush();
        if (pusi) TS.pes_valid = true;
        if (TS.pes_valid) {
            int copy = plen;
            if (TS.pes_len+copy > PES_MAX) copy = PES_MAX-TS.pes_len;
            if (copy>0) { memcpy(TS.pes+TS.pes_len, pay, copy); TS.pes_len+=copy; }
        }
    }
}

/* ── segment download + demux ──────────────────────────────── */
static void process_segment(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return;
    Buf b = {malloc(SEG_MAX/4), 0, SEG_MAX/4};
    if (!b.d) { curl_easy_cleanup(c); return; }
    b.d[0]=0;
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &b);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (b.len >= 188) {
        ts_reset();
        for (size_t i = 0; i+188 <= b.len; i += 188)
            ts_packet((const u8*)b.d+i);
        pes_flush(); /* flush last PES */
    }
    free(b.d);
}

/* ── background thread ─────────────────────────────────────── */
static void vid_thread(void *arg) {
    (void)arg;
    char variant_url[512] = {0};

    /* get HLS URL */
    LOG("vid: fetching token for %s", V.channel);
    if (!fetch_hls_url()) {
        LOG("vid: fetch_hls_url FAILED");
        LightLock_Lock(&V.lock); V.offline = true; LightLock_Unlock(&V.lock);
        return;
    }
    LOG("vid: hls_url=%.60s", V.hls_url);

    /* get master m3u8 → pick lowest quality variant */
    {
        char *master = http_get(V.hls_url, NULL);
        if (!master) {
            LOG("vid: master m3u8 download FAILED");
            LightLock_Lock(&V.lock); V.offline = true; LightLock_Unlock(&V.lock);
            return;
        }
        LOG("vid: master m3u8 ok len=%d", (int)strlen(master));
        char *var = m3u8_lowest_variant(master);
        free(master);
        if (!var) {
            LOG("vid: no variant found in m3u8");
            LightLock_Lock(&V.lock); V.offline = true; LightLock_Unlock(&V.lock);
            return;
        }
        LOG("vid: variant=%.80s", var);
        strncpy(variant_url, var, sizeof(variant_url)-1);
        free(var);
    }

    /* setup MVD config (auto-detect dimensions from SPS) */
    mvdstdGenerateDefaultConfig(&V.cfg, 0, 0, OUT_W, OUT_H,
                                 NULL, (u32*)V.outbuf, NULL);
    MVDSTD_SetConfig(&V.cfg);
    LOG("vid: MVD config set, entering stream loop");

    /* stream loop */
    while (V.active) {
        char *media = http_get(variant_url, NULL);
        if (!media) { LOG("vid: media m3u8 fetch failed"); svcSleepThread(2000000000LL); continue; }
        char *seg = m3u8_last_segment(media);
        free(media);
        if (!seg) { svcSleepThread(2000000000LL); continue; }
        if (strcmp(seg, V.last_seg) != 0) {
            LOG("vid: new seg %.60s", seg);
            strncpy(V.last_seg, seg, sizeof(V.last_seg)-1);
            process_segment(seg);
        }
        free(seg);
        svcSleepThread(2000000000LL); /* 2s poll */
    }
}

/* ── public API ────────────────────────────────────────────── */

bool video_init(void) {
    Result r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264,
                          MVD_OUTPUT_RGB565, MVD_DEFAULT_WORKBUF_SIZE, NULL);
    if (R_FAILED(r)) return false;
    V.mvd_ok = true;
    V.outbuf = linearAlloc(OUT_W * OUT_H * 2);
    V.nalbuf = linearAlloc(NAL_MAX);
    V.stgbuf = linearAlloc(TEX_W * TEX_H * 2);
    if (!V.outbuf || !V.nalbuf || !V.stgbuf) { mvdstdExit(); return false; }
    memset(V.outbuf, 0, OUT_W * OUT_H * 2);
    memset(V.stgbuf, 0, TEX_W * TEX_H * 2);
    LightLock_Init(&V.lock);
    C3D_TexInit(&V.tex, TEX_W, TEX_H, GPU_RGB565);
    C3D_TexSetFilter(&V.tex, GPU_LINEAR, GPU_LINEAR);
    V.subtex = (Tex3DS_SubTexture){ OUT_W, OUT_H,
                0.0f, (float)OUT_H/TEX_H, (float)OUT_W/TEX_W, 0.0f };
    V.img = (C2D_Image){ &V.tex, &V.subtex };
    return true;
}

void video_exit(void) {
    video_stop();
    if (V.mvd_ok) { mvdstdExit(); V.mvd_ok = false; }
    if (V.outbuf) { linearFree(V.outbuf); V.outbuf = NULL; }
    if (V.nalbuf) { linearFree(V.nalbuf); V.nalbuf = NULL; }
    if (V.stgbuf) { linearFree(V.stgbuf); V.stgbuf = NULL; }
    C3D_TexDelete(&V.tex);
}

void video_start(const char *channel, const char *oauth_pass, const char *client_id) {
    video_stop();
    strncpy(V.channel,   channel,   sizeof(V.channel)-1);
    strncpy(V.oauth,     oauth_pass,sizeof(V.oauth)-1);
    strncpy(V.client_id, client_id, sizeof(V.client_id)-1);
    V.hls_url[0] = 0; V.last_seg[0] = 0;
    V.has_frame = false; V.offline = false; V.active = true;
    V.thread = threadCreate(vid_thread, NULL, 128*1024, 0x25, -1, false);
    LOG("video_start: ch=%s thread=%p", V.channel, (void*)V.thread);
}

void video_stop(void) {
    if (!V.active) return;
    V.active = false;
    if (V.thread) { threadJoin(V.thread, U64_MAX); threadFree(V.thread); V.thread = NULL; }
}

void video_draw_top(float x, float y) {
    LightLock_Lock(&V.lock);
    bool ready = V.has_frame;
    if (ready) V.has_frame = false;
    LightLock_Unlock(&V.lock);

    if (ready && V.stgbuf) {
        /* outbuf already rendered by thread — copy into staging and upload */
        for (int row = 0; row < OUT_H; row++)
            memcpy(V.stgbuf + row*TEX_W*2, V.outbuf + row*OUT_W*2, OUT_W*2);
        GSPGPU_FlushDataCache(V.stgbuf, TEX_W * TEX_H * 2);
        C3D_SyncDisplayTransfer(
            (u32*)V.stgbuf,   GX_BUFFER_DIM(TEX_W, TEX_H),
            (u32*)V.tex.data, GX_BUFFER_DIM(TEX_W, TEX_H),
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
            GX_TRANSFER_FLIP_VERT(1) |
            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    }
    C2D_DrawImageAt(V.img, x, y, 0.5f, NULL, 1.0f, 1.0f);
}

bool video_is_offline(void) { LightLock_Lock(&V.lock); bool r=V.offline; LightLock_Unlock(&V.lock); return r; }
bool video_is_active(void)  { return V.active; }
