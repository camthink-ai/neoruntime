#ifndef OTA_HOST_H
#define OTA_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "host_link.h"

int ota_host_init(void);

/**
 * Handle OTA-related host_link commands.
 *
 * NOTE: Single-slot XIP mode:
 *  - APP must NOT download/write firmware.
 *  - Only supports HOST_LINK_CMD_OTA_ENTER_BOOT (set tag + reset).
 *
 * @return 1 if handled, 0 if not an OTA command.
 */
int ota_host_handle_request(host_link_handler_t *h, host_link_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* OTA_HOST_H */

