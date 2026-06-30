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
#include "driver/i2c_master.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#define WIFI_SSID           "EinebuNest"
#define WIFI_PASS           "LunRibbe"
#define WIFI_CONNECTED_BIT  BIT0

#define SDA_PIN     5
#define SCL_PIN     4
#define OLED_ADDR   0x3C
#define OLED_W      128
#define PAGES       8

static const char *TAG = "clock";
static EventGroupHandle_t s_wifi_event_group;
static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t oled_handle;
static esp_ip4_addr_t s_ip_addr = {0};

/* 5×7 font, column-encoded (LSB = top pixel). Index: '0'-'9'=0-9, ':'=10, ' '=11 */
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
    int idx = (c >= '0' && c <= '9') ? (c - '0') : (c == ':') ? 10 : (c == '-') ? 12 : (c == '.') ? 13 : 11;
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

    char body[1024];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv=\"refresh\" content=\"1\">"
        "<style>body{font-family:monospace;padding:20px}</style>"
        "</head><body>"
        "<pre>Firmware: %s\nDate:     %s\nTime:     %s\nUptime:   %02"PRIu32":%02"PRIu32":%02"PRIu32"</pre>"
        "<hr><h3>Firmware Update (OTA)</h3>"
        "<input type=\"file\" id=\"fw\" accept=\".bin\">"
        "<button onclick=\"upload()\">Upload &amp; Reboot</button>"
        "<div id=\"st\"></div>"
        "<script>"
        "async function upload(){"
        "const f=document.getElementById('fw').files[0];"
        "if(!f){alert('Select a .bin file');return;}"
        "document.getElementById('st').textContent='Uploading...';"
        "try{"
        "const r=await fetch('/update',{method:'POST',body:f,"
        "headers:{'Content-Type':'application/octet-stream'}});"
        "document.getElementById('st').textContent=await r.text();"
        "}catch(e){document.getElementById('st').textContent='Error: '+e;}}"
        "</script></body></html>",
        desc->version, date, tim, up_h, up_m, up_s);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, body);
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

static void webserver_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    httpd_handle_t server;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
    };
    httpd_uri_t update = {
        .uri = "/update", .method = HTTP_POST, .handler = update_handler,
    };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &update);
    ESP_LOGI(TAG, "Web server started on http://" IPSTR, IP2STR(&s_ip_addr));
}

/* ── Main ─────────────────────────────────────────────────────────────── */

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

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

    while (1) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);

        oled_clear();

        /* Top line: IP address (scale 1, page 0) */
        char ip_buf[20];
        if (s_ip_addr.addr != 0)
            snprintf(ip_buf, sizeof(ip_buf), "%d.%d.%d.%d", IP2STR(&s_ip_addr));
        else
            snprintf(ip_buf, sizeof(ip_buf), "0.0.0.0");
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

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
