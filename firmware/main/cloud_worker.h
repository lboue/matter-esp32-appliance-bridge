#pragma once

#include "esp_err.h"

/**
 * A single task that runs all outbound cloud calls (HomeConnect and SmartThings), in order.
 *
 * Matter command handlers run on the Matter task with the CHIP stack lock
 * held, so they must not block on HTTPS. They post a job here instead and
 * report the outcome back to the data model once the call completes.
 */

typedef void (*cloud_job_fn_t)(void *arg);

esp_err_t cloud_worker_start(void);

/**
 * Queue `fn(arg)` to run on the worker task. `arg` is owned by the job; the
 * job must free it. Returns ESP_ERR_NO_MEM if the queue is full, in which
 * case the caller still owns `arg`.
 */
esp_err_t cloud_worker_post(cloud_job_fn_t fn, void *arg);
