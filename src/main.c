
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_random.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "secrets.h"

// =====================================================
// GPIO AND SYSTEM CONFIGURATION
// =====================================================

#define LED_GPIO            GPIO_NUM_18
#define DHT_GPIO            GPIO_NUM_4

#define I2C_SDA_GPIO        GPIO_NUM_21
#define I2C_SCL_GPIO        GPIO_NUM_22

#define MAX30102_ADDR       0x57

#define WIFI_CONNECTED_BIT  BIT0

#define SAMPLE_WINDOW       100

#define MAX_RETRY_MS        5000
#define DHT_INTERVAL_MS     2500
#define BLYNK_INTERVAL_MS   5000

static const char *TAG = "ESP32_RTOS_MONITOR";

// =====================================================
// MAX30102 REGISTERS
// =====================================================

#define REG_FIFO_WR_PTR     0x04
#define REG_OVF_COUNTER     0x05
#define REG_FIFO_RD_PTR     0x06
#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C
#define REG_LED2_PA         0x0D
#define REG_PART_ID         0xFF

// =====================================================
// FALLBACK VALUES
// =====================================================

// These values keep the dashboard populated when a
// sensor is unavailable. V4 identifies fallback mode.

#define DEFAULT_TEMP        27.0f
#define DEFAULT_HUMIDITY    55.0f

#define DEFAULT_BPM         72.0f
#define DEFAULT_SPO2        98.0f

// =====================================================
// TELEMETRY
// =====================================================

typedef struct {
    float temperature;
    float humidity;
    float bpm;
    float spo2;

    bool dht_real;
    bool max_ok;
    bool finger_detected;

    char status[40];

} telemetry_t;

static telemetry_t telemetry = {
    .temperature = DEFAULT_TEMP,
    .humidity = DEFAULT_HUMIDITY,
    .bpm = DEFAULT_BPM,
    .spo2 = DEFAULT_SPO2,
    .dht_real = false,
    .max_ok = false,
    .finger_detected = false,
    .status = "STARTING"
};

// =====================================================
// GLOBAL HANDLES
// =====================================================

static SemaphoreHandle_t telemetry_mutex = NULL;
static EventGroupHandle_t wifi_event_group = NULL;

static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t max_device = NULL;

// =====================================================
// TELEMETRY MANAGEMENT
// =====================================================

static telemetry_t get_telemetry(void)
{
    telemetry_t copy = {0};

    if (xSemaphoreTake(telemetry_mutex,
                       pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = telemetry;
        xSemaphoreGive(telemetry_mutex);
    }

    return copy;
}

static void update_dht_data(float temp,
                            float hum,
                            bool real)
{
    if (xSemaphoreTake(telemetry_mutex,
                       pdMS_TO_TICKS(100)) == pdTRUE) {
        telemetry.temperature = temp;
        telemetry.humidity = hum;
        telemetry.dht_real = real;

        xSemaphoreGive(telemetry_mutex);
    }
}

static void update_max_data(float bpm,
                            float spo2,
                            bool finger,
                            bool max_ok)
{
    if (xSemaphoreTake(telemetry_mutex,
                       pdMS_TO_TICKS(100)) == pdTRUE) {
        telemetry.bpm = bpm;
        telemetry.spo2 = spo2;
        telemetry.finger_detected = finger;
        telemetry.max_ok = max_ok;

        xSemaphoreGive(telemetry_mutex);
    }
}

static void update_status(void)
{
    if (xSemaphoreTake(telemetry_mutex,
                       pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (!telemetry.dht_real && !telemetry.max_ok) {
        snprintf(telemetry.status,
                 sizeof(telemetry.status),
                 "BOTH_FALLBACK");
    }
    else if (!telemetry.dht_real && telemetry.max_ok &&
             telemetry.finger_detected) {
        snprintf(telemetry.status,
                 sizeof(telemetry.status),
                 "TEMP_FALLBACK_MAX_OK");
    }
    else if (!telemetry.dht_real && telemetry.max_ok) {
        snprintf(telemetry.status,
                 sizeof(telemetry.status),
                 "TEMP_FALLBACK_WAIT_FINGER");
    }
    else if (telemetry.dht_real && !telemetry.max_ok) {
        snprintf(telemetry.status,
                 sizeof(telemetry.status),
                 "MAX_FALLBACK");
    }
    else if (telemetry.dht_real && telemetry.max_ok &&
             telemetry.finger_detected) {
        snprintf(telemetry.status,
                 sizeof(telemetry.status),
                 "ALL_SENSORS_OK");
    }
    else {
        snprintf(telemetry.status,
                 sizeof(telemetry.status),
                 "WAITING_FOR_FINGER");
    }

    xSemaphoreGive(telemetry_mutex);
}

// =====================================================
// LED INITIALIZATION
// =====================================================

static void led_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(LED_GPIO, 0);
}

// =====================================================
// DHT11 DRIVER
// =====================================================

static bool wait_for_level(int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();

    while (gpio_get_level(DHT_GPIO) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return false;
        }
    }

    return true;
}

static bool dht11_read(float *temperature,
                       float *humidity)
{
    uint8_t data[5] = {0};

    gpio_set_direction(DHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_GPIO, 0);

    vTaskDelay(pdMS_TO_TICKS(20));

    gpio_set_level(DHT_GPIO, 1);
    esp_rom_delay_us(30);

    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DHT_GPIO, GPIO_PULLUP_ONLY);

    if (!wait_for_level(0, 120)) return false;
    if (!wait_for_level(1, 120)) return false;
    if (!wait_for_level(0, 120)) return false;

    for (int i = 0; i < 40; i++) {
        if (!wait_for_level(1, 100)) {
            return false;
        }

        int64_t pulse_start = esp_timer_get_time();

        if (!wait_for_level(0, 120)) {
            return false;
        }

        int pulse_width =
            (int)(esp_timer_get_time() - pulse_start);

        data[i / 8] <<= 1;

        if (pulse_width > 40) {
            data[i / 8] |= 1;
        }
    }

    uint8_t checksum =
        (uint8_t)(data[0] + data[1] +
                  data[2] + data[3]);

    if (checksum != data[4]) {
        return false;
    }

    *humidity = data[0] + data[1] * 0.1f;
    *temperature = data[2] + data[3] * 0.1f;

    return true;
}

// =====================================================
// WIFI INITIALIZATION
// =====================================================

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        xEventGroupClearBits(wifi_event_group,
                             WIFI_CONNECTED_BIT);

        ESP_LOGW(TAG, "WiFi disconnected. Reconnecting...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ESP_LOGI(TAG, "WiFi connected!");

        xEventGroupSetBits(wifi_event_group,
                           WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        abort();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL));

    wifi_config_t wifi_config = {0};

    strncpy((char *)wifi_config.sta.ssid,
            WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);

    strncpy((char *)wifi_config.sta.password,
            WIFI_PASSWORD,
            sizeof(wifi_config.sta.password) - 1);

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());
}

// =====================================================
// MAX30102 I2C LOW-LEVEL DRIVER
// =====================================================

static esp_err_t max_write_reg(uint8_t reg,
                               uint8_t value)
{
    if (max_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[2] = {reg, value};

    return i2c_master_transmit(
        max_device,
        data,
        sizeof(data),
        100);
}

static esp_err_t max_read_reg(uint8_t reg,
                              uint8_t *value)
{
    if (max_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(
        max_device,
        &reg,
        1,
        value,
        1,
        100);
}

// =====================================================
// MAX30102 INITIALIZATION
// =====================================================

static bool max30102_init(void)
{
    esp_err_t err;

    // Create I2C bus only once.
    if (i2c_bus == NULL) {

        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = I2C_SDA_GPIO,
            .scl_io_num = I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true
        };

        err = i2c_new_master_bus(
            &bus_config,
            &i2c_bus);

        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "I2C bus initialization failed: %s",
                     esp_err_to_name(err));
            return false;
        }

        ESP_LOGI(TAG, "I2C bus initialized at GPIO21/GPIO22");
    }

    // Add MAX30102 device only once.
    if (max_device == NULL) {

        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = MAX30102_ADDR,
            .scl_speed_hz = 100000
        };

        err = i2c_master_bus_add_device(
            i2c_bus,
            &device_config,
            &max_device);

        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "I2C device setup failed: %s",
                     esp_err_to_name(err));
            return false;
        }
    }

    // Check device identity.
    uint8_t part_id = 0;

    err = max_read_reg(REG_PART_ID, &part_id);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "MAX30102 communication failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "MAX30102 PART ID: 0x%02X", part_id);

    if (part_id != 0x15) {
        ESP_LOGE(TAG, "Unexpected MAX30102 PART ID");
        return false;
    }

    // Reset sensor.
    err = max_write_reg(REG_MODE_CONFIG, 0x40);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 reset failed");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    // Reset FIFO pointers.
    if (max_write_reg(REG_FIFO_WR_PTR, 0x00) != ESP_OK ||
        max_write_reg(REG_OVF_COUNTER, 0x00) != ESP_OK ||
        max_write_reg(REG_FIFO_RD_PTR, 0x00) != ESP_OK) {

        ESP_LOGE(TAG, "MAX30102 FIFO reset failed");
        return false;
    }

    // FIFO configuration.
    if (max_write_reg(REG_FIFO_CONFIG, 0x4F) != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 FIFO configuration failed");
        return false;
    }

    // SpO2 mode: Red and IR LEDs.
    if (max_write_reg(REG_MODE_CONFIG, 0x03) != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 mode configuration failed");
        return false;
    }

    // ADC range, sample rate and pulse width.
    if (max_write_reg(REG_SPO2_CONFIG, 0x27) != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 SpO2 configuration failed");
        return false;
    }

    // Red LED current.
    if (max_write_reg(REG_LED1_PA, 0x24) != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 red LED configuration failed");
        return false;
    }

    // IR LED current.
    if (max_write_reg(REG_LED2_PA, 0x24) != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 IR LED configuration failed");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "MAX30102 initialized successfully");

    return true;
}

// =====================================================
// MAX30102 FIFO SAMPLE READING
// =====================================================

// ESP_OK + sample_ready=false means FIFO is empty.
// Any other error means I2C communication failed.

static esp_err_t max_read_sample(uint32_t *red,
                                 uint32_t *ir,
                                 bool *sample_ready)
{
    uint8_t write_ptr = 0;
    uint8_t read_ptr = 0;

    *sample_ready = false;

    esp_err_t err = max_read_reg(
        REG_FIFO_WR_PTR,
        &write_ptr);

    if (err != ESP_OK) {
        return err;
    }

    err = max_read_reg(
        REG_FIFO_RD_PTR,
        &read_ptr);

    if (err != ESP_OK) {
        return err;
    }

    write_ptr &= 0x1F;
    read_ptr &= 0x1F;

    if (write_ptr == read_ptr) {
        return ESP_OK;
    }

    uint8_t reg = REG_FIFO_DATA;
    uint8_t raw[6] = {0};

    err = i2c_master_transmit_receive(
        max_device,
        &reg,
        1,
        raw,
        6,
        100);

    if (err != ESP_OK) {
        return err;
    }

    *red = (((uint32_t)raw[0] << 16) |
            ((uint32_t)raw[1] << 8) |
            raw[2]) & 0x3FFFF;

    *ir = (((uint32_t)raw[3] << 16) |
           ((uint32_t)raw[4] << 8) |
           raw[5]) & 0x3FFFF;

    *sample_ready = true;

    return ESP_OK;
}

// =====================================================
// EDUCATIONAL SPO2 ESTIMATION
// =====================================================

static float calculate_spo2(uint32_t *red_samples,
                            uint32_t *ir_samples,
                            int count)
{
    if (count < SAMPLE_WINDOW) {
        return 0.0f;
    }

    double red_mean = 0;
    double ir_mean = 0;

    for (int i = 0; i < count; i++) {
        red_mean += red_samples[i];
        ir_mean += ir_samples[i];
    }

    red_mean /= count;
    ir_mean /= count;

    if (red_mean < 1 || ir_mean < 1) {
        return 0.0f;
    }

    double red_variance = 0;
    double ir_variance = 0;

    for (int i = 0; i < count; i++) {
        double rd = red_samples[i] - red_mean;
        double id = ir_samples[i] - ir_mean;

        red_variance += rd * rd;
        ir_variance += id * id;
    }

    double red_ac = sqrt(red_variance / count);
    double ir_ac = sqrt(ir_variance / count);

    if (ir_ac < 1 || red_ac < 1) {
        return 0.0f;
    }

    double ratio =
        (red_ac / red_mean) / (ir_ac / ir_mean);

    // Approximate educational mapping only.
    double spo2 = 110.0 - (25.0 * ratio);

    if (spo2 > 100) spo2 = 100;
    if (spo2 < 70) spo2 = 0;

    return (float)spo2;
}

// =====================================================
// FALLBACK VALUE GENERATORS
// =====================================================

static float clamp_float(float value,
                         float minimum,
                         float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void generate_dht_fallback(float *temp,
                                  float *humidity)
{
    static float fallback_temp = DEFAULT_TEMP;
    static float fallback_humidity = DEFAULT_HUMIDITY;

    int temp_step = (int)(esp_random() % 7) - 3;
    int hum_step = (int)(esp_random() % 7) - 3;

    fallback_temp += temp_step * 0.1f;
    fallback_humidity += hum_step * 0.5f;

    fallback_temp = clamp_float(
        fallback_temp, 25.0f, 30.0f);

    fallback_humidity = clamp_float(
        fallback_humidity, 45.0f, 65.0f);

    *temp = fallback_temp;
    *humidity = fallback_humidity;
}

static void generate_max_fallback(float *bpm,
                                  float *spo2)
{
    static float fallback_bpm = DEFAULT_BPM;
    static float fallback_spo2 = DEFAULT_SPO2;

    int bpm_step = (int)(esp_random() % 5) - 2;
    int spo2_step = (int)(esp_random() % 3) - 1;

    fallback_bpm += (float)bpm_step;
    fallback_spo2 += (float)spo2_step * 0.1f;

    fallback_bpm = clamp_float(
        fallback_bpm, 68.0f, 82.0f);

    fallback_spo2 = clamp_float(
        fallback_spo2, 96.0f, 99.0f);

    *bpm = fallback_bpm;
    *spo2 = fallback_spo2;
}

// =====================================================
// TASK 1: DHT11 TASK
// =====================================================

static void dht11_task(void *arg)
{
    ESP_LOGI(TAG, "DHT11 task started");

    while (1) {

        float temp = 0;
        float hum = 0;

        bool success = dht11_read(&temp, &hum);

        if (success &&
            temp >= 0 && temp <= 60 &&
            hum >= 0 && hum <= 100) {

            update_dht_data(temp, hum, true);

            ESP_LOGI(TAG,
                     "Temperature = %.1f C | Humidity = %.1f %%",
                     temp, hum);
        }
        else {

            generate_dht_fallback(&temp, &hum);

            update_dht_data(temp, hum, false);

            ESP_LOGW(TAG,
                     "DHT11 unavailable. Using fallback readings: "
                     "T=%.1f C H=%.1f %%",
                     temp, hum);
        }

        update_status();

        vTaskDelay(pdMS_TO_TICKS(DHT_INTERVAL_MS));
    }
}

// =====================================================
// TASK 2: MAX30102 TASK
// =====================================================

static void max30102_task(void *arg)
{
    ESP_LOGI(TAG, "MAX30102 task started");

    uint32_t red_samples[SAMPLE_WINDOW] = {0};
    uint32_t ir_samples[SAMPLE_WINDOW] = {0};

    int sample_count = 0;
    int sample_index = 0;

    float bpm = 0;
    float spo2 = 0;

    int64_t last_beat_time = 0;

    bool previous_above_threshold = false;
    bool sensor_initialized = false;

    int communication_errors = 0;
    int log_counter = 0;

    while (1) {

        // Retry initialization if sensor is unavailable.
        if (!sensor_initialized) {

            ESP_LOGI(TAG, "Initializing MAX30102...");

            if (!max30102_init()) {

                generate_max_fallback(&bpm, &spo2);

                update_max_data(
                    bpm,
                    spo2,
                    false,
                    false);

                update_status();

                ESP_LOGW(TAG,
                         "MAX30102 unavailable. "
                         "Dashboard fallback: BPM=%.1f SpO2=%.1f",
                         bpm, spo2);

                vTaskDelay(pdMS_TO_TICKS(MAX_RETRY_MS));

                continue;
            }

            sensor_initialized = true;
            communication_errors = 0;

            sample_count = 0;
            sample_index = 0;

            bpm = 0;
            spo2 = 0;

            last_beat_time = 0;
            previous_above_threshold = false;

            update_max_data(0, 0, false, true);
            update_status();
        }

        uint32_t red = 0;
        uint32_t ir = 0;
        bool sample_ready = false;

        esp_err_t err = max_read_sample(
            &red,
            &ir,
            &sample_ready);

        if (err != ESP_OK) {

            communication_errors++;

            if (communication_errors >= 5) {

                ESP_LOGE(TAG,
                         "MAX30102 communication lost: %s",
                         esp_err_to_name(err));

                sensor_initialized = false;

                sample_count = 0;
                sample_index = 0;

                generate_max_fallback(&bpm, &spo2);

                update_max_data(
                    bpm,
                    spo2,
                    false,
                    false);

                update_status();

                communication_errors = 0;

                vTaskDelay(pdMS_TO_TICKS(MAX_RETRY_MS));

                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        communication_errors = 0;

        // No new FIFO sample yet.
        if (!sample_ready) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Basic finger detection.
        bool finger = (ir > 50000);

        if (!finger) {

            bpm = 0;
            spo2 = 0;

            sample_count = 0;
            sample_index = 0;

            previous_above_threshold = false;
            last_beat_time = 0;

            update_max_data(0, 0, false, true);
            update_status();

            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        red_samples[sample_index] = red;
        ir_samples[sample_index] = ir;

        sample_index = (sample_index + 1) % SAMPLE_WINDOW;

        if (sample_count < SAMPLE_WINDOW) {
            sample_count++;
        }

        uint32_t min_ir = UINT32_MAX;
        uint32_t max_ir = 0;

        for (int i = 0; i < sample_count; i++) {

            if (ir_samples[i] < min_ir) {
                min_ir = ir_samples[i];
            }

            if (ir_samples[i] > max_ir) {
                max_ir = ir_samples[i];
            }
        }

        uint32_t threshold =
            min_ir + (uint32_t)((max_ir - min_ir) * 0.55f);

        bool above_threshold = (ir > threshold);

        int64_t now = esp_timer_get_time();

        if (above_threshold && !previous_above_threshold) {

            if (last_beat_time != 0) {

                float interval_seconds =
                    (now - last_beat_time) / 1000000.0f;

                if (interval_seconds > 0.33f &&
                    interval_seconds < 1.5f) {

                    float new_bpm =
                        60.0f / interval_seconds;

                    if (new_bpm >= 40 && new_bpm <= 180) {

                        if (bpm == 0) {
                            bpm = new_bpm;
                        }
                        else {
                            bpm = (0.7f * bpm) +
                                  (0.3f * new_bpm);
                        }
                    }
                }
            }

            last_beat_time = now;
        }

        previous_above_threshold = above_threshold;

        if (sample_count == SAMPLE_WINDOW) {

            float calculated_spo2 = calculate_spo2(
                red_samples,
                ir_samples,
                SAMPLE_WINDOW);

            if (calculated_spo2 >= 70 &&
                calculated_spo2 <= 100) {
                spo2 = calculated_spo2;
            }
        }

        update_max_data(bpm, spo2, true, true);
        update_status();

        if (++log_counter >= 100) {

            log_counter = 0;

            ESP_LOGI(TAG,
                     "MAX30102: IR=%lu RED=%lu BPM=%.1f SpO2=%.1f%%",
                     (unsigned long)ir,
                     (unsigned long)red,
                     bpm,
                     spo2);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// =====================================================
// TASK 3: BLYNK COMMUNICATION
// =====================================================

static void send_to_blynk(telemetry_t data)
{
    char url[512];

    snprintf(url,
             sizeof(url),
             "https://blynk.cloud/external/api/update"
             "?token=%s&v0=%.1f&v1=%.1f&v2=%.1f&v3=%.1f&v4=%s",
             BLYNK_TOKEN,
             data.temperature,
             data.humidity,
             data.bpm,
             data.spo2,
             data.status);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP client initialization failed");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {

        int status_code =
            esp_http_client_get_status_code(client);

        if (status_code == 200) {
            ESP_LOGI(TAG, "Blynk update successful");
        }
        else {
            ESP_LOGE(TAG,
                     "Blynk HTTP status: %d",
                     status_code);
        }
    }
    else {
        ESP_LOGE(TAG,
                 "Blynk update failed: %s",
                 esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void communication_task(void *arg)
{
    ESP_LOGI(TAG, "Communication task started");

    while (1) {

        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY);

        telemetry_t data = get_telemetry();

        ESP_LOGI(TAG,
                 "Uploading: T=%.1f H=%.1f BPM=%.1f "
                 "SpO2=%.1f Status=%s",
                 data.temperature,
                 data.humidity,
                 data.bpm,
                 data.spo2,
                 data.status);

        send_to_blynk(data);

        vTaskDelay(pdMS_TO_TICKS(BLYNK_INTERVAL_MS));
    }
}

// =====================================================
// TASK 4: LED TASK
// =====================================================

static void led_task(void *arg)
{
    ESP_LOGI(TAG, "LED status task started");

    int led_state = 0;

    while (1) {

        led_state = !led_state;

        gpio_set_level(LED_GPIO, led_state);

        ESP_LOGI(TAG,
                 "LED %s",
                 led_state ? "ON" : "OFF");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// =====================================================
// MAIN APPLICATION
// =====================================================

void app_main(void)
{
    ESP_LOGI(TAG,
             "ESP32 FreeRTOS Health Monitor booting");

    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Create telemetry mutex.
    telemetry_mutex = xSemaphoreCreateMutex();

    if (telemetry_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create telemetry mutex");
        abort();
    }

    // Initialize LED.
    led_init();

    // Initialize WiFi.
    wifi_init();

    ESP_LOGI(TAG, "Starting FreeRTOS application tasks");

    BaseType_t result;

    result = xTaskCreate(
        dht11_task,
        "DHT11_Task",
        4096,
        NULL,
        5,
        NULL);

    configASSERT(result == pdPASS);

    result = xTaskCreate(
        max30102_task,
        "MAX30102_Task",
        6144,
        NULL,
        6,
        NULL);

    configASSERT(result == pdPASS);

    result = xTaskCreate(
        communication_task,
        "Communication_Task",
        6144,
        NULL,
        4,
        NULL);

    configASSERT(result == pdPASS);

    result = xTaskCreate(
        led_task,
        "LED_Task",
        2048,
        NULL,
        3,
        NULL);

    configASSERT(result == pdPASS);

    ESP_LOGI(TAG,
             "All application tasks created successfully");
}
