// Standard includes
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Scheduler includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <altera_avalon_pio_regs.h>

SemaphoreHandle_t xSemaphore;

void main() {
    xSemaphore = xSemaphoreCreateBinary();

    xTaskCreate(TaskFunction, "Task", 1000, NULL, 1, NULL);
    // Enable the interrupt here
    vTaskStartScheduler();
}

void TaskFunction(void *pvParameters) {
    while (1) {
        if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE) {
            // ISR has signaled the task
            // Do something in response
        }
    }
}

void ISR_Handler(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xSemaphore, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}