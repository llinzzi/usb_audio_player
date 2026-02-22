/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "audio_player.h"
#include "sdmmc_cmd.h"

static const char *TAG = "usb_audio_player";

// SD Card pins (Waveshare ESP32-S3)
#define SD_MOSI_PIN     (1)
#define SD_SCK_PIN      (2)
#define SD_MISO_PIN     (3)
// SD CS is controlled by TCA9554 EXIO7 (P7)

// TCA9554PWR I2C configuration (Waveshare ESP32-S3)
#define I2C_NUM         I2C_NUM_0
#define I2C_SDA_PIN     (15)
#define I2C_SCL_PIN     (14)
#define TCA9554_ADDR    0x20

// TCA9554 pin mapping - P7 is EXIO7 for SD CS
#define TCA9554_SD_CS_BIT   (0x80)  // P7 = bit 7

// TCA9554 register addresses
#define TCA9554_INPUT_PORT      0x00
#define TCA9554_OUTPUT_PORT     0x01
#define TCA9554_POLARITY_INV    0x02
#define TCA9554_CONFIGURATION   0x03

// SD card mount point
#define MOUNT_POINT             "/sdcard"
#define MP3_FILE_NAME           "/new_epic.mp3"

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2
#define DEFAULT_VOLUME          50
#define DEFAULT_UAC_FREQ        48000
#define DEFAULT_UAC_BITS        16
#define DEFAULT_UAC_CH          2

static QueueHandle_t s_event_queue = NULL;
static uac_host_device_handle_t s_spk_dev_handle = NULL;
static uint32_t s_spk_curr_freq = DEFAULT_UAC_FREQ;
static uint8_t s_spk_curr_bits = DEFAULT_UAC_BITS;
static uint8_t s_spk_curr_ch = DEFAULT_UAC_CH;
static FILE *s_fp = NULL;
static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg);

/**
 * @brief event group
 *
 * APP_EVENT            - General control event
 * UAC_DRIVER_EVENT     - UAC Host Driver event, such as device connection
 * UAC_DEVICE_EVENT     - UAC Host Device event, such as rx/tx completion, device disconnection
 */
typedef enum {
    APP_EVENT = 0,
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

/**
 * @brief event queue
 *
 * This event is used for delivering the UAC Host event from callback to the uac_lib_task
 */
typedef struct {
    event_group_t event_group;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_handle_t handle;
            uac_host_driver_event_t event;
            void *arg;
        } device_evt;
    };
} s_event_queue_t;

/**
 * @brief Write a byte to TCA9554 register
 */
static esp_err_t tca9554_write_reg(uint8_t reg_addr, uint8_t value)
{
    uint8_t write_buf[2] = {reg_addr, value};
    return i2c_master_write_to_device(I2C_NUM, TCA9554_ADDR, write_buf, sizeof(write_buf), 100 / portTICK_PERIOD_MS);
}

/**
 * @brief Initialize TCA9554PWR IO expander
 */
static esp_err_t tca9554_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM, conf.mode, 0, 0, 0));

    // Configure P7 (EXIO7/SDCS) as output, others as input
    // All outputs low by default
    ESP_ERROR_CHECK(tca9554_write_reg(TCA9554_OUTPUT_PORT, 0x00));
    // P7 as output (0), others as input (1) = 10000000 = 0x80
    ESP_ERROR_CHECK(tca9554_write_reg(TCA9554_CONFIGURATION, 0x80));

    ESP_LOGI(TAG, "TCA9554 initialized");
    return ESP_OK;
}

/**
 * @brief Set SD card CS pin state
 * @param select true to select SD card, false to deselect
 */
static void sd_cs_set(bool select)
{
    // P7 low = SD card selected, P7 high = SD card deselected
    uint8_t value = select ? 0x00 : TCA9554_SD_CS_BIT;
    tca9554_write_reg(TCA9554_OUTPUT_PORT, value);
}

/**
 * @brief Initialize SD card
 */
static esp_err_t sdcard_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");

    // Initialize TCA9554 first
    ESP_ERROR_CHECK(tca9554_init());

    // Select SD card (P7 low)
    sd_cs_set(true);
    vTaskDelay(10 / portTICK_PERIOD_MS);

    // Configure SPI bus for SD card
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    // Use SPI SD card mode
    sdmmc_card_t *card;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = -1;  // CS controlled by TCA9554 externally

    // Mount filesystem
    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card filesystem (%s)", esp_err_to_name(ret));
        sd_cs_set(false);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return ESP_OK;
}

static esp_err_t _audio_player_mute_fn(AUDIO_PLAYER_MUTE_SETTING setting)
{
    if (s_spk_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "mute setting: %s", setting == AUDIO_PLAYER_MUTE ? "mute" : "unmute");
    // some uac devices may not support mute, so we not check the return value
    if (setting == AUDIO_PLAYER_UNMUTE) {
        uac_host_device_set_volume(s_spk_dev_handle, DEFAULT_VOLUME);
        uac_host_device_set_mute(s_spk_dev_handle, false);
    } else {
        uac_host_device_set_volume(s_spk_dev_handle, 0);
        uac_host_device_set_mute(s_spk_dev_handle, true);
    }
    return ESP_OK;
}

static esp_err_t _audio_player_write_fn(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (s_spk_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *bytes_written = 0;
    esp_err_t ret = uac_host_device_write(s_spk_dev_handle, audio_buffer, len, timeout_ms);
    if (ret == ESP_OK) {
        *bytes_written = len;
    }
    return ret;
}

static esp_err_t _audio_player_std_clock(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    if (s_spk_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rate == s_spk_curr_freq && bits_cfg == s_spk_curr_bits && ch == s_spk_curr_ch) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Re-config: speaker rate %"PRIu32", bits %"PRIu32", mode %s", rate, bits_cfg, ch == 1 ? "MONO" : (ch == 2 ? "STEREO" : "INVALID"));
    ESP_ERROR_CHECK(uac_host_device_stop(s_spk_dev_handle));
    const uac_host_stream_config_t stm_config = {
        .channels = ch,
        .bit_resolution = bits_cfg,
        .sample_freq = rate,
    };
    s_spk_curr_freq = rate;
    s_spk_curr_bits = bits_cfg;
    s_spk_curr_ch = ch;
    return uac_host_device_start(s_spk_dev_handle, &stm_config);
}

static void _audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    ESP_LOGI(TAG, "ctx->audio_event = %d", ctx->audio_event);
    switch (ctx->audio_event) {
    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE: {
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_IDLE");
        if (s_spk_dev_handle == NULL) {
            break;
        }
        ESP_ERROR_CHECK(uac_host_device_suspend(s_spk_dev_handle));
        ESP_LOGI(TAG, "Play in loop");
        s_fp = fopen(MOUNT_POINT MP3_FILE_NAME, "rb");
        if (s_fp) {
            ESP_LOGI(TAG, "Playing '%s'", MP3_FILE_NAME);
            audio_player_play(s_fp);
        } else {
            ESP_LOGE(TAG, "unable to open filename '%s'", MP3_FILE_NAME);
        }
        break;
    }
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_PLAY");
        if (s_spk_dev_handle == NULL) {
            break;
        }
        ESP_ERROR_CHECK(uac_host_device_resume(s_spk_dev_handle));
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_PAUSE");
        break;
    default:
        break;
    }
}

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        // stop audio player first
        s_spk_dev_handle = NULL;
        audio_player_stop();
        ESP_LOGI(TAG, "UAC Device disconnected");
        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
        return;
    }
    // Send uac device event to the event queue
    s_event_queue_t evt_queue = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };
    // should not block here
    xQueueSend(s_event_queue, &evt_queue, 0);
}

static void uac_host_lib_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    // Send uac driver event to the event queue
    s_event_queue_t evt_queue = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    xQueueSend(s_event_queue, &evt_queue, 0);
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive(arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // In this example, there is only one client registered
        // So, once we deregister the client, this call must succeed with ESP_OK
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB Host shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

static void uac_lib_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
    s_event_queue_t evt_queue = {0};
    while (1) {
        if (xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY)) {
            if (UAC_DRIVER_EVENT ==  evt_queue.event_group) {
                uac_host_driver_event_t event = evt_queue.driver_evt.event;
                uint8_t addr = evt_queue.driver_evt.addr;
                uint8_t iface_num = evt_queue.driver_evt.iface_num;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_TX_CONNECTED: {
                    uac_host_dev_info_t dev_info;
                    uac_host_device_handle_t uac_device_handle = NULL;
                    const uac_host_device_config_t dev_config = {
                        .addr = addr,
                        .iface_num = iface_num,
                        .buffer_size = 16000,
                        .buffer_threshold = 4000,
                        .callback = uac_device_callback,
                        .callback_arg = NULL,
                    };
                    ESP_ERROR_CHECK(uac_host_device_open(&dev_config, &uac_device_handle));
                    ESP_ERROR_CHECK(uac_host_get_device_info(uac_device_handle, &dev_info));
                    ESP_LOGI(TAG, "UAC Device connected: SPK");
                    uac_host_printf_device_param(uac_device_handle);
                    // Start usb speaker with the default configuration
                    const uac_host_stream_config_t stm_config = {
                        .channels = s_spk_curr_ch,
                        .bit_resolution = s_spk_curr_bits,
                        .sample_freq = s_spk_curr_freq,
                    };
                    ESP_ERROR_CHECK(uac_host_device_start(uac_device_handle, &stm_config));
                    s_spk_dev_handle = uac_device_handle;
                    s_fp = fopen(MOUNT_POINT MP3_FILE_NAME, "rb");
                    if (s_fp) {
                        ESP_LOGI(TAG, "Playing '%s'", MP3_FILE_NAME);
                        audio_player_play(s_fp);
                    } else {
                        ESP_LOGE(TAG, "unable to open filename '%s'", MP3_FILE_NAME);
                    }
                    break;
                }
                case UAC_HOST_DRIVER_EVENT_RX_CONNECTED: {
                    // we don't support MIC in this example
                    ESP_LOGI(TAG, "UAC Device connected: MIC");
                    break;
                }
                default:
                    break;
                }
            } else if (UAC_DEVICE_EVENT == evt_queue.event_group) {
                uac_host_device_event_t event = evt_queue.device_evt.event;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                    s_spk_curr_bits = DEFAULT_UAC_BITS;
                    s_spk_curr_freq = DEFAULT_UAC_FREQ;
                    s_spk_curr_ch = DEFAULT_UAC_CH;
                    ESP_LOGI(TAG, "UAC Device disconnected");
                    break;
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                    break;
                case UAC_HOST_DEVICE_EVENT_TX_DONE:
                    break;
                case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                    break;
                default:
                    break;
                }
            } else if (APP_EVENT == evt_queue.event_group) {
                break;
            }
        }
    }

    ESP_LOGI(TAG, "UAC Driver uninstall");
    ESP_ERROR_CHECK(uac_host_uninstall());
}

void app_main(void)
{
    s_event_queue = xQueueCreate(10, sizeof(s_event_queue_t));
    assert(s_event_queue != NULL);

    // Initialize SD card
    ESP_ERROR_CHECK(sdcard_init());

    audio_player_config_t config = {.mute_fn = _audio_player_mute_fn,
                                    .write_fn = _audio_player_write_fn,
                                    .clk_set_fn = _audio_player_std_clock,
                                    .priority = 1
                                   };
    ESP_ERROR_CHECK(audio_player_new(config));
    ESP_ERROR_CHECK(audio_player_callback_register(_audio_player_callback, NULL));

    static TaskHandle_t uac_task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(uac_lib_task, "uac_events", 4096, NULL,
                                             USER_TASK_PRIORITY, &uac_task_handle, 0);
    assert(ret == pdTRUE);
    ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, (void *)uac_task_handle,
                                  USB_HOST_TASK_PRIORITY, NULL, 0);
    assert(ret == pdTRUE);

    while (1) {
        vTaskDelay(100);
    }

}
