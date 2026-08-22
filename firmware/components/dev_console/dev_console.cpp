#include "dev_console.h"
#include "tasks.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace DevConsole
{

namespace
{

constexpr TickType_t IDLE_POLL_MS = 50;
constexpr size_t CONSOLE_LINE_MAX = 48;

static LineHandler m_handler;

void consoleTask(void *arg)
{
    (void)arg;
    char line[CONSOLE_LINE_MAX];
    size_t len = 0;

    for (;;)
    {
        const int c = getchar();
        if (EOF == c)
        {
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
            continue;
        }
        if ('\n' == c || '\r' == c)
        {
            if (0 != len)
            {
                line[len] = '\0';
                len = 0;
                if (nullptr != m_handler)
                {
                    m_handler(line);
                }
            }
            continue;
        }
        if (len < CONSOLE_LINE_MAX - 1)
        {
            line[len++] = static_cast<char>(std::tolower(c));
        }
    }
}

} // namespace

esp_err_t start(const LineHandler handler)
{
    m_handler = handler;
    const BaseType_t ok = Tasks::create(Tasks::CONSOLE, consoleTask, nullptr);
    return (pdPASS == ok) ? ESP_OK : ESP_FAIL;
}

} // namespace DevConsole
