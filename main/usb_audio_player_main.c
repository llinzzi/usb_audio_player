/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/i2c_master.h"
#include "esp_io_expander_tca9554.h"
#include "driver/sdmmc_host.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "audio_player.h"

static const char *TAG = "usb_audio_player";

// I2C configuration for TCA9554
#define I2C_NUM             I2C_NUM_0
#define I2C_SCL_PIN         GPIO_NUM_14
#define I2C_SDA_PIN         GPIO_NUM_15
#define I2C_FREQ_HZ         400000

// SD card configuration (1-bit SDMMC mode for Waveshare ESP32-S3-Touch-AMOLED-1.8)
// From official example: CONFIG_EXAMPLE_PIN_CLK=2, CONFIG_EXAMPLE_PIN_CMD=1, CONFIG_EXAMPLE_PIN_D0=3
#define SD_CLK_PIN          GPIO_NUM_2
#define SD_CMD_PIN          GPIO_NUM_1
#define SD_D0_PIN           GPIO_NUM_3

// SD card configuration
#define SD_BASE_PATH        "/sd"
#define MP3_FILE_NAME       "/new_epic.mp3"

// USB Host and UAC task priorities
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
static esp_io_expander_handle_t s_io_expander = NULL;
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
        s_fp = fopen(SD_BASE_PATH MP3_FILE_NAME, "rb");
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
                    s_fp = fopen(SD_BASE_PATH MP3_FILE_NAME, "rb");
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

/**
 * @brief Initialize TCA9554 I/O expander and power on SD card
 */
static esp_err_t tca9554_init(void)
{
    ESP_LOGI(TAG, "Initializing TCA9554 I/O expander");

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_NUM,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

    esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &s_io_expander);

    if (ret != ESP_OK || s_io_expander == NULL) {
        ESP_LOGE(TAG, "Failed to initialize TCA9554");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "TCA9554 initialized");

    // Configure SD card power control pins as output
    // IO0, IO1, IO2, IO7 are used for SD card power/control
    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_io_expander,
                                            IO_EXPANDER_PIN_NUM_0 |
                                            IO_EXPANDER_PIN_NUM_1 |
                                            IO_EXPANDER_PIN_NUM_2 |
                                            IO_EXPANDER_PIN_NUM_7,
                                            IO_EXPANDER_OUTPUT));

    // Set all pins low first
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_0, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_1, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_2, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_7, 0));

    vTaskDelay(pdMS_TO_TICKS(200));

    // Set pins high to power on SD card
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_0, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_1, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_2, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_7, 1));

    ESP_LOGI(TAG, "SD card powered on");

    return ESP_OK;
}

/**
 * @brief Initialize SD card using SDMMC peripheral (1-bit mode)
 */
static esp_err_t sd_card_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card using SDMMC peripheral (1-bit mode)");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // This initializes the slot without card detect (CD) and write protect (WP) signals
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // Set pin numbers - 1-bit SDMMC mode
    slot_config.clk = SD_CLK_PIN;
    slot_config.cmd = SD_CMD_PIN;
    slot_config.d0 = SD_D0_PIN;
    slot_config.width = 1;  // Use 1-bit SDMMC mode

    // Enable internal pullups on enabled pins
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_BASE_PATH, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. Check if SD card is formatted.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card (0x%x)", ret);
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_BASE_PATH);
    ESP_LOGI(TAG, "SD card capacity: %llu MB", (uint64_t)card->csd.capacity / (1024 * 1024));

    return ESP_OK;
}

void app_main(void)
{
    s_event_queue = xQueueCreate(10, sizeof(s_event_queue_t));
    assert(s_event_queue != NULL);

    // Initialize TCA9554 I/O expander
    ESP_ERROR_CHECK(tca9554_init());

    // Initialize SD card
    ESP_ERROR_CHECK(sd_card_init());

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
