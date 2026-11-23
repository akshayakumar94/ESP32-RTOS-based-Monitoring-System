
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_PIN GPIO_NUM_18

void led_task(void *pvParameters)
{
    while (1)
    {
        gpio_set_level(LED_PIN, 1);
        printf("[LED TASK] LED ON\n");

        vTaskDelay(pdMS_TO_TICKS(500));

        gpio_set_level(LED_PIN, 0);
        printf("[LED TASK] LED OFF\n");

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void monitor_task(void *pvParameters)
{
    while (1)
    {
        printf("[MONITOR TASK] System is running...\n");

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

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

    xTaskCreate(
        led_task,
        "LED_Task",
        2048,
        NULL,
        2,// task with priority 2 will run before task with priority 1
        NULL
    );

    xTaskCreate(
        monitor_task,
        "Monitor_Task",
        2048,
        NULL,
        1,//low priority task
        NULL
    );
}
