#include "ota_host.h"

#include "ota_module.h"
#include "sys_config.h"

#include <stdint.h>
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

static TaskHandle_t s_reboot_task;

static void ota_reboot_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) > 0u) {
            /* Give UART DMA a tiny window to flush response. */
            osDelay(50);
            NVIC_SystemReset();
        }
    }
}

int ota_host_init(void)
{
    int rc = ota_module_init(&ota_module_platform_config);
    if (rc != OTA_MODULE_ERR_OK) {
        return rc;
    }

    if (s_reboot_task == NULL) {
        if (xTaskCreate(ota_reboot_task_fn, "ota_reboot", 512u, NULL, tskIDLE_PRIORITY + 1u, &s_reboot_task) != pdPASS) {
            s_reboot_task = NULL;
            return SYS_ERR_NO_MEM;
        }
    }

    return SYS_OK;
}

static void ota_host_reply_status(host_link_handler_t *h, host_link_frame_t *f, int32_t status)
{
    host_link_status_t s = {.status = status};
    (void)host_link_response(h, f, &s, sizeof(s));
}

static int ota_host_cmd_enter_boot(host_link_handler_t *h, host_link_frame_t *f)
{
    /* Single-slot XIP: APP does not perform OTA download/write.
     * Only set a bootloader tag and reboot; boot handles Ymodem update. */
    int rc = ota_module_set_boot_ymodem_flag(1u);
    ota_host_reply_status(h, f, (rc == OTA_MODULE_ERR_OK) ? SYS_OK : rc);

    if (s_reboot_task != NULL) {
        (void)xTaskNotifyGive(s_reboot_task);
    }
    return 1;
}

static int ota_host_cmd_reboot(host_link_handler_t *h, host_link_frame_t *f)
{
    ota_host_reply_status(h, f, SYS_OK);
    if (s_reboot_task != NULL) {
        (void)xTaskNotifyGive(s_reboot_task);
    }
    return 1;
}

int ota_host_handle_request(host_link_handler_t *h, host_link_frame_t *f)
{
    switch ((host_link_cmd_t)f->header.cmd) {
    case HOST_LINK_CMD_OTA_ENTER_BOOT:
        return ota_host_cmd_enter_boot(h, f);
    case HOST_LINK_CMD_REBOOT:
        return ota_host_cmd_reboot(h, f);
    default:
        return 0;
    }
}

