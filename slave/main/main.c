#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_now.h"

#define WIFI_SSID           "EinebuNest"
#define WIFI_PASS           "LunRibbe"
#define WIFI_CONNECTED_BIT  BIT0

#define LED_GPIO        15
#define OTA_BUF_SIZE    1024

static const char *TAG = "slave";
static EventGroupHandle_t s_wifi_event_group;
static esp_ip4_addr_t s_ip_addr = {0};

typedef enum { ALARM_STATE_OFF = 0, ALARM_STATE_ARMED = 1, ALARM_STATE_ACTIVE = 2 } alarm_state_t;
typedef struct { uint8_t state; } __attribute__((packed)) alarm_msg_t;

static volatile alarm_state_t slave_alarm_state = ALARM_STATE_OFF;

/* ── LED task ─────────────────────────────────────────────────────────── */

static void led_task(void *arg) {
    bool led_on = false;
    uint32_t led_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        led_ms += 50;
        if (slave_alarm_state == ALARM_STATE_ACTIVE) {
            if (led_ms >= 500) {
                led_on = !led_on;
                gpio_set_level(LED_GPIO, led_on);
                led_ms = 0;
            }
        } else {
            if (led_on) { gpio_set_level(LED_GPIO, 0); led_on = false; }
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
    ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&s_ip_addr));
}

/* ── ESP-NOW ──────────────────────────────────────────────────────────── */

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(alarm_msg_t)) return;
    alarm_msg_t msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.state <= ALARM_STATE_ACTIVE) {
        slave_alarm_state = (alarm_state_t)msg.state;
        ESP_LOGI(TAG, "Alarm state: %d", msg.state);
    }
}

static void espnow_init(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
}

/* ── Web server ───────────────────────────────────────────────────────── */

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
    off += 1;
    if (hdr.hash_appended) off += 32;
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

static const char *alarm_state_str(alarm_state_t s) {
    switch (s) {
        case ALARM_STATE_ACTIVE: return "ACTIVE";
        case ALARM_STATE_ARMED:  return "Armed";
        default:                 return "Off";
    }
}

static esp_err_t root_handler(httpd_req_t *req) {
    const esp_app_desc_t *desc = esp_app_get_description();
    uint32_t uptime_s = esp_timer_get_time() / 1000000;
    uint32_t up_h = uptime_s / 3600, up_m = (uptime_s % 3600) / 60, up_s = uptime_s % 60;
    uint32_t free_heap = esp_get_free_heap_size();

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

    alarm_state_t astate = slave_alarm_state;
    const char *astr = alarm_state_str(astate);
    const char *aclass = (astate == ALARM_STATE_ACTIVE) ? "active" :
                         (astate == ALARM_STATE_ARMED)  ? "armed"  : "";

    httpd_resp_set_type(req, "text/html");
#define SEND(s) httpd_resp_send_chunk(req, (s), strlen(s))

    SEND("<!DOCTYPE html><html><head>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<style>"
         "body{font-family:monospace;padding:16px;max-width:520px}"
         ".tabs{display:flex;gap:4px;margin-bottom:18px}"
         ".tb{flex:1;padding:11px 4px;cursor:pointer;border:1px solid #999;"
         "background:#eee;font-family:monospace;font-size:15px}"
         ".tb.on{background:#333;color:#fff;border-color:#333}"
         ".tc{display:none}.tc.on{display:block}"
         "button{font-family:monospace;padding:7px 12px;margin:2px}"
         ".active{color:red;font-weight:bold}"
         ".armed{color:orange;font-weight:bold}"
         "</style></head><body>"
         "<div class='tabs'>"
         "<button class='tb' data-tab='status' onclick=\"showTab('status')\">Status</button>"
         "<button class='tb' data-tab='ota'    onclick=\"showTab('ota')\">OTA</button>"
         "</div>");

    static char s_tab[512];
    snprintf(s_tab, sizeof(s_tab),
        "<div id='ts' class='tc'>"
        "<pre>Firmware:  %s\nSlot:      %s\nFree heap: %"PRIu32" B\n"
        "Uptime:    %02"PRIu32":%02"PRIu32":%02"PRIu32"\nIP:        " IPSTR "</pre>"
        "<p>Alarm: <span class='%s'><b>%s</b></span></p>"
        "</div>",
        desc->version, slot_label, free_heap, up_h, up_m, up_s,
        IP2STR(&s_ip_addr), aclass, astr);
    SEND(s_tab);

    static char o_tab[768];
    snprintf(o_tab, sizeof(o_tab),
        "<div id='to' class='tc'>"
        "<h3>Installed slots</h3>"
        "<p>ota_0: %s%s"
        " <a href='/download?slot=0'>[Download]</a>"
        " <button %s onclick='boot(0)'>Boot this</button></p>"
        "<p>ota_1: %s%s"
        " <a href='/download?slot=1'>[Download]</a>"
        " <button %s onclick='boot(1)'>Boot this</button></p>"
        "<h3>Upload new firmware</h3>"
        "<input type='file' id='fw' style='display:none'>"
        "<button onclick='pickFile()'>Choose File</button>"
        "<span id='fn' style='margin-left:8px'>No file selected</span><br><br>"
        "<button id='upbtn' onclick='upload()' disabled>Upload &amp; Reboot</button>"
        "<div id='st'></div>"
        "</div>",
        v0, r0, b0, v1, r1, b1);
    SEND(o_tab);

    SEND("<script>"
         "var tmr=null;"
         "function startRefresh(){if(!tmr)tmr=setInterval(()=>location.reload(),2000);}"
         "function stopRefresh(){clearInterval(tmr);tmr=null;}"
         "function showTab(n){"
         "['ts','to'].forEach(function(id){"
         "document.getElementById(id).className='tc';});"
         "document.querySelectorAll('.tb').forEach(function(b){"
         "b.classList.remove('on');});"
         "var ids={status:'ts',ota:'to'};"
         "document.getElementById(ids[n]).className='tc on';"
         "document.querySelector('[data-tab=\"'+n+'\"]').classList.add('on');"
         "location.hash=n;"
         "if(n==='status'){startRefresh();}else{stopRefresh();}}"
         "function pickFile(){stopRefresh();document.getElementById('fw').click();}"
         "document.getElementById('fw').onchange=function(){"
         "if(this.files[0]){"
         "document.getElementById('fn').textContent=this.files[0].name;"
         "document.getElementById('upbtn').disabled=false;}};"
         "async function boot(s){"
         "stopRefresh();"
         "document.getElementById('st').textContent='Switching slot...';"
         "var r=await fetch('/boot?slot='+s,{method:'POST'});"
         "document.getElementById('st').textContent=await r.text();}"
         "async function upload(){"
         "var f=document.getElementById('fw').files[0];"
         "if(!f){document.getElementById('st').textContent='Select a .bin file first';return;}"
         "document.getElementById('st').textContent='Uploading '+f.name+'...';"
         "try{"
         "var r=await fetch('/update',{method:'POST',body:f,"
         "headers:{'Content-Type':'application/octet-stream'}});"
         "document.getElementById('st').textContent=await r.text();"
         "}catch(e){document.getElementById('st').textContent='Error: '+e;}}"
         "showTab(location.hash.slice(1)||'status');"
         "</script></body></html>");

    httpd_resp_send_chunk(req, NULL, 0);
#undef SEND
    return ESP_OK;
}

static esp_err_t update_handler(httpd_req_t *req) {
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
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
        if (esp_ota_write(ota_handle, buf, n) != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
            return ESP_FAIL;
        }
        remaining -= n;
    }
    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(update_part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA finalise failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "Update successful — rebooting in 1 second.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
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
        snprintf(fname, sizeof(fname), "esp32_slave_%s.bin", app_desc.version);
    else
        snprintf(fname, sizeof(fname), "esp32_slave_ota%d.bin", slot);
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
    config.max_uri_handlers  = 6;
    httpd_handle_t server;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }
    httpd_uri_t root     = { .uri = "/",        .method = HTTP_GET,  .handler = root_handler };
    httpd_uri_t update   = { .uri = "/update",   .method = HTTP_POST, .handler = update_handler };
    httpd_uri_t boot_uri = { .uri = "/boot",     .method = HTTP_POST, .handler = boot_handler };
    httpd_uri_t dl_uri   = { .uri = "/download", .method = HTTP_GET,  .handler = download_handler };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &update);
    httpd_register_uri_handler(server, &boot_uri);
    httpd_register_uri_handler(server, &dl_uri);
    ESP_LOGI(TAG, "Web server started on http://" IPSTR, IP2STR(&s_ip_addr));
}

/* ── Main ─────────────────────────────────────────────────────────────── */

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(LED_GPIO, 0);

    xTaskCreate(led_task, "led", 1024, NULL, 5, NULL);

    wifi_init();
    espnow_init();
    webserver_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
