/**
  * @brief display_task, received JSON, parses and drive LED circle accordingly
  *
  * CLOSED SOURCE, NOT FOR PUBLIC RELEASE
  * (c) Copyright 2020, Sander and Coert Vonk
  * All rights reserved. Use of copyright notice does not imply publication.
  * All text above must be included in any redistribution
 **/

#include <sdkconfig.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/rmt.h>
#include <led_strip.h>
#include <cJSON.h>

#include "display_task.h"
#define ARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))
#define ALIGN( type ) __attribute__((aligned( __alignof__( type ) )))
#define PACK( type )  __attribute__((aligned( __alignof__( type ) ), packed ))
#define PACK8  __attribute__((aligned( __alignof__( uint8_t ) ), packed ))
#define MIN(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a < _b ? _a : _b; })
#define MAX(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a > _b ? _a : _b; })

static char const * const TAG = "display_task";

typedef struct {
    time_t start, stop;
} PACK8 event_t;

enum MAX_EVENTS { MAX_EVENTS = 30 };
typedef event_t events_t[MAX_EVENTS];

static time_t
_str2time(char * str) {  // e.g. 2020-06-25T22:30:16.329Z
    struct tm tm;
    strptime(str, "%Y-%m-%dT%H:%M:%S", &tm);
    return mktime(&tm);
}

static void  // far from ideal, but it gets the job done
_updateEventDetail(char const * const key,
char * const value,
event_t * const event )
{
    if (strcmp(key, "start") == 0) {
        event->start = _str2time(value);
    } else if (strcmp(key, "stop") == 0) {
        event->stop = _str2time(value);
    } else {
        ESP_LOGE(TAG, "%s: unrecognized key(%s) ", __func__, key);
    }
}

static void
_setTime(time_t const time)
{
    struct timeval tv = { .tv_sec = time, .tv_usec = 0};
    settimeofday(&tv, NULL);
}

static time_t
_getTime(time_t * time_)
{
    return time(time_);
}

static uint
_parseJson(char const * const serializedJson, time_t * const time, events_t events)
{
    uint len = 0;

    cJSON * const jsonRoot = cJSON_Parse(serializedJson);
    if (jsonRoot->type != cJSON_Object) {
        ESP_LOGE(TAG, "JSON root is not an Object");
        return 0;
    }
    cJSON const *const jsonTime = cJSON_GetObjectItem(jsonRoot, "time");
    if (!jsonTime || jsonTime->type != cJSON_String) {
        ESP_LOGE(TAG, "JSON.time is missing or not an String");
        return 0;
    }
    *time = _str2time(jsonTime->valuestring);

    cJSON const *const jsonEvents = cJSON_GetObjectItem(jsonRoot, "events");
    if (!jsonEvents || jsonEvents->type != cJSON_Array) {
        ESP_LOGE(TAG, "JSON.events is missing or not an Array");
        return 0;
    }

    enum DETAIL_COUNT { DETAIL_COUNT = 2 };
    static char const * const detailNames[DETAIL_COUNT] = {"start", "stop"};

    event_t * event = events;
    for (int ii = 0; ii < cJSON_GetArraySize(jsonEvents) && ii < MAX_EVENTS; ii++, len++) {

        cJSON const *const jsonEvent = cJSON_GetArrayItem(jsonEvents, ii);
        if (!jsonEvent || jsonEvent->type != cJSON_Object) {
            ESP_LOGE(TAG, "JSON.events[%d] is missing or not an Object", ii);
            return 0;
        }
        for (uint vv = 0; vv < ARRAYSIZE(detailNames); vv++) {
            cJSON const *const jsonObj = cJSON_GetObjectItem(jsonEvent, detailNames[vv]);
            if (!jsonObj || jsonObj->type != cJSON_String) {
                ESP_LOGE(TAG, "JSON.event[%d].%s is missing or not a String", ii, detailNames[vv]);
                return 0;
            }
            _updateEventDetail(detailNames[vv], jsonObj->valuestring, event);
        }
        event++;
    }
    cJSON_Delete(jsonRoot);
    return len;
}

// algorithm from https://en.wikipedia.org/wiki/HSL_and_HSV
static void
_hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h %= 360;
    uint32_t const rgb_max = v * 2.55f;
    uint32_t const rgb_min = rgb_max * (100 - s) / 100.0f;
    uint32_t const i = h / 60;
    uint32_t const diff = h % 60;
    uint32_t const rgb_adj = (rgb_max - rgb_min) * diff / 60;  // RGB adjustment amount by hue

    switch (i) {
        case 0:
            *r = rgb_max;
            *g = rgb_min + rgb_adj;
            *b = rgb_min;
            break;
        case 1:
            *r = rgb_max - rgb_adj;
            *g = rgb_max;
            *b = rgb_min;
            break;
        case 2:
            *r = rgb_min;
            *g = rgb_max;
            *b = rgb_min + rgb_adj;
            break;
        case 3:
            *r = rgb_min;
            *g = rgb_max - rgb_adj;
            *b = rgb_max;
            break;
        case 4:
            *r = rgb_min + rgb_adj;
            *g = rgb_min;
            *b = rgb_max;
            break;
        default:
            *r = rgb_max;
            *g = rgb_min;
            *b = rgb_max - rgb_adj;
            break;
    }
}

static void
_showEventOnClock(event_t const * const event, time_t const now, uint * const hue, led_strip_t * const strip)
{
    struct tm nowTm;
    localtime_r(&now, &nowTm);

    //ESP_LOGI(TAG, "now = %04d-%02d-%02d %02d:%02d", nowTm.tm_year + 1900, nowTm.tm_mon + 1, nowTm.tm_mday, nowTm.tm_hour, nowTm.tm_min);
    float const startsInHr = difftime(event->start, now) / 3600;
    float const stopsInHr = difftime(event->stop, now) / 3600;
#define DEBUG (1)
#if DEBUG
    struct tm startTm, stopTm;
    localtime_r(&event->start, &startTm);
    localtime_r(&event->stop, &stopTm);
#endif
    uint const hrsOnClock = 12;
    bool const alreadyFinished = stopsInHr < 0;
    bool const startsWithin12h = startsInHr < hrsOnClock;

    if (startsWithin12h && !alreadyFinished) {

        float const pxlsPerHr = CONFIG_CLOCK_WS2812_COUNT / hrsOnClock;
        float const hrsFromToc = nowTm.tm_hour % hrsOnClock + (float)nowTm.tm_min / 60.0;  // hours from top-of-clock
        uint const nowPxl = round(hrsFromToc * pxlsPerHr);
        uint const startPxl = round((hrsFromToc + MAX(startsInHr, 0)) * pxlsPerHr);
        uint const stopPxl = round((hrsFromToc + MIN(stopsInHr, hrsOnClock)) * pxlsPerHr);
#if DEBUG
        ESP_LOGI(TAG, "event = %04d-%02d-%02d %02d:%02d (in %5.2fh) to %04d-%02d-%02d %02d:%02d (in %5.2fh) => pxl = %2u to %2u",
                 startTm.tm_year + 1900, startTm.tm_mon + 1, startTm.tm_mday, startTm.tm_hour, startTm.tm_min, startsInHr,
                 stopTm.tm_year + 1900, stopTm.tm_mon + 1, stopTm.tm_mday, stopTm.tm_hour, stopTm.tm_min, stopsInHr,
                 startPxl, stopPxl);
#endif
        for (uint pp = startPxl; pp < stopPxl; pp++) {
            uint const minBrightness = 1;
            uint const maxBrightness = 50;
            uint const pct = 100 - (pp - nowPxl) * 100 / CONFIG_CLOCK_WS2812_COUNT;
            uint const brightness = minBrightness + (maxBrightness - minBrightness) * pct/100;
            uint r, g, b;
            _hsv2rgb(*hue, 100, brightness, &r, &g, &b);
            ESP_ERROR_CHECK(strip->set_pixel(strip, pp % CONFIG_CLOCK_WS2812_COUNT, r, g, b));
        }
        uint const goldenAngle = 137;
        *hue = (*hue + goldenAngle) % 360;
    }
}

void
display_task(void * ipc_void)
{
    display_task_ipc_t * ipc = ipc_void;
    QueueHandle_t jsonQ = ipc->jsonQ;

    // install ws2812 driver
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(CONFIG_CLOCK_WS2812_PIN, RMT_CHANNEL_0);
    config.clk_div = 2;  // set counter clock to 40MHz
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));
    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(CONFIG_CLOCK_WS2812_COUNT, (led_strip_dev_t)config.channel);
    led_strip_t * const strip = led_strip_new_rmt_ws2812(&strip_config);
    if (!strip) { ESP_LOGE(TAG, "Can't install WS2812 driver"); return; }

    event_t * const events = (event_t *)malloc(sizeof(events_t));
    if (!events) { ESP_LOGE(TAG, "No memory for events"); return; }

    ESP_ERROR_CHECK(strip->clear(strip, 100));  // turn off all LEDs

    uint len = 0;
    time_t now;
    time_t const loopInMsec = 10000UL;  // how often the while-loop runs [msec]
    while (1) {

        // if there was an calendar update then apply it

        char * msg;
        if (xQueueReceive(jsonQ, &msg, (TickType_t)(loopInMsec / portTICK_PERIOD_MS)) == pdPASS) {
            len = _parseJson(msg, &now, events); // translate from serialized JSON "msg" to C representation "events"
            free(msg);
            ESP_LOGI(TAG, "Update");
            _setTime(now); // update out time-of-day
        } else {
            ESP_LOGI(TAG, "No update");
            _getTime(&now);
        }

        // loop through each event and set LEDs accordingly

        uint hue = 0;
        event_t const * event = events;
        for (uint ee = 0; ee < len; ee++, event++) {
            _showEventOnClock(event, now, &hue, strip);
        }
        ESP_ERROR_CHECK(strip->refresh(strip, 100));
    }
}