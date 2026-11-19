#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <curl/curl.h>

#include "XPLMPlugin.h"
#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#define REFRESH_SEC 300.0f
#define INJECT_SEC    2.0f
#define POLL_SEC     15.0f
#define NUM_LAYERS     10
#define HRRR_LAYERS     9  // HRRR tops at 100 hPa, skip 70 hPa layer

// Layers 1-9: Open-Meteo pressure levels (HRRR or ICON)
// Layer 0:    METAR surface observation (overrides model surface)
static const float LAYER_ALT_M[NUM_LAYERS] = {
      0.0f,    // surface  — METAR actual ground obs
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

// Aloft vars start at 925 hPa (index 1); surface handled by METAR
static const char *SPD_VARS[NUM_LAYERS - 1] = {
    "windspeed_925hPa","windspeed_850hPa","windspeed_700hPa",
    "windspeed_500hPa","windspeed_300hPa","windspeed_200hPa",
    "windspeed_150hPa","windspeed_100hPa","windspeed_70hPa"
};
static const char *DIR_VARS[NUM_LAYERS - 1] = {
    "winddirection_925hPa","winddirection_850hPa","winddirection_700hPa",
    "winddirection_500hPa","winddirection_300hPa","winddirection_200hPa",
    "winddirection_150hPa","winddirection_100hPa","winddirection_70hPa"
};

static XPLMDataRef gLat, gLon, gWindDir, gWindSpd, gWindAlt;

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static float g_dirs[NUM_LAYERS], g_spds[NUM_LAYERS];
static int   g_aloft_ready = 0, g_aloft_fetching = 0;
static int   g_metar_ready = 0, g_metar_fetching = 0;
static int   g_has_aloft = 0,   g_has_metar = 0;
static int   g_nlayers = NUM_LAYERS;
static float g_model_sfc_dir = -1, g_model_sfc_spd = -1; // 10m AGL from model

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
        time_t t = (time_t)atol(p);
        const char *comma = strchr(p, ',');
        if (!comma || *(comma + 1) == ']') break;
        time_t tnext = (time_t)atol(comma + 1);
        if (now >= t && now < tnext) return idx;
        idx++;
        p = comma + 1;
    }
    return idx;
}

static float parse_field(const char *json, const char *key) {
    char k[32]; snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(json, k);
    if (!p) return -1;
    p += strlen(k);
    while (*p == ' ') p++;
    if (*p == '"' || *p == 'n' || *p == 'V') return -1;
    return (float)atof(p);
}

static double deg2rad(double d) { return d * 3.14159265358979 / 180.0; }

// Haversine distance in nm between two lat/lon points
static double dist_nm(double lat1, double lon1, double lat2, double lon2) {
    double dlat = deg2rad(lat2 - lat1), dlon = deg2rad(lon2 - lon1);
    double a = sin(dlat/2)*sin(dlat/2) + cos(deg2rad(lat1))*cos(deg2rad(lat2))*sin(dlon/2)*sin(dlon/2);
    return 2.0 * atan2(sqrt(a), sqrt(1-a)) * 3440.065; // Earth radius in nm
}

// Walk the JSON array of METAR objects, return wdir/wspd from the nearest station
static int nearest_metar(const char *json, double ac_lat, double ac_lon,
                         float *out_dir, float *out_spd) {
    double best_dist = 1e9;
    *out_dir = -1; *out_spd = -1;
    const char *p = json;
    while ((p = strstr(p, "\"lat\":")) != NULL) {
        double slat = atof(p + 6);
        const char *q = strstr(p, "\"lon\":");
        if (!q) break;
        double slon = atof(q + 6);
        double d = dist_nm(ac_lat, ac_lon, slat, slon);
        if (d < best_dist) {
            // Scan forward within this object for wdir/wspd
            const char *obj_end = strchr(p, '}');
            if (!obj_end) obj_end = p + 2048;
            char tmp[2048]; int len = (int)(obj_end - p + 1);
            if (len > (int)sizeof(tmp) - 1) len = sizeof(tmp) - 1;
            strncpy(tmp, p, len); tmp[len] = 0;
            float dir = -1, spd = -1;
            const char *wd = strstr(tmp, "\"wdir\":");
            const char *ws = strstr(tmp, "\"wspd\":");
            if (wd) { wd += 7; while(*wd==' ')wd++; if(*wd!='"'&&*wd!='n'&&*wd!='V') dir=(float)atof(wd); }
            if (ws) { ws += 7; while(*ws==' ')ws++; if(*ws!='"'&&*ws!='n') spd=(float)atof(ws); }
            if (dir >= 0 && spd >= 0) { best_dist = d; *out_dir = dir; *out_spd = spd; }
        }
        p++;
    }
    return (*out_dir >= 0 && *out_spd >= 0) ? 1 : 0;
}

static int is_conus(double lat, double lon) {
    return lat >= 24.0 && lat <= 50.0 && lon >= -125.0 && lon <= -65.0;
}

typedef struct { double lat, lon; const char *model; int nlayers; } AloftArgs;
typedef struct { double lat, lon; } MetarArgs; // lat/lon = aircraft position

// --- METAR thread: fetches surface obs, writes layer 0 ---
static void *fetch_metar_thread(void *arg) {
    MetarArgs *ma = (MetarArgs *)arg;
    double ac_lat = ma->lat, ac_lon = ma->lon;
    char url[256];
    snprintf(url, sizeof(url),
        "https://aviationweather.gov/api/data/metar"
        "?bbox=%.2f,%.2f,%.2f,%.2f&format=json",
        ac_lat - 0.5, ac_lon - 0.5,
        ac_lat + 0.5, ac_lon + 0.5);
    free(ma);

    CURL *c = curl_easy_init();
    if (!c) { pthread_mutex_lock(&gMutex); g_metar_fetching = 0; pthread_mutex_unlock(&gMutex); return NULL; }

    struct Buf b = { (char *)malloc(1), 0 };
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (rc == CURLE_OK && b.data) {
        float dir, spd;
        nearest_metar(b.data, ac_lat, ac_lon, &dir, &spd);
        if (dir >= 0 && spd >= 0) {
            pthread_mutex_lock(&gMutex);
            g_dirs[0] = dir; g_spds[0] = spd;
            g_metar_ready = 1;
            pthread_mutex_unlock(&gMutex);
        }
    }
    free(b.data);
    pthread_mutex_lock(&gMutex); g_metar_fetching = 0; pthread_mutex_unlock(&gMutex);
    return NULL;
}

// --- Aloft thread: fetches HRRR/ICON, writes layers 1-N ---
static void *fetch_aloft_thread(void *arg) {
    AloftArgs *fa = (AloftArgs *)arg;
    char url[640];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&hourly=windspeed_10m,winddirection_10m"
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
        fa->lat, fa->lon, fa->model);
    int nlayers = fa->nlayers;
    free(fa);

    CURL *c = curl_easy_init();
    if (!c) { pthread_mutex_lock(&gMutex); g_aloft_fetching = 0; pthread_mutex_unlock(&gMutex); return NULL; }

    struct Buf b = { (char *)malloc(1), 0 };
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (rc == CURLE_OK && b.data) {
        int hi = find_hour_index(b.data);
        float dirs[NUM_LAYERS - 1], spds[NUM_LAYERS - 1];
        int ok = 1;
        for (int i = 0; i < nlayers - 1; i++) {
            dirs[i] = parse_array_at(b.data, DIR_VARS[i], hi);
            spds[i] = parse_array_at(b.data, SPD_VARS[i], hi);
            if (dirs[i] < 0 || spds[i] < 0) { ok = 0; break; }
        }
        float msfc_dir = parse_array_at(b.data, "winddirection_10m", hi);
        float msfc_spd = parse_array_at(b.data, "windspeed_10m",    hi);
        if (ok) {
            pthread_mutex_lock(&gMutex);
            for (int i = 0; i < nlayers - 1; i++) {
                g_dirs[i + 1] = dirs[i];
                g_spds[i + 1] = spds[i];
            }
            if (msfc_dir >= 0 && msfc_spd >= 0) {
                g_model_sfc_dir = msfc_dir;
                g_model_sfc_spd = msfc_spd;
            }
            g_nlayers = nlayers;
            g_aloft_ready = 1;
            pthread_mutex_unlock(&gMutex);
        }
    }
    free(b.data);
    pthread_mutex_lock(&gMutex); g_aloft_fetching = 0; pthread_mutex_unlock(&gMutex);
    return NULL;
}

static void spawn_fetches(void) {
    double lat = XPLMGetDatad(gLat);
    double lon = XPLMGetDatad(gLon);

    // METAR surface fetch
    if (!g_metar_fetching) {
        MetarArgs *ma = (MetarArgs *)malloc(sizeof(MetarArgs));
        ma->lat = lat; ma->lon = lon;
        pthread_t t;
        if (pthread_create(&t, NULL, fetch_metar_thread, ma) == 0) { pthread_detach(t); g_metar_fetching = 1; }
        else free(ma);
    }

    // Aloft fetch
    if (!g_aloft_fetching) {
        AloftArgs *fa = (AloftArgs *)malloc(sizeof(AloftArgs));
        fa->lat     = lat; fa->lon = lon;
        fa->model   = is_conus(lat, lon) ? "gfs_hrrr"   : "icon_seamless";
        fa->nlayers = is_conus(lat, lon) ? HRRR_LAYERS  : NUM_LAYERS;
        pthread_t t;
        if (pthread_create(&t, NULL, fetch_aloft_thread, fa) == 0) { pthread_detach(t); g_aloft_fetching = 1; }
        else free(fa);
    }
}

static float g_elapsed = 0.0f;

static float loop_cb(float dt, float, int, void *) {
    pthread_mutex_lock(&gMutex);
    int aloft_ready   = g_aloft_ready;
    int metar_ready   = g_metar_ready;
    int aloft_fetch   = g_aloft_fetching;
    int metar_fetch   = g_metar_fetching;
    int has_aloft     = g_has_aloft;
    int has_metar     = g_has_metar;
    int nlayers       = g_nlayers;
    float model_sfc_dir = g_model_sfc_dir;
    float model_sfc_spd = g_model_sfc_spd;
    float dirs[NUM_LAYERS], spds[NUM_LAYERS];
    for (int i = 0; i < NUM_LAYERS; i++) { dirs[i] = g_dirs[i]; spds[i] = g_spds[i]; }
    if (aloft_ready) { g_aloft_ready = 0; g_has_aloft = 1; has_aloft = 1; }
    if (metar_ready) { g_metar_ready = 0; g_has_metar = 1; has_metar = 1; }
    pthread_mutex_unlock(&gMutex);

    if (aloft_ready || metar_ready) {
        char msg[128];
        float sfc_d = has_metar ? dirs[0] : model_sfc_dir;
        float sfc_s = has_metar ? spds[0] : model_sfc_spd;
        snprintf(msg, sizeof(msg),
            "RealWind: sfc %.0f/%.0fkt(%s)  10k %.0f/%.0fkt  30k %.0f/%.0fkt\n",
            sfc_d, sfc_s, has_metar ? "METAR" : "model-10m",
            dirs[3], spds[3], dirs[5], spds[5]);
        XPLMDebugString(msg);
    }

    // Inject aloft layers first, then METAR overwrites layer 0
    if (has_aloft) {
        for (int i = 1; i < nlayers; i++) {
            XPLMSetDatavf(gWindDir, &dirs[i], i, 1);
            XPLMSetDatavf(gWindSpd, &spds[i], i, 1);
            float alt = LAYER_ALT_M[i];
            XPLMSetDatavf(gWindAlt, &alt, i, 1);
        }
    }
    if (has_metar) {
        XPLMSetDatavf(gWindDir, &dirs[0], 0, 1);
        XPLMSetDatavf(gWindSpd, &spds[0], 0, 1);
        float alt = 0.0f;
        XPLMSetDatavf(gWindAlt, &alt, 0, 1);
    } else if (has_aloft && model_sfc_dir >= 0) {
        // No METAR (remote/oceanic) — use model 10m AGL wind
        XPLMSetDatavf(gWindDir, &model_sfc_dir, 0, 1);
        XPLMSetDatavf(gWindSpd, &model_sfc_spd, 0, 1);
        float alt = 0.0f;
        XPLMSetDatavf(gWindAlt, &alt, 0, 1);
    }

    g_elapsed += dt;
    int any_fetching = aloft_fetch || metar_fetch;
    int has_any = has_aloft || has_metar;
    if (!any_fetching && (!has_any || g_elapsed >= REFRESH_SEC)) {
        pthread_mutex_lock(&gMutex); spawn_fetches(); pthread_mutex_unlock(&gMutex);
        g_elapsed = 0.0f;
    }

    return has_any ? INJECT_SEC : POLL_SEC;
}

PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
    strcpy(name, "RealWind");
    strcpy(sig,  "com.nikhil.realwind");
    strcpy(desc, "Injects real-world winds into X-Plane 11");

    gLat     = XPLMFindDataRef("sim/flightmodel/position/latitude");
    gLon     = XPLMFindDataRef("sim/flightmodel/position/longitude");
    gWindDir = XPLMFindDataRef("sim/weather/wind_direction_degt");
    gWindSpd = XPLMFindDataRef("sim/weather/wind_speed_kt");
    gWindAlt = XPLMFindDataRef("sim/weather/wind_altitude_msl_m");

    if (!gLat || !gLon || !gWindDir || !gWindSpd || !gWindAlt) {
        XPLMDebugString("RealWind: failed to find required datarefs\n");
        return 0;
    }

    for (int i = 0; i < NUM_LAYERS; i++) { g_dirs[i] = -1; g_spds[i] = -1; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    XPLMRegisterFlightLoopCallback(loop_cb, 1.0f, NULL);
    XPLMDebugString("RealWind: loaded — METAR surface + HRRR/ICON aloft\n");
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    XPLMUnregisterFlightLoopCallback(loop_cb, NULL);
    curl_global_cleanup();
}

PLUGIN_API int  XPluginEnable(void)                           { return 1; }
PLUGIN_API void XPluginDisable(void)                          {}
PLUGIN_API void XPLMReceiveMessage(XPLMPluginID, int, void *) {}
