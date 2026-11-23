
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_PIN GPIO_NUM_18

void app_main(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    while (1)
    {
        ESP_ERROR_CHECK(gpio_set_level(LED_PIN, 1));
        printf("LED ON\n");

        vTaskDelay(pdMS_TO_TICKS(500));

        ESP_ERROR_CHECK(gpio_set_level(LED_PIN, 0));
        printf("LED OFF\n");

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

