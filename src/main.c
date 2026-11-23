
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

QueueHandle_t data_queue;

void producer_task(void *pvParameters)
{
    int sensor_value = 0;

    while (1)
    {
        sensor_value += 10;

        if (xQueueSend(data_queue, &sensor_value, 0) == pdPASS)
        {
            printf("[PRODUCER] Sent: %d\n", sensor_value);
        }
        else
        {
            printf("[PRODUCER] Queue full!\n");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void consumer_task(void *pvParameters)
{
    int received_value;

    while (1)
    {
        if (xQueueReceive(data_queue, &received_value, portMAX_DELAY) == pdPASS)
        {
            printf("[CONSUMER] Received: %d\n", received_value);
        }
    }
}

void app_main(void)
{
    data_queue = xQueueCreate(5, sizeof(int));

    if (data_queue == NULL)
    {
        printf("Queue creation failed!\n");
        return;
    }

    xTaskCreate(producer_task, "Producer", 2048, NULL, 2, NULL);
    xTaskCreate(consumer_task, "Consumer", 2048, NULL, 2, NULL);
}
