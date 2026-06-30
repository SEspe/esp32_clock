#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"

#define WIFI_SSID           "EinebuNest"
#define WIFI_PASS           "LunRibbe"
#define WIFI_CONNECTED_BIT  BIT0

#define SDA_PIN     5
#define SCL_PIN     4
#define OLED_ADDR   0x3C
#define OLED_W      128
#define PAGES       8

#define LED_GPIO           14
#define ACK_BTN_GPIO       0        /* BOOT button — press to dismiss/ack alarm */
#define ALARM_AUTO_STOP_S  60
#define SNOOZE_DURATION_S  (5 * 60)

static const char *TAG = "clock";
static EventGroupHandle_t s_wifi_event_group;
static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t oled_handle;
static esp_ip4_addr_t s_ip_addr = {0};

/* Alarm state — written from main task and HTTP task; bool/uint8 writes are atomic on Xtensa */
static volatile uint8_t  alarm_hour    = 7;
static volatile uint8_t  alarm_min     = 0;
static volatile uint8_t  alarm_enabled = 0;
static volatile bool     alarm_active  = false;
static volatile uint32_t alarm_since_s = 0;
static volatile time_t   snooze_until  = 0;

/* 5×7 font, column-encoded (LSB = top pixel). Index: '0'-'9'=0-9, ':'=10, ' '=11, '-'=12, '.'=13, 'v'=14 */
static const uint8_t font[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x08,0x08,0x08,0x00}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x03,0x04,0x08,0x04,0x03}, /* v */
};

static uint8_t fb[PAGES][OLED_W];

static void oled_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    i2c_master_transmit(oled_handle, buf, 2, 100);
}

static void oled_flush(void) {
    for (int p = 0; p < PAGES; p++) {
        oled_cmd(0xB0 | p); /* set page address */
        oled_cmd(0x00);      /* set lower column = 0 */
        oled_cmd(0x10);      /* set higher column = 0 */
        uint8_t buf[OLED_W + 1];
        buf[0] = 0x40;
        memcpy(buf + 1, fb[p], OLED_W);
        i2c_master_transmit(oled_handle, buf, sizeof(buf), 100);
    }
}

static void oled_clear(void) { memset(fb, 0, sizeof(fb)); }

static void draw_char(int x, int start_page, char c, int scale) {
    int idx = (c >= '0' && c <= '9') ? (c - '0') : (c == ':') ? 10 : (c == '-') ? 12 : (c == '.') ? 13 : (c == 'v') ? 14 : 11;
    for (int col = 0; col < 5; col++) {
        uint8_t col_data = font[idx][col];
        for (int sx = 0; sx < scale; sx++) {
            int px = x + col * scale + sx;
            if (px >= OLED_W) continue;
            for (int bit = 0; bit < 7; bit++) {
                if (!(col_data & (1 << bit))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = bit * scale + sy;
                    int pg = start_page + py / 8;
                    if (pg < PAGES)
                        fb[pg][px] |= (1 << (py % 8));
                }
            }
        }
    }
}

static void draw_string(int x, int page, const char *s, int scale) {
    while (*s) { draw_char(x, page, *s++, scale); x += (5 + 1) * scale; }
}

static void oled_init(void) {
    static const uint8_t cmds[] = {
        0xAE,             /* display off          */
        0xD5, 0x80,       /* clock divider        */
        0xA8, 0x3F,       /* mux ratio 64px       */
        0xD3, 0x00,       /* display offset 0     */
        0x40,             /* start line 0         */
        0x8D, 0x14,       /* charge pump on       */
        0x20, 0x02,       /* page addressing mode  */
        0xA1,             /* segment remap        */
        0xC8,             /* COM scan direction   */
        0xDA, 0x12,       /* COM pins config      */
        0x81, 0xCF,       /* contrast             */
        0xD9, 0xF1,       /* pre-charge period    */
        0xDB, 0x40,       /* VCOMH deselect       */
        0xA4,             /* pixels from RAM      */
        0xA6,             /* normal (non-inverted)*/
        0xAF,             /* display on           */
    };
    for (size_t i = 0; i < sizeof(cmds); i++) oled_cmd(cmds[i]);
}

static void oled_test_pattern(void) {
    for (int p = 0; p < PAGES; p++)
        memset(fb[p], (p % 2) ? 0xFF : 0x00, OLED_W);
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(1000));
    oled_clear();
    oled_flush();
}

/* ── Alarm NVS ────────────────────────────────────────────────────────── */

static void alarm_load_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "alm_h",  &v) == ESP_OK) alarm_hour    = v;
    if (nvs_get_u8(h, "alm_m",  &v) == ESP_OK) alarm_min     = v;
    if (nvs_get_u8(h, "alm_en", &v) == ESP_OK) alarm_enabled = v;
    nvs_close(h);
    ESP_LOGI(TAG, "Alarm loaded: %02d:%02d enabled=%d", alarm_hour, alarm_min, alarm_enabled);
}

static void alarm_save_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "alm_h",  alarm_hour);
    nvs_set_u8(h, "alm_m",  alarm_min);
    nvs_set_u8(h, "alm_en", alarm_enabled);
    nvs_commit(h);
    nvs_close(h);
}

/* ── LED task (GPIO14, 1s period when alarm active) ───────────────────── */

static void led_task(void *arg) {
    bool state = false;
    int  btn_low = 0;
    uint32_t led_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        led_ms += 50;

        /* Physical ack button: two consecutive 50 ms polls low = acknowledge */
        if (gpio_get_level(ACK_BTN_GPIO) == 0) {
            if (++btn_low >= 2 && alarm_active) {
                alarm_active = false;
                snooze_until = 0;
                gpio_set_level(LED_GPIO, 0);
                state  = false;
                led_ms = 0;
                btn_low = 0;
            }
        } else {
            btn_low = 0;
        }

        /* LED: toggle every 500 ms while alarm active */
        if (alarm_active) {
            if (led_ms >= 500) {
                state = !state;
                gpio_set_level(LED_GPIO, state);
                led_ms = 0;
            }
        } else {
            if (state) { gpio_set_level(LED_GPIO, 0); state = false; }
            led_ms = 0;
        }
    }
}

/* ── WiFi ─────────────────────────────────────────────────────────────── */

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        esp_wifi_connect();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_ip_addr = event->ip_info.ip;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL, NULL));

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid,     WIFI_SSID, sizeof(wcfg.sta.ssid));
    strncpy((char *)wcfg.sta.password, WIFI_PASS,  sizeof(wcfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, false, true, pdMS_TO_TICKS(60000));
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi connected");
}

/* ── NTP ──────────────────────────────────────────────────────────────── */

static void time_init(void) {
    /* Norway: CET (UTC+1) / CEST (UTC+2) with DST rules */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    struct tm t = {0};
    int tries = 0;
    while (t.tm_year < (2024 - 1900) && tries++ < 60) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time_t now = time(NULL);
        localtime_r(&now, &t);
    }
    ESP_LOGI(TAG, "Time synced");
}

/* ── Web server ───────────────────────────────────────────────────────── */

/* Minimal ESP image header structs for size calculation */
typedef struct {
    uint8_t  magic;
    uint8_t  segment_count;
    uint8_t  spi_mode;
    uint8_t  spi_speed_size;
    uint32_t entry_addr;
    uint8_t  wp_pin;
    uint8_t  spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t  min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t  reserved[4];
    uint8_t  hash_appended;
} __attribute__((packed)) img_hdr_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} __attribute__((packed)) seg_hdr_t;

static esp_err_t image_size(const esp_partition_t *part, size_t *out) {
    img_hdr_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK || hdr.magic != 0xE9)
        return ESP_FAIL;
    size_t off = sizeof(hdr);
    for (int i = 0; i < hdr.segment_count; i++) {
        seg_hdr_t seg;
        if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) return ESP_FAIL;
        off += sizeof(seg) + seg.data_len;
    }
    off += 1; /* checksum */
    if (hdr.hash_appended) off += 32; /* SHA256 */
    *out = off;
    return ESP_OK;
}

static void slot_version(const esp_partition_t *part, char *out, size_t len) {
    esp_app_desc_t d;
    if (part && esp_ota_get_partition_description(part, &d) == ESP_OK)
        snprintf(out, len, "%s", d.version);
    else
        snprintf(out, len, "(empty)");
}

static esp_err_t root_handler(httpd_req_t *req) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    const esp_app_desc_t *desc = esp_app_get_description();
    char date[36], tim[12];
    if (t.tm_year >= (2020 - 1900)) {
        snprintf(date, sizeof(date), "%02d.%02d.%04d",
                 t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
        snprintf(tim, sizeof(tim), "%02d:%02d:%02d",
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        snprintf(date, sizeof(date), "--.--.----");
        snprintf(tim,  sizeof(tim),  "--:--:--");
    }

    uint32_t uptime_s = esp_timer_get_time() / 1000000;
    uint32_t up_h = uptime_s / 3600, up_m = (uptime_s % 3600) / 60, up_s = uptime_s % 60;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *p0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *p1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    char v0[32], v1[32];
    slot_version(p0, v0, sizeof(v0));
    slot_version(p1, v1, sizeof(v1));
    const char *r0 = (running == p0) ? " &lt;-- running" : "";
    const char *r1 = (running == p1) ? " &lt;-- running" : "";
    const char *b0 = (running == p0) ? "disabled" : "";
    const char *b1 = (running == p1) ? "disabled" : "";

    const char *slot_label = running ? running->label : "?";
    uint32_t free_heap = esp_get_free_heap_size();

    /* snapshot volatile alarm state once */
    uint8_t a_h    = alarm_hour;
    uint8_t a_m    = alarm_min;
    uint8_t a_en   = alarm_enabled;
    bool    a_act  = alarm_active;
    time_t  s_until = snooze_until;

    char alarm_status[48];
    if (a_act) {
        snprintf(alarm_status, sizeof(alarm_status), "ACTIVE");
    } else if (s_until != 0) {
        struct tm st;
        localtime_r(&s_until, &st);
        snprintf(alarm_status, sizeof(alarm_status), "Snoozed &mdash; rings at %02d:%02d",
                 st.tm_hour, st.tm_min);
    } else {
        snprintf(alarm_status, sizeof(alarm_status), "%s", a_en ? "Armed" : "Off");
    }

    static char body[4096];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<style>body{font-family:monospace;padding:20px}</style>"
        "</head><body>"
        "<pre>Firmware: %s\nSlot:     %s\nDate:     %s\nTime:     %s\nUptime:   %02"PRIu32":%02"PRIu32":%02"PRIu32"\nFree heap: %"PRIu32" B</pre>"
        "<hr><h3>Installed slots</h3>"
        "<p>ota_0: %s%s <a href=\"/download?slot=0\">[Download]</a> <button %s onclick=\"boot(0)\">Boot this</button></p>"
        "<p>ota_1: %s%s <a href=\"/download?slot=1\">[Download]</a> <button %s onclick=\"boot(1)\">Boot this</button></p>"
        "<hr><h3>Upload new firmware</h3>"
        "<input type=\"file\" id=\"fw\" style=\"display:none\">"
        "<button onclick=\"pickFile()\">Choose File</button>"
        "<span id=\"fn\" style=\"margin-left:8px\">No file selected</span><br><br>"
        "<button id=\"upbtn\" onclick=\"upload()\" disabled>Upload &amp; Reboot</button>"
        "<div id=\"st\"></div>"
        "<hr><h3>Alarm</h3>"
        "<p>Status: <b>%s</b></p>"
        "<input type=\"time\" id=\"at\" value=\"%02d:%02d\">"
        " <label><input type=\"checkbox\" id=\"aen\" %s> Enabled</label>"
        " <button onclick=\"setAlarm()\">Set</button>"
        "<br><br>"
        "<button id=\"snz\" onclick=\"snoozeAlarm()\" %s>Snooze 5 min</button>"
        " <button id=\"disc\" onclick=\"dismissAlarm()\" %s>Dismiss</button>"
        "<div id=\"ast\"></div>"
        "<script>"
        "var tmr=setInterval(function(){location.reload()},1000);"
        "function pickFile(){clearInterval(tmr);document.getElementById('fw').click();}"
        "document.getElementById('fw').onchange=function(){"
        "if(this.files[0]){"
        "document.getElementById('fn').textContent=this.files[0].name;"
        "document.getElementById('upbtn').disabled=false;}}"
        ";"
        "async function boot(s){"
        "clearInterval(tmr);"
        "document.getElementById('st').textContent='Switching slot...';"
        "const r=await fetch('/boot?slot='+s,{method:'POST'});"
        "document.getElementById('st').textContent=await r.text();}"
        "async function upload(){"
        "const f=document.getElementById('fw').files[0];"
        "if(!f){document.getElementById('st').textContent='Select a .bin file first';return;}"
        "document.getElementById('st').textContent='Uploading '+f.name+'...';"
        "try{"
        "const r=await fetch('/update',{method:'POST',body:f,"
        "headers:{'Content-Type':'application/octet-stream'}});"
        "document.getElementById('st').textContent=await r.text();"
        "}catch(e){document.getElementById('st').textContent='Error: '+e;}}"
        "async function setAlarm(){"
        "clearInterval(tmr);"
        "const tv=document.getElementById('at').value;"
        "const en=document.getElementById('aen').checked?1:0;"
        "const[h,m]=tv.split(':');"
        "const r=await fetch('/alarm',{method:'POST',"
        "body:'hour='+h+'&min='+m+'&enabled='+en,"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'}});"
        "document.getElementById('ast').textContent=await r.text();"
        "setTimeout(()=>location.reload(),800);}"
        "async function snoozeAlarm(){"
        "clearInterval(tmr);"
        "const r=await fetch('/snooze',{method:'POST'});"
        "document.getElementById('ast').textContent=await r.text();"
        "setTimeout(()=>location.reload(),500);}"
        "async function dismissAlarm(){"
        "clearInterval(tmr);"
        "const r=await fetch('/dismiss',{method:'POST'});"
        "document.getElementById('ast').textContent=await r.text();"
        "setTimeout(()=>location.reload(),500);}"
        "</script></body></html>",
        desc->version, slot_label, date, tim, up_h, up_m, up_s, free_heap,
        v0, r0, b0, v1, r1, b1,
        alarm_status,
        a_h, a_m,
        a_en ? "checked" : "",
        a_act ? "" : "disabled",
        a_act ? "" : "disabled");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t alarm_set_handler(httpd_req_t *req) {
    char buf[64] = {0};
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }
    buf[n] = '\0';

    char val[8];
    if (httpd_query_key_value(buf, "hour", val, sizeof(val)) == ESP_OK) {
        int h = atoi(val);
        if (h >= 0 && h <= 23) alarm_hour = (uint8_t)h;
    }
    if (httpd_query_key_value(buf, "min", val, sizeof(val)) == ESP_OK) {
        int m = atoi(val);
        if (m >= 0 && m <= 59) alarm_min = (uint8_t)m;
    }
    alarm_enabled = (httpd_query_key_value(buf, "enabled", val, sizeof(val)) == ESP_OK &&
                     atoi(val) != 0) ? 1 : 0;
    alarm_save_nvs();
    ESP_LOGI(TAG, "Alarm set: %02d:%02d enabled=%d", alarm_hour, alarm_min, alarm_enabled);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Alarm updated");
    return ESP_OK;
}

static esp_err_t dismiss_handler(httpd_req_t *req) {
    alarm_active = false;
    snooze_until = 0;
    gpio_set_level(LED_GPIO, 0);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Alarm dismissed");
    return ESP_OK;
}

static esp_err_t snooze_handler(httpd_req_t *req) {
    alarm_active = false;
    snooze_until = time(NULL) + SNOOZE_DURATION_S;
    gpio_set_level(LED_GPIO, 0);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Snoozed for 5 minutes");
    return ESP_OK;
}

static esp_err_t boot_handler(httpd_req_t *req) {
    char query[16] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    int slot = -1;
    char val[4];
    if (httpd_query_key_value(query, "slot", val, sizeof(val)) == ESP_OK)
        slot = atoi(val);

    const esp_partition_t *target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        slot == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1,
        NULL);

    if (!target || esp_ota_set_boot_partition(target) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Boot switch failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "Switching — rebooting in 1 second.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

#define OTA_BUF_SIZE 1024

static esp_err_t update_handler(httpd_req_t *req) {
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: writing to partition %s", update_part->label);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[OTA_BUF_SIZE];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
            return ESP_FAIL;
        }
        remaining -= n;
        ESP_LOGI(TAG, "OTA: %d / %d bytes", req->content_len - remaining, req->content_len);
    }

    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(update_part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA finalise failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA complete — rebooting");
    httpd_resp_sendstr(req, "Update successful — rebooting in 1 second.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t download_handler(httpd_req_t *req) {
    char query[16] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char val[4];
    int slot = 0;
    if (httpd_query_key_value(query, "slot", val, sizeof(val)) == ESP_OK)
        slot = atoi(val);

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        slot == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1,
        NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No partition");
        return ESP_FAIL;
    }

    size_t img_sz = 0;
    if (image_size(part, &img_sz) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid image");
        return ESP_FAIL;
    }

    esp_app_desc_t app_desc;
    char fname[64];
    if (esp_ota_get_partition_description(part, &app_desc) == ESP_OK)
        snprintf(fname, sizeof(fname), "esp32_clock_%s.bin", app_desc.version);
    else
        snprintf(fname, sizeof(fname), "esp32_clock_ota%d.bin", slot);

    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char buf[1024];
    size_t off = 0;
    while (off < img_sz) {
        size_t n = img_sz - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        esp_partition_read(part, off, buf, n);
        httpd_resp_send_chunk(req, buf, n);
        off += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void webserver_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.max_uri_handlers  = 9;

    httpd_handle_t server;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }

    httpd_uri_t root     = { .uri = "/",        .method = HTTP_GET,  .handler = root_handler };
    httpd_uri_t update   = { .uri = "/update",   .method = HTTP_POST, .handler = update_handler };
    httpd_uri_t boot_uri = { .uri = "/boot",     .method = HTTP_POST, .handler = boot_handler };
    httpd_uri_t dl_uri   = { .uri = "/download", .method = HTTP_GET,  .handler = download_handler };
    httpd_uri_t alarm_uri= { .uri = "/alarm",    .method = HTTP_POST, .handler = alarm_set_handler };
    httpd_uri_t dismiss  = { .uri = "/dismiss",  .method = HTTP_POST, .handler = dismiss_handler };
    httpd_uri_t snooze   = { .uri = "/snooze",   .method = HTTP_POST, .handler = snooze_handler };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &update);
    httpd_register_uri_handler(server, &boot_uri);
    httpd_register_uri_handler(server, &dl_uri);
    httpd_register_uri_handler(server, &alarm_uri);
    httpd_register_uri_handler(server, &dismiss);
    httpd_register_uri_handler(server, &snooze);
    ESP_LOGI(TAG, "Web server started on http://" IPSTR, IP2STR(&s_ip_addr));
}

/* ── Main ─────────────────────────────────────────────────────────────── */

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    alarm_load_nvs();

    /* LED output on GPIO14 */
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_GPIO, 0);

    /* Ack button input on GPIO0 (BOOT button), active-low with internal pull-up */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << ACK_BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    xTaskCreate(led_task, "led", 1024, NULL, 5, NULL);

    /* I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = SDA_PIN,
        .scl_io_num        = SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    /* SSD1306 device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OLED_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &oled_handle));

    oled_init();
    oled_clear();
    oled_flush();

    wifi_init();
    time_init();
    webserver_init();

    time_t last_alarm_trigger = 0;

    while (1) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);

        /* Alarm trigger: fire at HH:MM:00, at most once per minute */
        if (alarm_enabled && !alarm_active && t.tm_year >= (2020 - 1900) &&
            t.tm_hour == alarm_hour && t.tm_min == alarm_min && t.tm_sec < 5 &&
            (now - last_alarm_trigger) > 60) {
            alarm_active = true;
            alarm_since_s = (uint32_t)(now);
            last_alarm_trigger = now;
            ESP_LOGI(TAG, "Alarm triggered");
        }

        /* Snooze re-trigger */
        if (!alarm_active && snooze_until != 0 && now >= snooze_until) {
            alarm_active  = true;
            alarm_since_s = (uint32_t)(now);
            snooze_until  = 0;
            last_alarm_trigger = now;
            ESP_LOGI(TAG, "Alarm re-triggered after snooze");
        }

        /* Auto-stop after ALARM_AUTO_STOP_S seconds */
        if (alarm_active && (uint32_t)(now) - alarm_since_s >= ALARM_AUTO_STOP_S) {
            alarm_active = false;
            ESP_LOGI(TAG, "Alarm auto-stopped");
        }

        oled_clear();

        /* Top line: IP address + firmware version (scale 1, page 0) */
        char ip_buf[48];
        const char *ver = esp_app_get_description()->version;
        if (s_ip_addr.addr != 0)
            snprintf(ip_buf, sizeof(ip_buf), "%d.%d.%d.%d %s", IP2STR(&s_ip_addr), ver);
        else
            snprintf(ip_buf, sizeof(ip_buf), "0.0.0.0 %s", ver);
        draw_string(0, 0, ip_buf, 1);

        /* Middle: date (scale 1, page 1) */
        char date_buf[36];
        if (t.tm_year >= (2020 - 1900))
            snprintf(date_buf, sizeof(date_buf), "%02d.%02d.%04d",
                     t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
        else
            snprintf(date_buf, sizeof(date_buf), "--.--.----");
        draw_string(34, 1, date_buf, 1);

        /* Bottom: time (scale 2, pages 3-6) */
        if (t.tm_year < (2020 - 1900)) {
            draw_string(17, 3, "--:--:--", 2);
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
            draw_string(17, 3, buf, 2);
        }
        oled_flush();

        /* Invert display when alarm is active, alternating each second */
        oled_cmd(alarm_active && (t.tm_sec % 2 == 0) ? 0xA7 : 0xA6);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
