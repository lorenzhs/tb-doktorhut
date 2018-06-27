#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c.h"

#include "ssd1366.h"
#include "font8x8_basic.h"

#define SDA_PIN GPIO_NUM_15
#define SCL_PIN GPIO_NUM_2

#define tag "SSD1306"

void i2c_master_init()
{
	i2c_config_t i2c_config = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = SDA_PIN,
		.scl_io_num = SCL_PIN,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 1000000
	};
	i2c_param_config(I2C_NUM_0, &i2c_config);
	i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
}

void ssd1306_init() {
	esp_err_t espRc;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);

	i2c_master_write_byte(cmd, OLED_CMD_SET_CHARGE_PUMP, true);
	i2c_master_write_byte(cmd, 0x14, true);

	i2c_master_write_byte(cmd, OLED_CMD_SET_SEGMENT_REMAP, true); // reverse left-right mapping
	i2c_master_write_byte(cmd, OLED_CMD_SET_COM_SCAN_MODE, true); // reverse up-bottom mapping

	i2c_master_write_byte(cmd, OLED_CMD_DISPLAY_ON, true);
	i2c_master_stop(cmd);

	espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
	if (espRc == ESP_OK) {
		ESP_LOGI(tag, "OLED configured successfully");
	} else {
		ESP_LOGE(tag, "OLED configuration failed. code: 0x%.2X", espRc);
	}
	i2c_cmd_link_delete(cmd);
}

void task_ssd1306_display_pattern(void *ignore) {
	i2c_cmd_handle_t cmd;

	for (uint8_t i = 0; i < 8; i++) {
		cmd = i2c_cmd_link_create();
		i2c_master_start(cmd);
		i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
		i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_SINGLE, true);
		i2c_master_write_byte(cmd, 0xB0 | i, true);
		i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_DATA_STREAM, true);
		for (uint8_t j = 0; j < 128; j++) {
			i2c_master_write_byte(cmd, 0xFF >> (j % 8), true);
		}
		i2c_master_stop(cmd);
		i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
		i2c_cmd_link_delete(cmd);
	}

	vTaskDelete(NULL);
}

void task_ssd1306_display_clear(void *ignore) {
	i2c_cmd_handle_t cmd;

	uint8_t zero[128];
        memset(zero, 0, sizeof zero);
	for (uint8_t i = 0; i < 8; i++) {
		cmd = i2c_cmd_link_create();
		i2c_master_start(cmd);
		i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
		i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_SINGLE, true);
		i2c_master_write_byte(cmd, 0xB0 | i, true);

		i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_DATA_STREAM, true);
		i2c_master_write(cmd, zero, 128, true);
		i2c_master_stop(cmd);
		i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
		i2c_cmd_link_delete(cmd);
	}

	vTaskDelete(NULL);
}


void task_ssd1306_contrast(void *ignore) {
	i2c_cmd_handle_t cmd;

	uint8_t contrast = 0;
	uint8_t direction = 1;
	while (true) {
		cmd = i2c_cmd_link_create();
		i2c_master_start(cmd);
		i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
		i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);
		i2c_master_write_byte(cmd, OLED_CMD_SET_CONTRAST, true);
		i2c_master_write_byte(cmd, contrast, true);
		i2c_master_stop(cmd);
		i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
		i2c_cmd_link_delete(cmd);
		vTaskDelay(1/portTICK_PERIOD_MS);

		contrast += direction;
		if (contrast == 0xFF) { direction = -1; }
		if (contrast == 0x0) { direction = 1; }
	}
	vTaskDelete(NULL);
}

void task_ssd1306_scroll(void *ignore) {
	esp_err_t espRc;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);

	i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);

	i2c_master_write_byte(cmd, 0x29, true); // vertical and horizontal scroll (p29)
	i2c_master_write_byte(cmd, 0x00, true);
	i2c_master_write_byte(cmd, 0x00, true);
	i2c_master_write_byte(cmd, 0x07, true);
	i2c_master_write_byte(cmd, 0x01, true);
	i2c_master_write_byte(cmd, 0x3F, true);

	i2c_master_write_byte(cmd, 0xA3, true); // set vertical scroll area (p30)
	i2c_master_write_byte(cmd, 0x20, true);
	i2c_master_write_byte(cmd, 0x40, true);

	i2c_master_write_byte(cmd, 0x2F, true); // activate scroll (p29)

	i2c_master_stop(cmd);
	espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
	if (espRc == ESP_OK) {
		ESP_LOGI(tag, "Scroll command succeeded");
	} else {
		ESP_LOGE(tag, "Scroll command failed. code: 0x%.2X", espRc);
	}

	i2c_cmd_link_delete(cmd);

	vTaskDelete(NULL);
}

void task_ssd1306_display_text(const void *arg_text) {
	char *text = (char*)arg_text;
	uint8_t text_len = strlen(text);

	i2c_cmd_handle_t cmd;

	uint8_t cur_page = 0;

	cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);

	i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);
	i2c_master_write_byte(cmd, 0x00, true); // reset column
	i2c_master_write_byte(cmd, 0x10, true);
	i2c_master_write_byte(cmd, 0xB0 | cur_page, true); // reset page

	i2c_master_stop(cmd);
	i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
	i2c_cmd_link_delete(cmd);

	for (uint8_t i = 0; i < text_len; i++) {
		if (text[i] == '\n') {
			cmd = i2c_cmd_link_create();
			i2c_master_start(cmd);
			i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);

			i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);
			i2c_master_write_byte(cmd, 0x00, true); // reset column
			i2c_master_write_byte(cmd, 0x10, true);
			i2c_master_write_byte(cmd, 0xB0 | ++cur_page, true); // increment page

			i2c_master_stop(cmd);
			i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
			i2c_cmd_link_delete(cmd);
		} else {
			cmd = i2c_cmd_link_create();
			i2c_master_start(cmd);
			i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);

			i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_DATA_STREAM, true);
			i2c_master_write(cmd, font8x8_basic_tr[(uint8_t)text[i]], 8, true);

			i2c_master_stop(cmd);
			i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
			i2c_cmd_link_delete(cmd);
		}
	}

	vTaskDelete(NULL);
}


esp_err_t event_handler(void *ctx, system_event_t *event)
{
    return ESP_OK;
}

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

    i2c_master_init();
    ssd1306_init();

    xTaskCreate(&task_ssd1306_display_clear, "ssd1306_display_clear", 2048, NULL, 6, NULL);
    vTaskDelay(100/portTICK_PERIOD_MS);
    xTaskCreate(&task_ssd1306_display_text, "ssd1306_display_text", 2048,
                (void *)"EUR/USD=1.2345", 6, NULL);


    nvs_flash_init();
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
    }
}
