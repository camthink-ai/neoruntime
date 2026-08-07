#ifndef __CMD_CONSOLE_H__
#define __CMD_CONSOLE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sys_config.h"

#define CMD_CONSOLE_TASK_NAME           "console_task"
#define CMD_CONSOLE_TASK_STACK_SIZE     (2048)
#define CMD_CONSOLE_TASK_PRIORITY       (1)

int cmd_console_init(void);
int cmd_console_putc(char c);
int cmd_console_puts(const char *s);

#ifdef __cplusplus
}
#endif
#endif /* __CMD_CONSOLE_H__ */
