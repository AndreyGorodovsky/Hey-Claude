/*
 * Console command for memory headroom.
 *
 *   mem         free memory of both kinds, the least that has ever been
 *               free, and how much of its stack each of the firmware's
 *               tasks has never used
 *
 * Two things can run out on this chip without warning until the crash.
 *
 * The heap is the memory handed out while the firmware runs. There are two:
 * internal RAM, about half a megabyte, fast and needed by WiFi and by
 * anything interrupts touch; and PSRAM, 8 MB, for large buffers. The
 * "lowest ever" figure is the one to watch: it shows the worst moment since
 * boot, which a reading taken at a quiet time would miss.
 *
 * A stack is the private working memory of one task, fixed in size when
 * the task is created. A task that needs more than it was given writes
 * past the end and corrupts whatever lies there. FreeRTOS remembers how
 * close each task has come: the figure printed is the part of the stack
 * that has never been used. A few hundred bytes or less means the stack is
 * too small.
 *
 * A diagnostic aid, kept apart so that it can be taken out again: delete
 * this file and mem_cmd.h, the #include and the mem_cmd_register() call in
 * console.c, and the "mem_cmd.c" entry in this directory's CMakeLists.txt.
 *
 * Context: the handler runs in the console task.
 */
#include "mem_cmd.h"

#include <stdio.h>
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mem_cmd";

/* The tasks this firmware creates, by the names they were created with in
 * their components; a task added or renamed there has to be added here by
 * hand. "server_link_ws" exists only while there is a connection, or an attempt at
 * one, and can end between being found and being read: on a diagnostic
 * command, a wrong figure for it once in a while is accepted. */
static const char *const TASKS[] = {
    "capture", "wakeword", "player", "upload", "state_machine", "server_link",
    "server_link_ws", "display",
};

static void print_heap(const char *name, uint32_t caps)
{
    printf("%-9s free %7u bytes, lowest ever %7u, largest block %7u\n", name,
           (unsigned)heap_caps_get_free_size(caps),
           (unsigned)heap_caps_get_minimum_free_size(caps),
           (unsigned)heap_caps_get_largest_free_block(caps));
}

static int cmd_mem(int argc, char **argv)
{
    print_heap("internal", MALLOC_CAP_INTERNAL);
    print_heap("PSRAM", MALLOC_CAP_SPIRAM);
    printf("Stack never used, in bytes:\n");
    for (size_t i = 0; i < sizeof(TASKS) / sizeof(TASKS[0]); i++) {
        /* Looks a task up by name; NULL if there is none by that name now */
        TaskHandle_t task = xTaskGetHandle(TASKS[i]);
        if (task == NULL) {
            printf("  %-14s (not running)\n", TASKS[i]);
        } else {
            printf("  %-14s %5u\n", TASKS[i], (unsigned)uxTaskGetStackHighWaterMark(task));
        }
    }
    return 0;
}

esp_err_t mem_cmd_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "mem",
        .help = "Show free memory and how much stack each task has left",
        .func = cmd_mem,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "mem");
    return ESP_OK;
}
