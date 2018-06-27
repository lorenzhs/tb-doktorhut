#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"

#include "esp_http_client.h"

/*=================================================*/
// display stuff
#include "driver/gpio.h"
#include "driver/i2c.h"

#define SDA_PIN GPIO_NUM_15
#define SCL_PIN GPIO_NUM_2
#define tag "SSD1306"

#include "font8x8_basic.h"
#include "ssd1366.h"

/*=================================================*/
// wifi stuff

#define WIFI_SSID "KA-WLAN"
#define WIFI_PASS ""

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

#define WEB_SERVER "webrates.truefx.com"
#define WEB_PORT "80"
#define WEB_URL "http://webrates.truefx.com/rates/connect.html"

static const char *TAG = "example";

static const char *REQUEST = "GET " WEB_URL " HTTP/1.0\r\n"
    "Host: "WEB_SERVER"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";


/******************************************************************************/
/*** WiFi Login (KA-WLAN)******************************************************/

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Write out data
                // printf("%.*s", evt->data_len, (char*)evt->data);
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

static int wifi_login()
{
    int status = 1;

    esp_http_client_config_t config = {
        .url = "http://cp.ka-wlan.de/login",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // GET login form

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET error requesting login page: %s",
                 esp_err_to_name(err));
        goto _exit;
    }
    esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));

    // read data
    const int bufsize = 5000;
    char* data = malloc(bufsize);
    int read_index = 0, total_len = 0;
    while (1) {
        int read_len = esp_http_client_read(client, data + read_index, bufsize - read_index);
        ESP_LOGI(TAG, "Read %d bytes", read_len);
        if (read_len <= 0) {
            break;
        }
        read_index += read_len;
        total_len += read_len;
        data[read_index] = 0;
    }
    if (total_len <= 0) {
        ESP_LOGE(TAG, "Invalid length of the response");
        free(data);
        goto _exit;
    }

    ESP_LOGD(TAG, "Data=%s", data);

    // extract login information
    const char done_needle[] = "Sie sind erfolgreich eingeloggt.";
    const char mac_needle[] = "<input type=\"hidden\" name=\"username\" type=\"text\" value=\"";
    const char pw_needle[] = "<input type=\"hidden\" name=\"password\" type=\"password\" value=\"";

    if (strstr(data, done_needle) == NULL) {
        // we're already logged in
        free(data);
        status = 0;
        goto _exit;
    }

    char* mac_begin = strstr(data, mac_needle) + strlen(mac_needle);
    char* mac_end = mac_begin + 17;

    char* pw_begin = strstr(data, pw_needle) + strlen(pw_needle);
    char* pw_end = strstr(pw_begin, "\"");

    ESP_LOGI(TAG, "Found MAC address at %d - %d, password at %d - %d",
             mac_begin - data, mac_end - data, pw_begin - data, pw_end - data);

    *mac_end = 0;
    *pw_end = 0;

    char mac[18];
    char pw[32];

    strncpy(mac, mac_begin, 18);
    strncpy(pw, pw_begin, 32);

    ESP_LOGI(TAG, "MAC: %s, Pass: %s", mac, pw);

    free(data);

    // assemble POST request
    int post_data_len = 55 /* default params */ + 17 /* mac */ + strlen(pw);
    char *post_data = malloc(post_data_len);
    sprintf(post_data,
            "dst=http://example.com&popup=false&username=%s&password=%s",
            mac, pw);
    ESP_LOGI(TAG, "post data: %s", post_data);

    // POST the login request
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    status = 0;

_exit:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return status;
}

static esp_http_client_handle_t get_quote_client() {
    esp_http_client_config_t config = {
        .url = "http://webrates.truefx.com/rates/connect.html",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    return client;
}

static void update_quote(esp_http_client_handle_t client,
                         char* buf, int bufsize, char* dest, int destsize) {
    /* Step 1: fetch new quote */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET error requesting quote: %s",
                 esp_err_to_name(err));
        return;
    }
    esp_http_client_fetch_headers(client);

    // read data
    int read_index = 0, total_len = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buf + read_index, bufsize - read_index);
        ESP_LOGI(TAG, "Read %d bytes", read_len);
        if (read_len <= 0) {
            break;
        }
        read_index += read_len;
        total_len += read_len;
        buf[read_index] = 0;
    }
    if (total_len <= 0) {
        ESP_LOGE(TAG, "Invalid length of the response");
        return;
    }

    ESP_LOGI(TAG, "Response: %s", buf);
}

/******************************************************************************/
/*** WiFi Authentication ******************************************************/

esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

/******************************************************************************/
/*** Main Logic ***************************************************************/

void app_main(void)
{
    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ",
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    printf("silicon revision %d, ", chip_info.revision);

    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

/* init display */
    i2c_master_init();
    ssd1306_init();

    xTaskCreate(&task_ssd1306_display_clear, "ssd1306_display_clear", 2048, NULL, 6, NULL);
    vTaskDelay(100/portTICK_PERIOD_MS);
    xTaskCreate(&task_ssd1306_display_text, "ssd1306_display_text", 2048,
                (void *)"EUR/USD=1.2345", 6, NULL);
/* end display init */

    ESP_ERROR_CHECK( nvs_flash_init() );
    /*
    tcpip_adapter_init();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    wifi_config_t sta_config = {
        .sta = {
            .ssid = "KA-WLAN",
            .password = "",
            .bssid_set = false
        }
    };
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_ERROR_CHECK( esp_wifi_connect() );

    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    int level = 0;
    while (true) {
        gpio_set_level(GPIO_NUM_4, level);
        level = !level;
        vTaskDelay(300 / portTICK_PERIOD_MS);
        }*/

    initialise_wifi();

    /* Wait for the callback to set the CONNECTED_BIT in the
       event group.
    */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to AP");

    int status = wifi_login();
    if (status != 0)
        ESP_LOGE(TAG, "Couldn't log into KA-WLAN?");

    // get http client for quote fetching
    esp_http_client_handle_t client = get_quote_client();

    // initialise buffers
    const int bufsize = 512, stringsize = 128;
    char *buf = malloc(bufsize),
        *string = malloc(stringsize);

    update_quote(client, buf, bufsize, string, stringsize);

    /* xTaskCreate(&http_get_task, "http_get_task", 4096, NULL, 5, NULL); */
}
