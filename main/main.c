/*
 * TB-Doktorhut
 *
 * Possible todo items:
 *
 * - if exchange rate didn't change in the last ~10 fetches, double timeout (up
 *   to 1 minute or so) to prevent weekend API hammering
 */

#include <string.h>
#include <time.h>
#include <stdlib.h>

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

#define SDA_PIN GPIO_NUM_4
#define SCL_PIN GPIO_NUM_18
#define tag "SSD1306"

#include "font8x8_basic.h"
#include "ssd1366.h"

// display string
#define STRINGSIZE 100
char string[STRINGSIZE];


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

static const char *TAG = "tbhut";

// wifi quote buffer
#define BUFSIZE 5000
char buf[BUFSIZE];

char post_data[100];

TickType_t quote_lastwake;

/***************************************************/
// LED stuff

#include "esp32_digital_led_lib.h"

#define LED_PIN GPIO_NUM_14
#define LED_LEN 41

#ifndef __cplusplus
#define nullptr  NULL
#endif

#define floor(a)   ((int)(a))

#define BR_NORM 0.1
#define BR_FLASH 0.5

strand_t STRANDS[] = { // Avoid using any of the strapping pins on the ESP32
    {.rmtChannel = 1, .gpioNum = LED_PIN, .ledType = LED_SK6812W_V1,
     .brightLimit = (int)(BR_NORM * 255), .numPixels = LED_LEN,
     .pixels = nullptr, ._stateVars = nullptr},
};
strand_t *strand = &STRANDS[0];

int STRANDCNT = sizeof(STRANDS)/sizeof(STRANDS[0]);

int arr[LED_LEN];
float colours[LED_LEN];
int flash1 = -1, flash2 = -1;

TickType_t led_lastwake;


static void safe_sleep(int ms, TickType_t *last_wake) {
    const int loops = ms / 10;

    for (int i = 0; i < loops; i++) {
        vTaskDelayUntil(last_wake, 10 / portTICK_PERIOD_MS);
        *last_wake = xTaskGetTickCount();
    }

    if (loops * 10 < ms) {
        vTaskDelayUntil(last_wake, (ms - 10 * loops) / portTICK_PERIOD_MS);
        *last_wake = xTaskGetTickCount();
    }
}


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
    int read_index = 0, total_len = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buf + read_index, BUFSIZE - read_index);
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
        goto _exit;
    }

    ESP_LOGD(TAG, "Data=%s", buf);

    // extract login information
    const char done_needle[] = "Sie sind erfolgreich eingeloggt.";

    if (strstr(buf, done_needle) != NULL) {
        // we're already logged in
        ESP_LOGI(TAG, "already logged in, aborting");
        status = 0;
        goto _exit;
    }

    const char mac_needle[] = "<input type=\"hidden\" name=\"username\" type=\"text\" value=\"";
    const char pw_needle[] = "<input type=\"hidden\" name=\"password\" type=\"password\" value=\"";

    char* mac_begin = strstr(buf, mac_needle) + strlen(mac_needle);
    char* mac_end = mac_begin + 17;

    char* pw_begin = strstr(buf, pw_needle) + strlen(pw_needle);
    char* pw_end = strstr(pw_begin, "\"");

    ESP_LOGI(TAG, "Found MAC address at %d - %d, password at %d - %d",
             mac_begin - buf, mac_end - buf, pw_begin - buf, pw_end - buf);

    *mac_end = 0;
    *pw_end = 0;

    char mac[18];
    char pw[32];

    strncpy(mac, mac_begin, 18);
    strncpy(pw, pw_begin, 32);

    ESP_LOGI(TAG, "MAC: %s, Pass: %s", mac, pw);

    // assemble POST request
    //int post_data_len = 55 /* default params */ + 17 /* mac */ + strlen(pw);
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

static void update_quote(esp_http_client_handle_t client) {
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
        int read_len = esp_http_client_read(client, buf + read_index, BUFSIZE - read_index);
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

    ESP_LOGD(TAG, "Response: %s", buf);

    char* dest = string;
    sprintf(dest, "TB Forex Rates\n");
    dest += 15; // hack

    // EUR/USD
    memcpy(dest, buf, 7);
    sprintf(dest + 7, "         \nBid: ");
    memcpy(dest + 22, buf + 70, 4);
    memcpy(dest + 26, buf + 110, 3);
    sprintf(dest + 29, "\nAsk: ");
    memcpy(dest + 35, buf + 140, 4);
    memcpy(dest + 39, buf + 180, 3);
    dest[42] = '\n';
    dest[43] = '\n';

    // GBP/USD
    memcpy(dest + 44, buf + 14, 7);
    sprintf(dest + 51, "         \nBid: ");
    memcpy(dest + 66, buf + 78, 4);
    memcpy(dest + 70, buf + 116, 3);
    sprintf(dest + 73, "\nAsk: ");
    memcpy(dest + 79, buf + 148, 4);
    memcpy(dest + 83, buf + 186, 3);
    dest[86] = 0;

    ESP_LOGI(TAG, "Quote: %s", dest);
}

static void quote_task(void* pvParam) {
    // Wait for the callback to set the CONNECTED_BIT in the event group.
    ESP_LOGI(TAG, "Waiting for WiFi connection...");

    int dots = 0;
    EventBits_t bits;
    do {
        sprintf(string, "Connecting\nto WiFi");
        for (int i = 0; i < dots; ++i) {
            sprintf(string + strlen(string), ".");
        }
        // clearing
        for (int i = dots; i < 5; ++i) {
            sprintf(string + strlen(string), " ");
        }

        xTaskCreate(&task_ssd1306_display_text, "ssd1306_display_text", 2048,
                    string, 6, NULL);

        dots = (dots + 1) % 5;

        bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false,
                                   true, 1000 / portTICK_PERIOD_MS);

    } while (!(bits & CONNECTED_BIT));
    ESP_LOGI(TAG, "Connected to AP");

    // Try to log into KA-WLAN
    int connectDelay = 1000;
    while (wifi_login() != 0) {
        ESP_LOGE(TAG, "Couldn't log into KA-WLAN?"
                 "Waiting %d ms before trying to reconnect", connectDelay);
        vTaskDelay(connectDelay / portTICK_PERIOD_MS);
        connectDelay *= 1.5;
    }

    xTaskCreate(&task_ssd1306_display_clear, "ssd1306_display_clear", 2048,
                NULL, 6, NULL);


    // get http client for quote fetching
    esp_http_client_handle_t client = get_quote_client();

    while (1) {
        ESP_LOGI(TAG, "fetching updated quote...");
        update_quote(client);

        /* update text */
        xTaskCreate(&task_ssd1306_display_text, "ssd1306_display_text", 2048,
                    string, 6, NULL);

        quote_lastwake = xTaskGetTickCount();

        taskYIELD();
        /* delay */
        safe_sleep(1000, &quote_lastwake);
    }

    vTaskDelete(NULL);
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
/*** LED control***************************************************************/

void LED_setup(gpio_num_t gpioNum, int gpioMode, int gpioVal) {
    gpio_mode_t gpioModeNative = (gpio_mode_t)(gpioMode);
    gpio_pad_select_gpio(gpioNum);
    gpio_set_direction(gpioNum, gpioModeNative);
    gpio_set_level(gpioNum, gpioVal);
}

uint32_t IRAM_ATTR millis()
{
  return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

pixelColor_t hsv_to_rgb(float h, float s, float v) {
    /* h in in range [0,6], s and v in [0,1]
       result is in form #rrggbb */
    int i;
    float m, n, f;

    // Saturation or value of out range => return black
    if ((s<0.0) || (s>1.0) || (v<0.0) || (v>1.0)) {
        return pixelFromRGB(0,0,0);
    }

    if ((h < 0.0) || (h > 6.0)) {
        // Hue out of range => return grey according to value
        v *= 255;
        return pixelFromRGB(v, v, v);
        //return long(v) | long(v) << 8 | long(v) << 16;
    }

    i = floor(h);
    f = h - i;
    if ( !(i&1) ) {
        f = 1 - f; // if i is even
    }
    m = v * (1 - s);
    n = v * (1 - s * f);

    v *= 255;
    n *= 255;
    m *= 255;
    /* B, R, G */
    switch (i) {
    case 6:
    case 0: // (v, n, m)
        return pixelFromRGB(n, m, v);
        //return long(v ) << 16 | long(n) << 8 | long(m);
    case 1: // (n, v, m)
        return pixelFromRGB(v, m, n);
        //return long(n ) << 16 | long(v) << 8 | long(m);
    case 2: // (m, v, n)
        return pixelFromRGB(v, n, m);
        //return long(m ) << 16 | long(v) << 8 | long(n);
    case 3: // (m, n, v)
        return pixelFromRGB(n, v, m);
        //return long(m ) << 16 | long(n) << 8 | long(v);
    case 4: // (n, m, v)
        return pixelFromRGB(m, v, n);
        //return long(n ) << 16 | long(m) << 8 | long(v);
    case 5: // (v, m, n)
        return pixelFromRGB(m, n, v);
        //return long(v ) << 16 | long(m) << 8 | long(n);
    }
    ESP_LOGE(TAG, "hsv_to_rgb error: default case reached!");
    return pixelFromRGB(0, 0, 0);
}


void init_sort() {
    for (int i = 0; i < LED_LEN; i++) {
        arr[i] = rand();
        colours[i] = (float)arr[i] / (float)(RAND_MAX/6.0);
        ESP_LOGD("sort", "arr[%d] = %d, hue: %f", i, arr[i], colours[i]);
    }
}

static void led_update() {
    for (int i = 0; i < LED_LEN; i++) {
        strand->pixels[i] = hsv_to_rgb(colours[i], 1.0, BR_NORM);
    }

    ESP_LOGD("sort", "led_update: flashing pixels %d and %d", flash1, flash2);
    if (flash1 >= LED_LEN || flash2 >= LED_LEN) {
        ESP_LOGE("sort", "out of bounds flash index: %d %d", flash1, flash2);
    }
    if (flash1 >= 0)
        strand->pixels[flash1] = hsv_to_rgb(colours[flash1], 1.0, BR_FLASH);
    if (flash2 >= 0)
        strand->pixels[flash2] = hsv_to_rgb(colours[flash2], 1.0, BR_FLASH);


    digitalLeds_updatePixels(strand);

    flash1 = -1; flash2 = -1;

    // voluntarily yield CPU to other tasks (for wifi stuff)
    //taskYIELD();
    safe_sleep(100, &led_lastwake);
}

static void swap(int a, int b) {
    if (a >= LED_LEN || b >= LED_LEN)
        ESP_LOGE("sort", "out of bounds swap: %d %d", a, b);

    float temp_colour = colours[a];
    int temp_val = arr[a];
    colours[a] = colours[b]; arr[a] = arr[b];
    colours[b] = temp_colour; arr[b] = temp_val;

    flash1 = a; flash2 = b;

    led_update();
}

static int partition (int low, int high)
{
    int pi = rand() % (high - low) + low;
    int pivot = arr[pi];
    swap(pi, high);

    int i = (low - 1);  // Index of smaller element

    for (int j = low; j <= high- 1; j++) {
        if (arr[j] <= pivot)
        {
            i++;
            swap(i, j);
        }
    }
    swap(i + 1, high);
    return (i + 1);
}

/* low  --> Starting index,
   high  --> Ending index */
static void quickSort(int level, int low, int high) {
    if (low < high) {
        int pi = partition(low, high);
        ESP_LOGD("sort", "level %d, pivot: %d -> %d / %f",
                 level, pi, arr[pi], colours[pi]);

        quickSort(level + 1, low, pi - 1);
        quickSort(level + 1, pi + 1, high);
    }
}

static void LED_task(void *pvParameters) {
    led_lastwake = xTaskGetTickCount();
    while (1) {
        ESP_LOGI("sort", "initialising...");
        init_sort();
        led_update();

        ESP_LOGI("sort", "starting quicksort");
        quickSort(0, 0, LED_LEN - 1);

        led_update();

        ESP_LOGI("sort", "done, short pause");
        safe_sleep(2000, &led_lastwake);
    }

    vTaskDelete(NULL);
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
    ESP_ERROR_CHECK( nvs_flash_init() );

    // Initialise display
    i2c_master_init();
    ssd1306_init();

    xTaskCreate(&task_ssd1306_display_clear, "ssd1306_display_clear", 2048, NULL, 6, NULL);
    vTaskDelay(100/portTICK_PERIOD_MS);


    // Initialise LEDs
    LED_setup(LED_PIN, GPIO_MODE_OUTPUT, 0);
    if (digitalLeds_initStrands(STRANDS, STRANDCNT)) {
        ESP_LOGE(TAG, "LED init failure :(");
    }
    srand(esp_random());

    // schedule LED sorting task
    xTaskCreate(&LED_task, "LED_task", 2048, NULL, 4, NULL);

    // Connect to wifi
    initialise_wifi();

    xTaskCreate(&quote_task, "quote_task", 2048, NULL, 6, NULL);
}
