#include "cloud_worker.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "cloud_worker"

#define QUEUE_LENGTH 16
#define TASK_STACK 8192

typedef struct {
    cloud_job_fn_t fn;
    void *arg;
} job_t;

static QueueHandle_t s_queue;

static void worker_task(void *)
{
    job_t job;
    while (true) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE) {
            job.fn(job.arg);
        }
    }
}

esp_err_t cloud_worker_start(void)
{
    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(job_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(worker_task, "cloud_worker", TASK_STACK, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t cloud_worker_post(cloud_job_fn_t fn, void *arg)
{
    job_t job = {fn, arg};
    if (s_queue == NULL || xQueueSend(s_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Worker queue full; dropping job");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
