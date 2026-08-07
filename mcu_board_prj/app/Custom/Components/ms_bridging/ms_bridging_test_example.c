/*
 * MS Bridging Test Usage Example
 * 
 * This file demonstrates how to use the optimized MS Bridging test
 * with separated polling and communication tasks.
 */

#include "ms_bridging_test.h"

// Example 1: Basic test execution
void ms_bridging_test_basic_example(void)
{
    printf("Starting MS Bridging Test...\n");
    
    // Run the test
    int result = ms_bridging_test_run();
    
    if (result == 0) {
        printf("MS Bridging Test completed successfully!\n");
    } else {
        printf("MS Bridging Test failed with error: %d\n", result);
    }
}

// Example 2: Test with statistics monitoring
void ms_bridging_test_with_stats_example(void)
{
    printf("Starting MS Bridging Test with Statistics...\n");
    
    // Initialize test
    if (ms_bridging_test_init() != 0) {
        printf("Test initialization failed\n");
        return;
    }
    
    // Run test for a specific duration
    uint32_t start_time = osKernelGetTickCount();
    uint32_t test_duration_ms = 10000; // 10 seconds
    
    while (osKernelGetTickCount() - start_time < test_duration_ms) {
        // Get current statistics
        test_stats_t master_stats, slave_stats;
        ms_bridging_test_get_stats(&master_stats, &slave_stats);
        
        printf("Master: Req_sent=%d, Req_recv=%d, Errors=%d\n",
               master_stats.requests_sent, master_stats.requests_received, master_stats.errors);
        printf("Slave:  Req_sent=%d, Req_recv=%d, Errors=%d\n",
               slave_stats.requests_sent, slave_stats.requests_received, slave_stats.errors);
        
        osDelay(2000); // Report every 2 seconds
    }
    
    // Stop and cleanup
    ms_bridging_test_stop();
    ms_bridging_test_cleanup();
    
    printf("Test completed\n");
}

// Example 3: Integration with main application task
void your_main_task(void *argument)
{
    // Your application initialization code here...
    printf("Application starting...\n");
    
    // Run the bridging test
    ms_bridging_test_basic_example();
    
    // Your application main loop here...
    while (1) {
        // Your application logic...
        printf("Application running...\n");
        osDelay(5000);
    }
}

// Example 4: Create test task in main.c
void create_test_task_example(void)
{
    osThreadAttr_t task_attr = {
        .name = "TestTask",
        .stack_size = 2048,
        .priority = osPriorityNormal,
    };
    
    osThreadId_t test_task_id = osThreadNew(your_main_task, NULL, &task_attr);
    
    if (test_task_id == NULL) {
        printf("Failed to create test task\n");
    } else {
        printf("Test task created successfully\n");
    }
}

// Example 5: Custom test configuration
void ms_bridging_test_custom_example(void)
{
    printf("Starting Custom MS Bridging Test...\n");
    
    // Initialize test
    if (ms_bridging_test_init() != 0) {
        printf("Test initialization failed\n");
        return;
    }
    
    // Custom test logic
    uint32_t test_start = osKernelGetTickCount();
    uint32_t custom_duration = 15000; // 15 seconds
    
    while (osKernelGetTickCount() - test_start < custom_duration) {
        // Check if test is still running
        if (!ms_bridging_test_is_running()) {
            printf("Test stopped by external signal\n");
            break;
        }
        
        // Custom monitoring logic
        test_stats_t master_stats, slave_stats;
        ms_bridging_test_get_stats(&master_stats, &slave_stats);
        
        // Check for errors
        if (master_stats.errors > 10 || slave_stats.errors > 10) {
            printf("Too many errors detected, stopping test\n");
            ms_bridging_test_stop();
            break;
        }
        
        osDelay(1000);
    }
    
    // Final statistics
    test_stats_t final_master_stats, final_slave_stats;
    ms_bridging_test_get_stats(&final_master_stats, &final_slave_stats);
    
    printf("Final Statistics:\n");
    printf("Master: Total requests=%d, Errors=%d\n", 
           final_master_stats.requests_sent + final_master_stats.requests_received,
           final_master_stats.errors);
    printf("Slave:  Total requests=%d, Errors=%d\n", 
           final_slave_stats.requests_sent + final_slave_stats.requests_received,
           final_slave_stats.errors);
    
    // Cleanup
    ms_bridging_test_cleanup();
    printf("Custom test completed\n");
}
