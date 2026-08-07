#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include "ms_bridging_test.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "main.h"

static test_context_t g_test_ctx = {0};

// Master send function
static int master_send_func(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    virtual_msg_t msg = {0};
    
    if (len > MS_BR_BUF_MAX_SIZE) {
        return MS_BR_ERR_INVALID_SIZE;
    }
    
    memcpy(msg.data, buf, len);
    msg.len = len;
    
    osStatus_t status = osMessageQueuePut(g_test_ctx.master_to_slave_queue, &msg, 0, timeout_ms);
    return (status == osOK) ? MS_BR_OK : MS_BR_ERR_FAILED;
}

// Slave send function
static int slave_send_func(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    virtual_msg_t msg = {0};
    
    if (len > MS_BR_BUF_MAX_SIZE) {
        return MS_BR_ERR_INVALID_SIZE;
    }
    
    memcpy(msg.data, buf, len);
    msg.len = len;
    
    osStatus_t status = osMessageQueuePut(g_test_ctx.slave_to_master_queue, &msg, 0, timeout_ms);
    return (status == osOK) ? MS_BR_OK : MS_BR_ERR_FAILED;
}

// Master notification callback
static void master_notify_cb(void *handler, ms_bridging_frame_t *frame)
{
    test_context_t *ctx = &g_test_ctx;
    
    printf("[MASTER] Received notification: type=%d, cmd=%d, id=%d, len=%d\n", 
           frame->header.type, frame->header.cmd, frame->header.id, frame->header.len);
    
    if (frame->header.type == MS_BR_FRAME_TYPE_REQUEST) {
        ctx->master_stats.requests_received++;
        
        // Handle different request types
        switch (frame->header.cmd) {
            case MS_BR_FRAME_CMD_KEEPLIVE:
                printf("[MASTER] Handling keep alive request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->master_stats.responses_sent++;
                break;
                
            case MS_BR_FRAME_CMD_GET_TIME: {
                ms_bridging_time_t time = {
                    .year = 2024,
                    .month = 1,
                    .day = 15,
                    .week = 1,
                    .hour = 10,
                    .minute = 30,
                    .second = 45
                };
                printf("[MASTER] Handling get time request\n");
                ms_bridging_response(handler, frame, &time, sizeof(time));
                ctx->master_stats.responses_sent++;
                break;
            }
            
            case MS_BR_FRAME_CMD_SET_TIME:
                printf("[MASTER] Handling set time request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->master_stats.responses_sent++;
                break;
            
            case MS_BR_FRAME_CMD_PWR_CTRL: {
                printf("[MASTER] Handling power control request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->master_stats.responses_sent++;
                break;
            }
                
            case MS_BR_FRAME_CMD_PWR_STATUS: {
                uint32_t switch_bits = 0x12345678;
                printf("[MASTER] Handling power status request\n");
                ms_bridging_response(handler, frame, &switch_bits, sizeof(switch_bits));
                ctx->master_stats.responses_sent++;
                break;
            }

            case MS_BR_FRAME_CMD_KEY_VALUE:
                printf("[MASTER] Handling key value request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->master_stats.responses_sent++;
                break;
                
            case MS_BR_FRAME_CMD_PIR_VALUE:
                printf("[MASTER] Handling PIR value request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->master_stats.responses_sent++;
                break;

            default:
                printf("[MASTER] Unknown request command: %d\n", frame->header.cmd);
                break;
        }
    } else if (frame->header.type == MS_BR_FRAME_TYPE_EVENT) {
        ctx->master_stats.events_received++;
        
        // Handle different event types
        switch (frame->header.cmd) {
            case MS_BR_FRAME_CMD_KEY_VALUE:
                printf("[MASTER] Received key value event\n");
                ms_bridging_event_ack(handler, frame);
                ctx->master_stats.acks_sent++;
                break;
                
            case MS_BR_FRAME_CMD_PIR_VALUE:
                printf("[MASTER] Received PIR value event\n");
                ms_bridging_event_ack(handler, frame);
                ctx->master_stats.acks_sent++;
                break;
                
            default:
                printf("[MASTER] Unknown event command: %d\n", frame->header.cmd);
                break;
        }
    }
}

// Slave notification callback
static void slave_notify_cb(void *handler, ms_bridging_frame_t *frame)
{
    test_context_t *ctx = &g_test_ctx;
    
    printf("[SLAVE] Received notification: type=%d, cmd=%d, id=%d, len=%d\n", 
           frame->header.type, frame->header.cmd, frame->header.id, frame->header.len);
    
    if (frame->header.type == MS_BR_FRAME_TYPE_REQUEST) {
        ctx->slave_stats.requests_received++;
        
        // Handle different request types
        switch (frame->header.cmd) {
            case MS_BR_FRAME_CMD_KEEPLIVE:
                printf("[SLAVE] Handling keep alive request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->slave_stats.responses_sent++;
                break;
                
            case MS_BR_FRAME_CMD_GET_TIME: {
                ms_bridging_time_t time = {
                    .year = 2024,
                    .month = 1,
                    .day = 15,
                    .week = 1,
                    .hour = 12,
                    .minute = 45,
                    .second = 30
                };
                printf("[SLAVE] Handling get time request\n");
                ms_bridging_response(handler, frame, &time, sizeof(time));
                ctx->slave_stats.responses_sent++;
                break;
            }

            case MS_BR_FRAME_CMD_SET_TIME: {
                printf("[SLAVE] Handling set time request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->slave_stats.responses_sent++;
                break;
            }
            
            case MS_BR_FRAME_CMD_PWR_CTRL: {
                printf("[SLAVE] Handling power control request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->slave_stats.responses_sent++;
                break;
            }

            case MS_BR_FRAME_CMD_PWR_STATUS: {
                uint32_t switch_bits = 0x12345678;
                printf("[SLAVE] Handling power status request\n");
                ms_bridging_response(handler, frame, &switch_bits, sizeof(switch_bits));
                ctx->slave_stats.responses_sent++;
                break;
            }
            
            case MS_BR_FRAME_CMD_KEY_VALUE:
                printf("[SLAVE] Handling key value request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->slave_stats.responses_sent++;
                break;
                
            case MS_BR_FRAME_CMD_PIR_VALUE:
                printf("[SLAVE] Handling PIR value request\n");
                ms_bridging_response(handler, frame, NULL, 0);
                ctx->slave_stats.responses_sent++;
                break;
                
            default:
                printf("[SLAVE] Unknown request command: %d\n", frame->header.cmd);
                break;
        }
    } else if (frame->header.type == MS_BR_FRAME_TYPE_EVENT) {
        ctx->slave_stats.events_received++;
        
        // Handle different event types
        switch (frame->header.cmd) {
            case MS_BR_FRAME_CMD_KEY_VALUE:
                printf("[SLAVE] Received key value event\n");
                ms_bridging_event_ack(handler, frame);
                ctx->slave_stats.acks_sent++;
                break;
                
            case MS_BR_FRAME_CMD_PIR_VALUE:
                printf("[SLAVE] Received PIR value event\n");
                ms_bridging_event_ack(handler, frame);
                ctx->slave_stats.acks_sent++;
                break;
                
            default:
                printf("[SLAVE] Unknown event command: %d\n", frame->header.cmd);
                break;
        }
    }
}

// Master polling task - dedicated for polling only
static void master_polling_task(void *argument)
{
    test_context_t *ctx = &g_test_ctx;
    
    printf("[MASTER_POLLING] Task started\n");
    
    while (ctx->test_running) {
        // Only polling, no other operations
        ms_bridging_polling(ctx->master_handler);
        // osDelay(5); // Short delay to prevent excessive CPU usage
    }
    
    printf("[MASTER_POLLING] Task ended\n");
    vTaskDelete(NULL);
}

// Master task - only for sending requests and events
static void master_task(void *argument)
{
    test_context_t *ctx = &g_test_ctx;
    uint32_t test_start_time = osKernelGetTickCount();
    uint32_t last_request_time = 0;
    uint32_t last_event_time = 0;
    uint16_t request_counter = 0;
    uint16_t event_counter = 0;
    
    printf("[MASTER] Task started\n");
    
    while (ctx->test_running) {
        uint32_t current_time = osKernelGetTickCount();
        
        // Send requests periodically
        if (current_time - last_request_time >= TEST_REQUEST_INTERVAL_MS) {
            int ret = MS_BR_OK;
            
            switch (request_counter % 4) {
                case 0:
                    printf("[MASTER] Sending keep alive request\n");
                    ret = ms_bridging_request_keep_alive(ctx->master_handler);
                    break;
                    
                case 1: {
                    ms_bridging_time_t time;
                    printf("[MASTER] Sending get time request\n");
                    ret = ms_bridging_request_get_time(ctx->master_handler, &time);
                    if (ret == MS_BR_OK) {
                        printf("[MASTER] Received time: %04d-%02d-%02d %02d:%02d:%02d\n",
                               time.year, time.month, time.day, time.hour, time.minute, time.second);
                    }
                    break;
                }
                
                case 2: {
                    ms_bridging_time_t time = {
                        .year = 2024,
                        .month = 1,
                        .day = 15,
                        .week = 1,
                        .hour = 14,
                        .minute = 30,
                        .second = 0
                    };
                    printf("[MASTER] Sending set time request\n");
                    ret = ms_bridging_request_set_time(ctx->master_handler, &time);
                    break;
                }
                
                case 3: {
                    uint32_t switch_bits;
                    printf("[MASTER] Sending power status request\n");
                    ret = ms_bridging_request_power_status(ctx->master_handler, &switch_bits);
                    if (ret == MS_BR_OK) {
                        printf("[MASTER] Received switch bits: 0x%08X\n", switch_bits);
                    }
                    break;
                }
            }
            
            if (ret == MS_BR_OK) {
                ctx->master_stats.requests_sent++;
            } else {
                ctx->master_stats.errors++;
                printf("[MASTER] Request failed: %d\n", ret);
            }
            
            request_counter++;
            last_request_time = current_time;
        }
        
        // Send events periodically
        if (current_time - last_event_time >= TEST_EVENT_INTERVAL_MS) {
            int ret = MS_BR_OK;
            
            if (event_counter % 2 == 0) {
                uint16_t key_value = 0x1234 + event_counter;
                printf("[MASTER] Sending key value event: 0x%04X\n", key_value);
                ret = ms_bridging_event_key_value(ctx->master_handler, key_value);
            } else {
                uint16_t pir_value = 0x5678 + event_counter;
                printf("[MASTER] Sending PIR value event: 0x%04X\n", pir_value);
                ret = ms_bridging_event_pir_value(ctx->master_handler, pir_value);
            }
            
            if (ret == MS_BR_OK) {
                ctx->master_stats.events_sent++;
            } else {
                ctx->master_stats.errors++;
                printf("[MASTER] Event failed: %d\n", ret);
            }
            
            event_counter++;
            last_event_time = current_time;
        }
        
        // Check if test time is up
        if (current_time - test_start_time >= (TEST_RUN_TIME_SEC * 1000)) {
            ctx->test_running = 0;
            printf("[MASTER] Test time completed\n");
        }
        
        osDelay(50); // Longer delay since polling is handled separately
    }
    
    printf("[MASTER] Task ended\n");
    vTaskDelete(NULL);
}

// Slave polling task - dedicated for polling only
static void slave_polling_task(void *argument)
{
    test_context_t *ctx = &g_test_ctx;
    
    printf("[SLAVE_POLLING] Task started\n");
    
    while (ctx->test_running) {
        // Only polling, no other operations
        ms_bridging_polling(ctx->slave_handler);
        // osDelay(5); // Short delay to prevent excessive CPU usage
    }
    
    printf("[SLAVE_POLLING] Task ended\n");
    vTaskDelete(NULL);
}

// Slave task - only for sending requests and events
static void slave_task(void *argument)
{
    test_context_t *ctx = &g_test_ctx;
    uint32_t last_request_time = 0;
    uint32_t last_event_time = 0;
    uint16_t request_counter = 0;
    uint16_t event_counter = 0;
    
    printf("[SLAVE] Task started\n");
    
    while (ctx->test_running) {
        uint32_t current_time = osKernelGetTickCount();
        
        // Send requests periodically (offset from master)
        if (current_time - last_request_time >= TEST_REQUEST_INTERVAL_MS + 500) {
            int ret = MS_BR_OK;
            
            switch (request_counter % 3) {
                case 0:
                    printf("[SLAVE] Sending keep alive request\n");
                    ret = ms_bridging_request_keep_alive(ctx->slave_handler);
                    break;
                    
                case 1: {
                    ms_bridging_time_t time;
                    printf("[SLAVE] Sending get time request\n");
                    ret = ms_bridging_request_get_time(ctx->slave_handler, &time);
                    if (ret == MS_BR_OK) {
                        printf("[SLAVE] Received time: %04d-%02d-%02d %02d:%02d:%02d\n",
                               time.year, time.month, time.day, time.hour, time.minute, time.second);
                    }
                    break;
                }
                
                case 2: {
                    ms_bridging_power_ctrl_t power_ctrl = {
                        .power_mode = 1,
                        .switch_bits = 0x87654321,
                        .wakeup_flags = 0x11111111,
                        .sleep_second = 30
                    };
                    printf("[SLAVE] Sending power control request\n");
                    ret = ms_bridging_request_power_control(ctx->slave_handler, &power_ctrl);
                    break;
                }
            }
            
            if (ret == MS_BR_OK) {
                ctx->slave_stats.requests_sent++;
            } else {
                ctx->slave_stats.errors++;
                printf("[SLAVE] Request failed: %d\n", ret);
            }
            
            request_counter++;
            last_request_time = current_time;
        }
        
        // Send events periodically (offset from master)
        if (current_time - last_event_time >= TEST_EVENT_INTERVAL_MS + 1000) {
            int ret = MS_BR_OK;
            
            if (event_counter % 2 == 0) {
                uint16_t key_value = 0xABCD + event_counter;
                printf("[SLAVE] Sending key value event: 0x%04X\n", key_value);
                ret = ms_bridging_event_key_value(ctx->slave_handler, key_value);
            } else {
                uint16_t pir_value = 0xEF00 + event_counter;
                printf("[SLAVE] Sending PIR value event: 0x%04X\n", pir_value);
                ret = ms_bridging_event_pir_value(ctx->slave_handler, pir_value);
            }
            
            if (ret == MS_BR_OK) {
                ctx->slave_stats.events_sent++;
            } else {
                ctx->slave_stats.errors++;
                printf("[SLAVE] Event failed: %d\n", ret);
            }
            
            event_counter++;
            last_event_time = current_time;
        }
        
        osDelay(50); // Longer delay since polling is handled separately
    }
    
    printf("[SLAVE] Task ended\n");
    vTaskDelete(NULL);
}

// Communication relay task
static void comm_relay_task(void *argument)
{
    test_context_t *ctx = &g_test_ctx;
    virtual_msg_t msg;
    
    printf("[RELAY] Task started\n");
    
    while (ctx->test_running) {
        // Relay master to slave
        if (osMessageQueueGet(ctx->master_to_slave_queue, &msg, NULL, 10) == osOK) {
            ms_bridging_recv(ctx->slave_handler, msg.data, msg.len);
        }
        
        // Relay slave to master
        if (osMessageQueueGet(ctx->slave_to_master_queue, &msg, NULL, 10) == osOK) {
            ms_bridging_recv(ctx->master_handler, msg.data, msg.len);
        }
        
        osDelay(1);
    }
    
    printf("[RELAY] Task ended\n");
    vTaskDelete(NULL);
}

// Monitor task
static void monitor_task(void *argument)
{
    test_context_t *ctx = &g_test_ctx;
    uint32_t last_report_time = 0;
    
    printf("[MONITOR] Task started\n");
    
    while (ctx->test_running) {
        uint32_t current_time = osKernelGetTickCount();
        
        // Report statistics every 5 seconds
        if (current_time - last_report_time >= 5000) {
            printf("\n=== Test Statistics ===\n");
            printf("Master: Req_sent=%d, Req_recv=%d, Resp_sent=%d, Resp_recv=%d, "
                   "Evt_sent=%d, Evt_recv=%d, Ack_sent=%d, Ack_recv=%d, Errors=%d\n",
                   ctx->master_stats.requests_sent, ctx->master_stats.requests_received,
                   ctx->master_stats.responses_sent, ctx->master_stats.responses_received,
                   ctx->master_stats.events_sent, ctx->master_stats.events_received,
                   ctx->master_stats.acks_sent, ctx->master_stats.acks_received,
                   ctx->master_stats.errors);
            
            printf("Slave:  Req_sent=%d, Req_recv=%d, Resp_sent=%d, Resp_recv=%d, "
                   "Evt_sent=%d, Evt_recv=%d, Ack_sent=%d, Ack_recv=%d, Errors=%d\n",
                   ctx->slave_stats.requests_sent, ctx->slave_stats.requests_received,
                   ctx->slave_stats.responses_sent, ctx->slave_stats.responses_received,
                   ctx->slave_stats.events_sent, ctx->slave_stats.events_received,
                   ctx->slave_stats.acks_sent, ctx->slave_stats.acks_received,
                   ctx->slave_stats.errors);
            printf("========================\n\n");
            
            last_report_time = current_time;
        }
        
        osDelay(100);
    }
    
    printf("[MONITOR] Task ended\n");
    vTaskDelete(NULL);
}

// Test initialization
int ms_bridging_test_init(void)
{
    test_context_t *ctx = &g_test_ctx;
    
    printf("=== MS Bridging Test Initialization ===\n");
    
    // Create message queues
    ctx->master_to_slave_queue = osMessageQueueNew(TEST_QUEUE_SIZE, sizeof(virtual_msg_t), NULL);
    ctx->slave_to_master_queue = osMessageQueueNew(TEST_QUEUE_SIZE, sizeof(virtual_msg_t), NULL);
    
    if (ctx->master_to_slave_queue == NULL || ctx->slave_to_master_queue == NULL) {
        printf("Failed to create message queues\n");
        return -1;
    }
    
    // Initialize bridging handlers
    ctx->master_handler = ms_bridging_init(master_send_func, master_notify_cb);
    ctx->slave_handler = ms_bridging_init(slave_send_func, slave_notify_cb);
    
    if (ctx->master_handler == NULL || ctx->slave_handler == NULL) {
        printf("Failed to initialize bridging handlers\n");
        return -1;
    }
    
    // Initialize statistics
    memset(&ctx->master_stats, 0, sizeof(test_stats_t));
    memset(&ctx->slave_stats, 0, sizeof(test_stats_t));
    
    ctx->test_running = 1;
    
    printf("MS Bridging Test initialized successfully\n");
    return 0;
}

// Test cleanup
void ms_bridging_test_cleanup(void)
{
    test_context_t *ctx = &g_test_ctx;
    
    printf("=== MS Bridging Test Cleanup ===\n");
    
    ctx->test_running = 0;
    
    // Wait for tasks to finish
    osDelay(5000);
    
    // Cleanup handlers
    if (ctx->master_handler) {
        ms_bridging_deinit(ctx->master_handler);
        ctx->master_handler = NULL;
    }
    
    if (ctx->slave_handler) {
        ms_bridging_deinit(ctx->slave_handler);
        ctx->slave_handler = NULL;
    }
    
    // Cleanup queues
    if (ctx->master_to_slave_queue) {
        osMessageQueueDelete(ctx->master_to_slave_queue);
        ctx->master_to_slave_queue = NULL;
    }
    
    if (ctx->slave_to_master_queue) {
        osMessageQueueDelete(ctx->slave_to_master_queue);
        ctx->slave_to_master_queue = NULL;
    }
    
    printf("MS Bridging Test cleanup completed\n");
}

// Main test function
int ms_bridging_test_run(void)
{
    test_context_t *ctx = &g_test_ctx;
    osThreadAttr_t task_attr = {0};
    
    printf("=== MS Bridging Test Start ===\n");
    
    // Initialize test
    if (ms_bridging_test_init() != 0) {
        printf("Test initialization failed\n");
        return -1;
    }
    
    // Create tasks
    task_attr.stack_size = TEST_TASK_STACK_SIZE;
    task_attr.priority = TEST_TASK_PRIORITY;
    
    // Create main tasks (for sending requests/events)
    ctx->master_task_id = osThreadNew(master_task, NULL, &task_attr);
    ctx->slave_task_id = osThreadNew(slave_task, NULL, &task_attr);
    
    // Create polling tasks (dedicated for polling only)
    ctx->master_polling_task_id = osThreadNew(master_polling_task, NULL, &task_attr);
    ctx->slave_polling_task_id = osThreadNew(slave_polling_task, NULL, &task_attr);
    
    // Create communication and monitoring tasks
    ctx->comm_relay_task_id = osThreadNew(comm_relay_task, NULL, &task_attr);
    ctx->monitor_task_id = osThreadNew(monitor_task, NULL, &task_attr);
    
    if (ctx->master_task_id == NULL || ctx->slave_task_id == NULL || 
        ctx->master_polling_task_id == NULL || ctx->slave_polling_task_id == NULL ||
        ctx->comm_relay_task_id == NULL || ctx->monitor_task_id == NULL) {
        printf("Failed to create test tasks\n");
        ms_bridging_test_cleanup();
        return -1;
    }
    
    printf("Test tasks created successfully\n");
    printf("Test will run for %d seconds\n", TEST_RUN_TIME_SEC);
    
    // Wait for test completion
    osDelay(TEST_RUN_TIME_SEC * 1000);
    
    // Cleanup
    ms_bridging_test_cleanup();
    
    printf("=== MS Bridging Test Completed ===\n");
    return 0;
}

// Additional utility functions
void ms_bridging_test_get_stats(test_stats_t *master_stats, test_stats_t *slave_stats)
{
    test_context_t *ctx = &g_test_ctx;
    
    if (master_stats != NULL) {
        memcpy(master_stats, &ctx->master_stats, sizeof(test_stats_t));
    }
    
    if (slave_stats != NULL) {
        memcpy(slave_stats, &ctx->slave_stats, sizeof(test_stats_t));
    }
}

void ms_bridging_test_stop(void)
{
    test_context_t *ctx = &g_test_ctx;
    ctx->test_running = 0;
}

uint8_t ms_bridging_test_is_running(void)
{
    test_context_t *ctx = &g_test_ctx;
    return ctx->test_running;
}
