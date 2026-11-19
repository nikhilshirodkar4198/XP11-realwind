#include <string.h>
#include <stdlib.h>
#include <stdio.h>
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

// Pressure level → approximate MSL altitude in metres
static const float LAYER_ALT_M[NUM_LAYERS] = {
    111.0f,    // 1000 hPa  ~   364 ft
    762.0f,    //  925 hPa  ~ 2,500 ft
    1524.0f,   //  850 hPa  ~ 5,000 ft
    3048.0f,   //  700 hPa  ~10,000 ft
    5486.0f,   //  500 hPa  ~18,000 ft
    9144.0f,   //  300 hPa  ~30,000 ft
    11887.0f,  //  200 hPa  ~39,000 ft
    13608.0f,  //  150 hPa  ~44,600 ft
    16180.0f,  //  100 hPa  ~53,000 ft
    18300.0f   //   70 hPa  ~60,000 ft
};

static const char *SPD_VARS[NUM_LAYERS] = {
    "windspeed_1000hPa","windspeed_925hPa","windspeed_850hPa",
    "windspeed_700hPa","windspeed_500hPa","windspeed_300hPa",
    "windspeed_200hPa","windspeed_150hPa","windspeed_100hPa","windspeed_70hPa"
};
static const char *DIR_VARS[NUM_LAYERS] = {
    "winddirection_1000hPa","winddirection_925hPa","winddirection_850hPa",
    "winddirection_700hPa","winddirection_500hPa","winddirection_300hPa",
    "winddirection_200hPa","winddirection_150hPa","winddirection_100hPa","winddirection_70hPa"
};

static XPLMDataRef gLat, gLon, gWindDir, gWindSpd, gWindAlt;

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static float g_dirs[NUM_LAYERS], g_spds[NUM_LAYERS];
static int   g_ready = 0, g_fetching = 0, g_has_data = 0, g_nlayers = NUM_LAYERS;

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

// Return value at index idx inside a JSON array named key
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

// Find hourly index whose unix timestamp bracket contains now
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

// HRRR covers CONUS and tops out at 100 hPa (~53k ft) — skip 70 hPa layer for it
#define HRRR_LAYERS 9

typedef struct { double lat, lon; const char *model; int nlayers; } FetchArgs;

static int is_conus(double lat, double lon) {
    return lat >= 24.0 && lat <= 50.0 && lon >= -125.0 && lon <= -65.0;
}

static void *fetch_thread(void *arg) {
    FetchArgs *fa = (FetchArgs *)arg;
    char url[640];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&hourly=windspeed_1000hPa,winddirection_1000hPa"
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
    if (!c) { pthread_mutex_lock(&gMutex); g_fetching = 0; pthread_mutex_unlock(&gMutex); return NULL; }

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
        float dirs[NUM_LAYERS], spds[NUM_LAYERS];
        int ok = 1;
        for (int i = 0; i < nlayers; i++) {
            dirs[i] = parse_array_at(b.data, DIR_VARS[i], hi);
            spds[i] = parse_array_at(b.data, SPD_VARS[i], hi);
            if (dirs[i] < 0 || spds[i] < 0) { ok = 0; break; }
        }
        if (ok) {
            pthread_mutex_lock(&gMutex);
            for (int i = 0; i < nlayers; i++) { g_dirs[i] = dirs[i]; g_spds[i] = spds[i]; }
            g_nlayers = nlayers;
            g_ready = 1;
            pthread_mutex_unlock(&gMutex);
        }
    }
    free(b.data);
    pthread_mutex_lock(&gMutex); g_fetching = 0; pthread_mutex_unlock(&gMutex);
    return NULL;
}

static void spawn_fetch(void) {
    FetchArgs *fa = (FetchArgs *)malloc(sizeof(FetchArgs));
    fa->lat  = XPLMGetDatad(gLat);
    fa->lon  = XPLMGetDatad(gLon);
    fa->model   = is_conus(fa->lat, fa->lon) ? "gfs_hrrr"      : "icon_seamless";
    fa->nlayers = is_conus(fa->lat, fa->lon) ? HRRR_LAYERS : NUM_LAYERS;
    pthread_t t;
    if (pthread_create(&t, NULL, fetch_thread, fa) == 0) { pthread_detach(t); g_fetching = 1; }
    else free(fa);
}

static float g_elapsed = 0.0f;

static float loop_cb(float dt, float, int, void *) {
    pthread_mutex_lock(&gMutex);
    int ready    = g_ready;
    int fetching = g_fetching;
    int has_data = g_has_data;
    int nlayers  = g_nlayers;
    float dirs[NUM_LAYERS], spds[NUM_LAYERS];
    for (int i = 0; i < NUM_LAYERS; i++) { dirs[i] = g_dirs[i]; spds[i] = g_spds[i]; }
    if (ready) { g_ready = 0; g_has_data = 1; has_data = 1; }
    pthread_mutex_unlock(&gMutex);

    if (ready) {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "RealWind: sfc %.0f/%.0fkt  10k %.0f/%.0fkt  30k %.0f/%.0fkt  60k %.0f/%.0fkt\n",
            dirs[0], spds[0], dirs[3], spds[3], dirs[5], spds[5], dirs[9], spds[9]);
        XPLMDebugString(msg);
    }

    if (has_data) {
        for (int i = 0; i < nlayers; i++) {
            XPLMSetDatavf(gWindDir, &dirs[i], i, 1);
            XPLMSetDatavf(gWindSpd, &spds[i], i, 1);
            float alt = LAYER_ALT_M[i];
            XPLMSetDatavf(gWindAlt, &alt, i, 1);
        }
    }

    g_elapsed += dt;
    if (!fetching && (!has_data || g_elapsed >= REFRESH_SEC)) {
        pthread_mutex_lock(&gMutex); spawn_fetch(); pthread_mutex_unlock(&gMutex);
        g_elapsed = 0.0f;
    }

    return has_data ? INJECT_SEC : POLL_SEC;
}

PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
    strcpy(name, "RealWind");
    strcpy(sig,  "com.nikhil.realwind");
    strcpy(desc, "Injects real-world winds aloft into X-Plane 11");

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
    XPLMDebugString("RealWind: loaded — HRRR (1h) over CONUS, ICON (3h) global\n");
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    XPLMUnregisterFlightLoopCallback(loop_cb, NULL);
    curl_global_cleanup();
}

PLUGIN_API int  XPluginEnable(void)                           { return 1; }
PLUGIN_API void XPluginDisable(void)                          {}
PLUGIN_API void XPLMReceiveMessage(XPLMPluginID, int, void *) {}
