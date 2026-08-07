#ifndef __NR_MICRO_SHELL_PORT_H__
#define __NR_MICRO_SHELL_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include "cmd_console.h"

/* Required configuration macros */
#define shell_putc(x) cmd_console_putc(x)
#define shell_puts(s) cmd_console_puts(s)

/* Optional configuration macros */
#define NR_SHELL_MAX_LINE_SZ            80  /* Maximum command line length */
#define NR_SHELL_PROMPT                 "ne503@mcu" /* Command prompt string */
#define NR_SHELL_MAX_PARAM_NUM          16  /* Maximum number of parameters per command */

#define NR_SHELL_AUTO_COMPLETE_SUPPORT      /* Enable command auto-completion feature */

#define NR_SHELL_HISTORY_CMD_SUPPORT        /* Enable command history feature */
#define NR_SHELL_HISTORY_CMD_NUM        10   /* Number of commands to keep in history */
#define NR_SHELL_HISTORY_CMD_SZ         64  /* Maximum size of each history command */

#ifdef __cplusplus
}
#endif
#endif /* __NR_MICRO_SHELL_PORT_H__ */
