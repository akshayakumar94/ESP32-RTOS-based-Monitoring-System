
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

SemaphoreHandle_t serial_mutex;

void task_one(void *pvParameters)
{
    while (1)
    {
        if (xSemaphoreTake(serial_mutex, portMAX_DELAY) == pdTRUE)
        {
            printf("\n[TASK 1] Starting operation\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            printf("[TASK 1] Operation completed\n");

            xSemaphoreGive(serial_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void task_two(void *pvParameters)
{
    while (1)
    {
        if (xSemaphoreTake(serial_mutex, portMAX_DELAY) == pdTRUE)
        {
            printf("\n[TASK 2] Starting operation\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            printf("[TASK 2] Operation completed\n");

            xSemaphoreGive(serial_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    serial_mutex = xSemaphoreCreateMutex();

    if (serial_mutex == NULL)
    {
        printf("Mutex creation failed!\n");
        return;
    }

    xTaskCreate(task_one, "Task_One", 2048, NULL, 2, NULL);
    xTaskCreate(task_two, "Task_Two", 2048, NULL, 2, NULL);
}
