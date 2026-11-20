#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <winhttp.h>
#  include <process.h>
static void msleep(int ms) { Sleep(ms); }
#else
#  include <pthread.h>
#  include <curl/curl.h>
static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

#include "XPLMPlugin.h"
#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#define REFRESH_SEC     300.0f
#define INJECT_SEC        2.0f
#define POLL_SEC         15.0f
#define NUM_LAYERS         12   // fetched levels; 3 of them are injected at a time
#define HRRR_LAYERS        11   // HRRR tops at 100 hPa, skip 70 hPa
#define METAR_MAX_AGE    3600   // reject METARs older than 60 min

// Layer 0 = surface (METAR/model-10m); layers 1-11 = pressure levels
static const float LAYER_ALT_M[NUM_LAYERS] = {
      0.0f,    // surface  — METAR / model-10m
    244.0f,    //  975 hPa ~   800 ft
    500.0f,    //  950 hPa ~ 1,600 ft
    762.0f,    //  925 hPa ~ 2,500 ft
   1524.0f,    //  850 hPa ~ 5,000 ft
   3048.0f,    //  700 hPa ~10,000 ft
   5486.0f,    //  500 hPa ~18,000 ft
   9144.0f,    //  300 hPa ~30,000 ft
  11887.0f,    //  200 hPa ~39,000 ft
  13608.0f,    //  150 hPa ~44,600 ft
  16180.0f,    //  100 hPa ~53,000 ft
  18300.0f     //   70 hPa ~60,000 ft
};

static const char *SPD_VARS[NUM_LAYERS - 1] = {
    "windspeed_975hPa","windspeed_950hPa","windspeed_925hPa",
    "windspeed_850hPa","windspeed_700hPa","windspeed_500hPa",
    "windspeed_300hPa","windspeed_200hPa","windspeed_150hPa",
    "windspeed_100hPa","windspeed_70hPa"
};
static const char *DIR_VARS[NUM_LAYERS - 1] = {
    "winddirection_975hPa","winddirection_950hPa","winddirection_925hPa",
    "winddirection_850hPa","winddirection_700hPa","winddirection_500hPa",
    "winddirection_300hPa","winddirection_200hPa","winddirection_150hPa",
    "winddirection_100hPa","winddirection_70hPa"
};

#define METAR_AGL_LIMIT_M 1524.0f  // ~5,000 ft AGL — above this use model-10m for surface

// ---------- X-Plane datarefs ----------
#define XP_WIND_LAYERS 3   // XP11: separate sim/weather/wind_*[0..2] datarefs

static XPLMDataRef gLat, gLon, gHeading, gGroundspeed, gHeightAGL, gElevMSL;
static XPLMDataRef gWindAltL[XP_WIND_LAYERS], gWindDirL[XP_WIND_LAYERS], gWindSpdL[XP_WIND_LAYERS];
static XPLMDataRef gTurb0;

// Feedback datarefs (readable by FlyWithLua / DataRef editor)
static XPLMDataRef gDRSurfaceSource = NULL; // 0=none 1=METAR 2=model-10m
static XPLMDataRef gDRMetarAge      = NULL; // seconds since METAR obsTime
static XPLMDataRef gDRAloftAge      = NULL; // seconds since last aloft fetch

// ---------- Shared state ----------
#ifdef _WIN32
static SRWLOCK gMutex = SRWLOCK_INIT;
static void lock(void)   { AcquireSRWLockExclusive(&gMutex); }
static void unlock(void) { ReleaseSRWLockExclusive(&gMutex); }
#else
static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static void lock(void)   { pthread_mutex_lock(&gMutex); }
static void unlock(void) { pthread_mutex_unlock(&gMutex); }
#endif

static float  g_dirs[NUM_LAYERS], g_spds[NUM_LAYERS];
static int    g_nlayers         = NUM_LAYERS;
static float  g_model_sfc_dir   = -1,   g_model_sfc_spd  = -1;
static float  g_metar_gust      = -1;
static time_t g_metar_obs_time  = 0;
static time_t g_aloft_fetch_time = 0;

static int    g_aloft_ready     = 0,    g_aloft_fetching = 0;
static int    g_metar_ready     = 0,    g_metar_fetching = 0;
static int    g_has_aloft       = 0,    g_has_metar      = 0;
static int    g_surface_source  = 0;    // 0/1/2
static int    g_shutdown        = 0;
static int    g_active_threads  = 0;

// ---------- HTTP buffer ----------
struct Buf { char *data; size_t size; };

static size_t write_cb(void *ptr, size_t sz, size_t n, void *ud) {
    struct Buf *b = (struct Buf *)ud;
    size_t total = sz * n;
    b->data = (char *)realloc(b->data, b->size + total + 1);
    if (!b->data) return 0;
    memcpy(b->data + b->size, ptr, total);
    b->size += total;
    b->data[b->size] = 0;
    return total;
}

// ---------- HTTP GET: WinHTTP on Windows, libcurl elsewhere. Returns 1 on success ----------
#ifdef _WIN32
static int http_get(const char *url, struct Buf *b) {
    wchar_t wurl[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024)) return 0;

    URL_COMPONENTS uc; memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 1024;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;
    // Query string comes back separately; WinHTTP wants it as part of the object path
    if (uc.lpszExtraInfo) wcsncat_s(path, 1024, uc.lpszExtraInfo, _TRUNCATE);

    int ok = 0;
    HINTERNET s = WinHttpOpen(L"RealWind/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, 2500, 2500, 2500, 2500);  // per phase; keeps a fetch inside XPluginStop's 12s wait
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER,
                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                          uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
          && WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, sl = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
        ok = (status == 200);
        char chunk[8192]; DWORD got;
        while (ok && WinHttpReadData(r, chunk, sizeof(chunk), &got) && got > 0)
            if (write_cb(chunk, 1, got, b) != got) ok = 0;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
#else
static int http_get(const char *url, struct Buf *b) {
    CURL *c = curl_easy_init();
    if (!c) return 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return rc == CURLE_OK;
}
#endif

// ---------- Detached thread start. Returns 1 on success ----------
typedef void *(*thread_fn)(void *);
#ifdef _WIN32
typedef struct { thread_fn fn; void *arg; } ThreadStart;
static unsigned __stdcall thread_trampoline(void *p) {
    ThreadStart s = *(ThreadStart *)p; free(p);
    s.fn(s.arg);
    return 0;
}
static int start_thread(thread_fn fn, void *arg) {
    ThreadStart *s = (ThreadStart *)malloc(sizeof(ThreadStart));
    if (!s) return 0;
    s->fn = fn; s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, thread_trampoline, s, 0, NULL);
    if (!h) { free(s); return 0; }
    CloseHandle((HANDLE)h);
    return 1;
}
#else
static int start_thread(thread_fn fn, void *arg) {
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return 0;
    pthread_detach(t);
    return 1;
}
#endif

// ---------- JSON helpers ----------
static float parse_array_at(const char *json, const char *key, int idx) {
    char k[64]; snprintf(k, sizeof(k), "\"%s\":[", key);
    const char *p = strstr(json, k);
    if (!p) return -1;
    p += strlen(k);
    for (int i = 0; i < idx; i++) {
        p = strchr(p, ',');
        if (!p) return -1;
        p++;
    }
    while (*p == ' ') p++;
    return (float)atof(p);
}

static int find_hour_index(const char *json) {
    const char *p = strstr(json, "\"time\":[");
    if (!p) return 0;
    p += 8;
    time_t now = time(NULL);
    int idx = 0;
    while (*p && *p != ']') {
        time_t t = (time_t)strtoll(p, NULL, 10);
        const char *c = strchr(p, ',');
        if (!c || *(c+1) == ']') break;
        if (now >= t && now < (time_t)strtoll(c+1, NULL, 10)) return idx;
        idx++; p = c + 1;
    }
    return idx;
}

// ---------- Geometry ----------
static double deg2rad(double d) { return d * 3.14159265358979 / 180.0; }

static double dist_nm(double la1, double lo1, double la2, double lo2) {
    double dlat = deg2rad(la2-la1), dlon = deg2rad(lo2-lo1);
    double a = sin(dlat/2)*sin(dlat/2) +
               cos(deg2rad(la1))*cos(deg2rad(la2))*sin(dlon/2)*sin(dlon/2);
    return 2.0 * atan2(sqrt(a), sqrt(1.0-a)) * 3440.065;
}

// Circular blend: t=0→d1, t=1→d2
static float blend_dir(float d1, float d2, float t) {
    float diff = d2 - d1;
    if (diff >  180.0f) diff -= 360.0f;
    if (diff < -180.0f) diff += 360.0f;
    float r = d1 + t * diff;
    if (r <    0.0f) r += 360.0f;
    if (r >= 360.0f) r -= 360.0f;
    return r;
}

static int is_conus(double lat, double lon) {
    return lat >= 24.0 && lat <= 50.0 && lon >= -125.0 && lon <= -65.0;
}

// ---------- METAR parser: nearest station by Haversine ----------
static void nearest_metar(const char *json, double ac_lat, double ac_lon,
                           float *dir, float *spd, float *gust, time_t *obs) {
    *dir = *spd = *gust = -1; *obs = 0;
    double best = 1e9;
    // One station record runs from its "icaoId" to the next one; wind/obsTime come before lat/lon
    const char *p = strstr(json, "\"icaoId\":");
    while (p) {
        const char *next = strstr(p + 1, "\"icaoId\":");
        int len = next ? (int)(next - p) : (int)strlen(p);
        if (len > 4095) len = 4095;
        char tmp[4096]; memcpy(tmp, p, len); tmp[len] = 0;
        p = next;

        const char *la = strstr(tmp, "\"lat\":"), *lo = strstr(tmp, "\"lon\":");
        if (!la || !lo) continue;
        double d = dist_nm(ac_lat, ac_lon, atof(la + 6), atof(lo + 6));
        if (d < best) {
            const char *f;
            float td=-1, ts=-1, tg=-1; time_t to=0;
            if ((f=strstr(tmp,"\"wdir\":"))  && f[7]!='"'&&f[7]!='n'&&f[7]!='V') td=(float)atof(f+7);
            if ((f=strstr(tmp,"\"wspd\":"))  && f[7]!='"'&&f[7]!='n')             ts=(float)atof(f+7);
            if ((f=strstr(tmp,"\"wgst\":"))  && f[7]!='"'&&f[7]!='n')             tg=(float)atof(f+7);
            if ((f=strstr(tmp,"\"obsTime\":")))                                    to=(time_t)strtoll(f+10, NULL, 10);
            if (td >= 0 && ts >= 0) { best=d; *dir=td; *spd=ts; *gust=tg; *obs=to; }
        }
    }
}

// ---------- Thread lifecycle helpers ----------
static void thread_enter(void) {
    lock(); g_active_threads++; unlock();
}
static void thread_exit(void) {
    lock(); g_active_threads--; unlock();
}

// ---------- Arg structs ----------
typedef struct { double lat, lon; } MetarArgs;
typedef struct { double lat, lon; const char *model; int nlayers; } AloftArgs;

// ---------- METAR fetch thread ----------
static void *fetch_metar_thread(void *arg) {
    MetarArgs *ma = (MetarArgs *)arg;
    double ac_lat = ma->lat, ac_lon = ma->lon;
    free(ma);
    thread_enter();

    lock(); int shut = g_shutdown; unlock();
    if (shut) { lock(); g_metar_fetching=0; unlock(); thread_exit(); return NULL; }

    char url[256];
    snprintf(url, sizeof(url),
        "https://aviationweather.gov/api/data/metar"
        "?bbox=%.2f,%.2f,%.2f,%.2f&format=json",
        ac_lat-0.5, ac_lon-0.5, ac_lat+0.5, ac_lon+0.5);

    struct Buf b = { (char *)malloc(1), 0 };
    int ok = http_get(url, &b);

    if (ok && b.data) {
        float dir, spd, gust; time_t obs;
        nearest_metar(b.data, ac_lat, ac_lon, &dir, &spd, &gust, &obs);
        // Discard stale METAR
        if (dir >= 0 && obs > 0 && (time(NULL) - obs) > METAR_MAX_AGE)
            dir = spd = gust = -1;
        if (dir >= 0 && spd >= 0) {
            lock();
            g_dirs[0]=dir; g_spds[0]=spd;
            g_metar_gust=gust; g_metar_obs_time=obs;
            g_metar_ready=1;
            unlock();
        }
    }
    free(b.data);
    lock(); g_metar_fetching=0; unlock();
    thread_exit();
    return NULL;
}

// ---------- Aloft fetch thread ----------
static void *fetch_aloft_thread(void *arg) {
    AloftArgs *fa = (AloftArgs *)arg;
    double lat=fa->lat, lon=fa->lon; int nlayers=fa->nlayers; const char *model=fa->model;
    free(fa);
    thread_enter();

    lock(); int shut=g_shutdown; unlock();
    if (shut) { lock(); g_aloft_fetching=0; unlock(); thread_exit(); return NULL; }

    char url[768];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&hourly=windspeed_10m,winddirection_10m"
        ",windspeed_975hPa,winddirection_975hPa"
        ",windspeed_950hPa,winddirection_950hPa"
        ",windspeed_925hPa,winddirection_925hPa"
        ",windspeed_850hPa,winddirection_850hPa"
        ",windspeed_700hPa,winddirection_700hPa"
        ",windspeed_500hPa,winddirection_500hPa"
        ",windspeed_300hPa,winddirection_300hPa"
        ",windspeed_200hPa,winddirection_200hPa"
        ",windspeed_150hPa,winddirection_150hPa"
        ",windspeed_100hPa,winddirection_100hPa"
        ",windspeed_70hPa,winddirection_70hPa"
        "&wind_speed_unit=kn&forecast_days=1&timeformat=unixtime&models=%s",
        lat, lon, model);

    struct Buf b = { (char *)malloc(1), 0 };
    int http_ok = http_get(url, &b);

    if (http_ok && b.data) {
        int hi = find_hour_index(b.data);
        float msfc_dir = parse_array_at(b.data, "winddirection_10m", hi);
        float msfc_spd = parse_array_at(b.data, "windspeed_10m",     hi);
        float dirs[NUM_LAYERS-1], spds[NUM_LAYERS-1];
        int ok = 1;
        for (int i = 0; i < nlayers-1; i++) {
            dirs[i] = parse_array_at(b.data, DIR_VARS[i], hi);
            spds[i] = parse_array_at(b.data, SPD_VARS[i], hi);
            if (dirs[i] < 0 || spds[i] < 0) { ok=0; break; }
        }
        if (ok) {
            lock();
            for (int i = 0; i < nlayers-1; i++) { g_dirs[i+1]=dirs[i]; g_spds[i+1]=spds[i]; }
            if (msfc_dir >= 0) { g_model_sfc_dir=msfc_dir; g_model_sfc_spd=msfc_spd; }
            g_nlayers=nlayers; g_aloft_ready=1; g_aloft_fetch_time=time(NULL);
            unlock();
        }
    }
    free(b.data);
    lock(); g_aloft_fetching=0; unlock();
    thread_exit();
    return NULL;
}

// ---------- Spawn both fetches ----------
static void spawn_fetches(void) {
    double ac_lat = XPLMGetDatad(gLat);
    double ac_lon = XPLMGetDatad(gLon);

    // Aloft: project forward by half refresh interval so we anticipate wind ahead
    float gs_ms   = XPLMGetDataf(gGroundspeed);
    float hdg_deg = XPLMGetDataf(gHeading);
    double dist_ahead = (gs_ms * 1.94384) * (REFRESH_SEC / 2.0) / 3600.0; // nm
    double hdg_rad    = deg2rad(hdg_deg);
    double aloft_lat  = ac_lat + (dist_ahead / 60.0) * cos(hdg_rad);
    double aloft_lon  = ac_lon + (dist_ahead / (60.0 * cos(deg2rad(ac_lat)))) * sin(hdg_rad);

    float agl_m = XPLMGetDataf(gHeightAGL);
    if (!g_metar_fetching && agl_m < METAR_AGL_LIMIT_M) {
        MetarArgs *ma = (MetarArgs *)malloc(sizeof(MetarArgs));
        ma->lat=ac_lat; ma->lon=ac_lon;
        if (start_thread(fetch_metar_thread, ma)) g_metar_fetching=1;
        else free(ma);
    }

    if (!g_aloft_fetching) {
        AloftArgs *fa = (AloftArgs *)malloc(sizeof(AloftArgs));
        fa->lat=aloft_lat; fa->lon=aloft_lon;
        fa->model   = is_conus(aloft_lat,aloft_lon) ? "gfs_hrrr"  : "icon_seamless";
        fa->nlayers = is_conus(aloft_lat,aloft_lon) ? HRRR_LAYERS : NUM_LAYERS;
        if (start_thread(fetch_aloft_thread, fa)) g_aloft_fetching=1;
        else free(fa);
    }
}

// ---------- Flight loop ----------
static float g_elapsed = 0.0f;

static float loop_cb(float dt, float, int, void *) {
    lock();
    int aloft_ready=g_aloft_ready, metar_ready=g_metar_ready;
    int aloft_fetch=g_aloft_fetching, metar_fetch=g_metar_fetching;
    int has_aloft=g_has_aloft, has_metar=g_has_metar;
    int nlayers=g_nlayers;
    float msfc_dir=g_model_sfc_dir, msfc_spd=g_model_sfc_spd, mgust=g_metar_gust;
    float dirs[NUM_LAYERS], spds[NUM_LAYERS];
    for (int i=0;i<NUM_LAYERS;i++) { dirs[i]=g_dirs[i]; spds[i]=g_spds[i]; }
    if (aloft_ready) { g_aloft_ready=0; g_has_aloft=1; has_aloft=1; }
    if (metar_ready) { g_metar_ready=0; g_has_metar=1; has_metar=1; }
    unlock();

    // Determine surface wind source — METAR only used when low enough to matter
    float agl_m = XPLMGetDataf(gHeightAGL);
    float sfc_dir, sfc_spd; int src;
    if (has_metar && agl_m < METAR_AGL_LIMIT_M)
                           { sfc_dir=dirs[0];  sfc_spd=spds[0];  src=1; }
    else if (msfc_dir>=0)  { sfc_dir=msfc_dir; sfc_spd=msfc_spd; src=2; }
    else                   { sfc_dir=-1;        sfc_spd=-1;        src=0; }

    lock(); g_surface_source=src; unlock();

    if (aloft_ready || metar_ready) {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "RealWind: sfc %.0f/%.0fkt(%s)  10k %.0f/%.0fkt  30k %.0f/%.0fkt\n",
            sfc_dir, sfc_spd, src==1?"METAR":src==2?"model":"none",
            dirs[5], spds[5], dirs[7], spds[7]);
        XPLMDebugString(msg);
    }

    // XP11 has 3 wind layers: 0 = surface, 1/2 = the fetched levels bracketing the aircraft.
    // X-Plane interpolates between layers, so wind at the aircraft is always real data, and
    // re-picking the bracket on a level crossing is seamless (that level's wind is shared).
    double alt_msl = XPLMGetDatad(gElevMSL);
    float  gnd_msl = (float)(alt_msl - agl_m);

    if (has_aloft) {
        // Blend layers 1-2 toward surface to smooth METAR→model discontinuity
        if (sfc_dir >= 0) {
            dirs[1]=blend_dir(dirs[1],sfc_dir,0.33f); spds[1]=spds[1]*0.67f+sfc_spd*0.33f;
            dirs[2]=blend_dir(dirs[2],sfc_dir,0.17f); spds[2]=spds[2]*0.83f+sfc_spd*0.17f;
        }
        // Lowest level above the ground: high-elevation airports sit above 975/950 hPa
        int first = 1;
        while (first < nlayers-2 && LAYER_ALT_M[first] <= gnd_msl + 30.0f) first++;
        int lo = first;
        while (lo < nlayers-2 && LAYER_ALT_M[lo+1] <= alt_msl) lo++;
        int pick[2] = { lo, lo+1 };
        for (int k=0; k<2; k++) {
            int i = pick[k];
            float alt = LAYER_ALT_M[i];
            XPLMSetDataf(gWindAltL[k+1], alt);
            XPLMSetDataf(gWindDirL[k+1], dirs[i]);
            XPLMSetDataf(gWindSpdL[k+1], spds[i]);
        }
    }

    if (sfc_dir >= 0) {
        XPLMSetDataf(gWindAltL[0], gnd_msl > 0.0f ? gnd_msl : 0.0f);
        XPLMSetDataf(gWindDirL[0], sfc_dir);
        XPLMSetDataf(gWindSpdL[0], sfc_spd);

        // Inject gust as surface-layer turbulence (METAR only), 0-10 scale
        if (gTurb0) {
            float turb=0.0f;
            if (src==1 && mgust > sfc_spd) {
                turb = (mgust - sfc_spd) / 15.0f * 10.0f;
                if (turb > 10.0f) turb = 10.0f;
            }
            XPLMSetDataf(gTurb0, turb);
        }
    }

    g_elapsed += dt;
    if (!(aloft_fetch||metar_fetch) && (!( has_aloft||has_metar) || g_elapsed>=REFRESH_SEC)) {
        lock(); spawn_fetches(); unlock();
        g_elapsed=0.0f;
    }
    return (has_aloft||has_metar) ? INJECT_SEC : POLL_SEC;
}

// ---------- Feedback dataref callbacks ----------
static float cb_surface_source(void *r) {
    lock(); float v=(float)g_surface_source; unlock(); return v;
}
static float cb_metar_age(void *r) {
    lock(); time_t t=g_metar_obs_time; unlock();
    return t ? (float)(time(NULL)-t) : -1.0f;
}
static float cb_aloft_age(void *r) {
    lock(); time_t t=g_aloft_fetch_time; unlock();
    return t ? (float)(time(NULL)-t) : -1.0f;
}

// ---------- Plugin lifecycle ----------
PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
    strcpy(name, "RealWind");
    strcpy(sig,  "com.nikhil.realwind");
    strcpy(desc, "Injects real-world wind into X-Plane 11");

    gLat         = XPLMFindDataRef("sim/flightmodel/position/latitude");
    gLon         = XPLMFindDataRef("sim/flightmodel/position/longitude");
    gHeading     = XPLMFindDataRef("sim/flightmodel/position/psi");
    gGroundspeed = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
    gHeightAGL   = XPLMFindDataRef("sim/flightmodel/position/y_agl");
    gElevMSL     = XPLMFindDataRef("sim/flightmodel/position/elevation");
    gTurb0       = XPLMFindDataRef("sim/weather/turbulence[0]");

    int ok = gLat && gLon && gHeading && gGroundspeed && gHeightAGL && gElevMSL;
    for (int k=0; k<XP_WIND_LAYERS; k++) {
        char n[64];
        snprintf(n, sizeof(n), "sim/weather/wind_altitude_msl_m[%d]", k); gWindAltL[k] = XPLMFindDataRef(n);
        snprintf(n, sizeof(n), "sim/weather/wind_direction_degt[%d]", k); gWindDirL[k] = XPLMFindDataRef(n);
        snprintf(n, sizeof(n), "sim/weather/wind_speed_kt[%d]",       k); gWindSpdL[k] = XPLMFindDataRef(n);
        ok = ok && gWindAltL[k] && gWindDirL[k] && gWindSpdL[k];
    }

    if (!ok) {
        XPLMDebugString("RealWind: missing required datarefs\n"); return 0;
    }

    for (int i=0;i<NUM_LAYERS;i++) { g_dirs[i]=-1; g_spds[i]=-1; }
#ifndef _WIN32
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    XPLMRegisterFlightLoopCallback(loop_cb, 1.0f, NULL);

    gDRSurfaceSource = XPLMRegisterDataAccessor("realwind/surface_source",
        xplmType_Float,0, NULL,NULL,cb_surface_source,NULL,NULL,NULL,
        NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
    gDRMetarAge = XPLMRegisterDataAccessor("realwind/metar_age_sec",
        xplmType_Float,0, NULL,NULL,cb_metar_age,NULL,NULL,NULL,
        NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
    gDRAloftAge = XPLMRegisterDataAccessor("realwind/aloft_age_sec",
        xplmType_Float,0, NULL,NULL,cb_aloft_age,NULL,NULL,NULL,
        NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);

    XPLMDebugString("RealWind: loaded\n");
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    XPLMUnregisterFlightLoopCallback(loop_cb, NULL);

    // Signal threads to exit, wait up to 12s (curl timeout is 10s)
    lock(); g_shutdown=1; unlock();
    for (int i=0; i<120; i++) {
        lock(); int active=g_active_threads; unlock();
        if (active==0) break;
        msleep(100);
    }

#ifndef _WIN32
    curl_global_cleanup();
#endif

    if (gDRSurfaceSource) XPLMUnregisterDataAccessor(gDRSurfaceSource);
    if (gDRMetarAge)      XPLMUnregisterDataAccessor(gDRMetarAge);
    if (gDRAloftAge)      XPLMUnregisterDataAccessor(gDRAloftAge);
}

PLUGIN_API int  XPluginEnable(void)                           { return 1; }
PLUGIN_API void XPluginDisable(void)                          {}
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void *) {}
