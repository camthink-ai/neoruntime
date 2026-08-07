#ifndef __APP_H
#define __APP_H

#ifdef __cplusplus
extern "C" {
#endif

#define APP_TASK_NAME               "app"
#define APP_TASK_STACK_SIZE         (1536)
#define APP_TASK_PRIORITY           (2)

void app_init(void);

#ifdef __cplusplus
}
#endif
#endif /* __APP_H */
