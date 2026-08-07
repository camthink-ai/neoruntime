#include "sys_config.h"
#include "cmd_console.h"
#include "host_link_app.h"
#include "app.h"
#include "bsp_ctrl.h"
#include "ota_module.h"

void app_task(void *arg)
{
    uint32_t t0 = osKernelGetTickCount();
    uint8_t marked = 0u;

    osDelay(100);
    bsp_ctrl_set_ir_cut(0); // disable IR-CUT
    for (;;) {
        HAL_GPIO_TogglePin(GPIOD, SYS_LED_Pin);
        if (!marked) {
            uint32_t now = osKernelGetTickCount();
            if ((now - t0) >= 5000u) {
                (void)ota_module_app_mark_verified();
                marked = 1u;
            }
        }
        osDelay(100);
    }
}

void app_init(void)
{
    if (bsp_ctrl_init() != SYS_OK) {
        WIC_LOGE("[app_init] Failed to initialize bsp_ctrl!");
        return;
    }
    if (cmd_console_init() != SYS_OK) {
        WIC_LOGE("[app_init] Failed to initialize cmd_console!");
        return;
    }
    if (host_link_app_init() != SYS_OK) {
        WIC_LOGE("[app_init] Failed to initialize host_link_app!");
        return;
    }
    if (xTaskCreate(app_task, APP_TASK_NAME, APP_TASK_STACK_SIZE, NULL, APP_TASK_PRIORITY, NULL) != pdPASS) {
        WIC_LOGE("[app_init] Failed to create task!");
        return;
    }
    WIC_LOGD("[app_init] ok! version: %s.", APP_VERSION);
}
