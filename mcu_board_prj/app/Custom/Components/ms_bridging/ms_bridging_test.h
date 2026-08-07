#ifndef __MS_BRIDGING_TEST_H
#define __MS_BRIDGING_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ms_bridging.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"

// Test configuration
#define TEST_QUEUE_SIZE                 (4)
#define TEST_TASK_STACK_SIZE            (2048)
#define TEST_TASK_PRIORITY              (osPriorityNormal)
#define TEST_RUN_TIME_SEC               (30)
#define TEST_REQUEST_INTERVAL_MS        (1000)
#define TEST_EVENT_INTERVAL_MS          (2000)

// Test statistics
typedef struct {
    uint32_t requests_sent;
    uint32_t requests_received;
    uint32_t responses_sent;
    uint32_t responses_received;
    uint32_t events_sent;
    uint32_t events_received;
    uint32_t acks_sent;
    uint32_t acks_received;
    uint32_t errors;
    uint32_t timeouts;
} test_stats_t;

// Test context
typedef struct {
    ms_bridging_handler_t *master_handler;
    ms_bridging_handler_t *slave_handler;
    osMessageQueueId_t master_to_slave_queue;
    osMessageQueueId_t slave_to_master_queue;
    test_stats_t master_stats;
    test_stats_t slave_stats;
    uint8_t test_running;
    osThreadId_t master_task_id;
    osThreadId_t slave_task_id;
    osThreadId_t master_polling_task_id;
    osThreadId_t slave_polling_task_id;
    osThreadId_t comm_relay_task_id;
    osThreadId_t monitor_task_id;
} test_context_t;

// Virtual communication message
typedef struct {
    uint8_t data[MS_BR_BUF_MAX_SIZE];
    uint16_t len;
} virtual_msg_t;

// Test function declarations
/// @brief Initialize MS bridging test
/// @return 0 on success, -1 on failure
int ms_bridging_test_init(void);

/// @brief Cleanup MS bridging test
void ms_bridging_test_cleanup(void);

/// @brief Run MS bridging test
/// @return 0 on success, -1 on failure
int ms_bridging_test_run(void);

/// @brief Get test statistics
/// @param master_stats pointer to master statistics
/// @param slave_stats pointer to slave statistics
void ms_bridging_test_get_stats(test_stats_t *master_stats, test_stats_t *slave_stats);

/// @brief Stop the test
void ms_bridging_test_stop(void);

/// @brief Check if test is running
/// @return 1 if running, 0 if stopped
uint8_t ms_bridging_test_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* __MS_BRIDGING_TEST_H */
