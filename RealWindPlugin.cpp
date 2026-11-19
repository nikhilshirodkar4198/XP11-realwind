#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <curl/curl.h>

#include "XPLMPlugin.h"
#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#define REFRESH_SEC 300.0f
#define POLL_SEC     15.0f

static XPLMDataRef gLat, gLon, gWindDir, gWindSpd, gWindAlt;

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static float  g_dir = -1, g_spd = -1;
static int    g_ready = 0, g_fetching = 0;

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

// Extract first numeric value for "key": from JSON (handles null/string as -1)
static float parse_num(const char *json, const char *key) {
    char k[32]; snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(json, k);
    if (!p) return -1;
    p += strlen(k);
    while (*p == ' ') p++;
    if (*p == '"' || *p == 'n' || *p == 'V') return -1; // "VRB", null
    return (float)atof(p);
}

typedef struct { double lat, lon; } FetchArgs;

static void *fetch_thread(void *arg) {
    FetchArgs *fa = (FetchArgs *)arg;
    char url[256];
    snprintf(url, sizeof(url),
        "https://aviationweather.gov/api/data/metar"
        "?bbox=%.2f,%.2f,%.2f,%.2f&format=json",
        fa->lat - 0.5, fa->lon - 0.5,
        fa->lat + 0.5, fa->lon + 0.5);
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
        float dir = parse_num(b.data, "wdir");
        float spd = parse_num(b.data, "wspd");
        if (dir >= 0 && spd >= 0) {
            pthread_mutex_lock(&gMutex);
            g_dir = dir; g_spd = spd; g_ready = 1;
            pthread_mutex_unlock(&gMutex);
        }
    }
    free(b.data);
    pthread_mutex_lock(&gMutex); g_fetching = 0; pthread_mutex_unlock(&gMutex);
    return NULL;
}

static void spawn_fetch(void) {
    FetchArgs *fa = (FetchArgs *)malloc(sizeof(FetchArgs));
    fa->lat = XPLMGetDatad(gLat);
    fa->lon = XPLMGetDatad(gLon);
    pthread_t t;
    if (pthread_create(&t, NULL, fetch_thread, fa) == 0) {
        pthread_detach(t);
        g_fetching = 1;
    } else {
        free(fa);
    }
}

static float loop_cb(float, float, int, void *) {
    pthread_mutex_lock(&gMutex);
    int ready    = g_ready;
    int fetching = g_fetching;
    float dir = g_dir, spd = g_spd;
    if (ready) g_ready = 0;
    pthread_mutex_unlock(&gMutex);

    if (ready) {
        XPLMSetDatavf(gWindDir, &dir, 0, 1);
        XPLMSetDatavf(gWindSpd, &spd, 0, 1);
        float alt = 0.0f;
        XPLMSetDatavf(gWindAlt, &alt, 0, 1);
        char msg[64];
        snprintf(msg, sizeof(msg), "RealWind: %.0f deg @ %.0f kts\n", dir, spd);
        XPLMDebugString(msg);

        pthread_mutex_lock(&gMutex); spawn_fetch(); pthread_mutex_unlock(&gMutex);
        return REFRESH_SEC;
    }

    if (!fetching) {
        pthread_mutex_lock(&gMutex); spawn_fetch(); pthread_mutex_unlock(&gMutex);
    }
    return POLL_SEC;
}

PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
    strcpy(name, "RealWind");
    strcpy(sig,  "com.nikhil.realwind");
    strcpy(desc, "Injects METAR wind into X-Plane 11");

    gLat     = XPLMFindDataRef("sim/flightmodel/position/latitude");
    gLon     = XPLMFindDataRef("sim/flightmodel/position/longitude");
    gWindDir = XPLMFindDataRef("sim/weather/wind_direction_degt");
    gWindSpd = XPLMFindDataRef("sim/weather/wind_speed_kt");
    gWindAlt = XPLMFindDataRef("sim/weather/wind_altitude_msl_m");

    if (!gLat || !gLon || !gWindDir || !gWindSpd || !gWindAlt) {
        XPLMDebugString("RealWind: failed to find required datarefs\n");
        return 0;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    XPLMRegisterFlightLoopCallback(loop_cb, 1.0f, NULL);
    XPLMDebugString("RealWind: loaded\n");
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    XPLMUnregisterFlightLoopCallback(loop_cb, NULL);
    curl_global_cleanup();
}

PLUGIN_API int  XPluginEnable(void)                           { return 1; }
PLUGIN_API void XPluginDisable(void)                          {}
PLUGIN_API void XPLMReceiveMessage(XPLMPluginID, int, void *) {}
