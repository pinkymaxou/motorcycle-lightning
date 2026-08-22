/* Every FreeRTOS task in the firmware, described in one place.
 *
 * Core affinity and priority are safety properties here, not tuning knobs:
 * the render task must never be starved by the radio, and every RMT channel
 * has to be created on the lighting core. Keeping the four specs side by side
 * is what makes those relations checkable at a glance. */
#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace Tasks
{

/* Lighting owns core 1: the render task and every RMT channel live there.
 * WiFi, httpd and the console stay on core 0 so a network burst cannot
 * delay a frame. */
constexpr BaseType_t LIGHTING_CORE = 1;
constexpr BaseType_t SERVICE_CORE = 0;

struct TaskSpec
{
    const char *name;
    uint32_t    stack_bytes;
    UBaseType_t priority;
    BaseType_t  core;
};

/* ~75 FPS, the only task that must never be late. It subscribes to the task
 * watchdog, which panics (and resets) if a frame is ever missed for 5 s. */
constexpr TaskSpec RENDER = { "render", 6144, 10, LIGHTING_CORE };

/* Throwaway task: creates the strips' RMT channels on the lighting core and
 * latches them black, then deletes itself. Runs before anything else in the
 * boot sequence, so it outranks the console but not the render task. */
constexpr TaskSpec STRIP_INIT = { "strip_init", 3072, 5, LIGHTING_CORE };

/* Same, for the on-module status pixel. */
constexpr TaskSpec STATUS_LED_INIT = { "sled_init", 3072, 5, LIGHTING_CORE };

/* Serial console: diagnostics only, lowest of the lot. */
constexpr TaskSpec CONSOLE = { "console", 3072, 2, SERVICE_CORE };

/* The HTTP server's own task, created by esp_http_server from these values.
 * Only alive while the config WiFi is up. */
constexpr TaskSpec HTTPD = { "httpd", 8192, 5, SERVICE_CORE };

/* One call site for xTaskCreatePinnedToCore, so a task can never be started
 * with parameters that are not the ones written above. */
inline BaseType_t create(const TaskSpec &spec, const TaskFunction_t fn,
                         void *const arg, TaskHandle_t *const out = nullptr)
{
    return xTaskCreatePinnedToCore(fn, spec.name, spec.stack_bytes, arg,
                                   spec.priority, out, spec.core);
}

} // namespace Tasks
