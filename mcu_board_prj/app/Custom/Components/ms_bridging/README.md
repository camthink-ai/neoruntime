# MS Bridging Test Suite - Optimized Architecture

## Overview

Based on your requirements, I have optimized the MS Bridging test suite by separating `polling` and `recv` operations from other APIs into different threads to avoid blocking issues that cause request failures.

## Optimized Architecture

### Thread Separation Design

```
┌─────────────────┐    ┌─────────────────┐
│   Master Task   │    │   Slave Task     │
│ (Send Requests) │    │ (Send Requests)  │
└─────────────────┘    └─────────────────┘
         │                       │
         └───────────┬─────────────┘
                     │
         ┌─────────────────────────┐
         │   Communication Relay   │
         │      (Recv Only)        │
         └─────────────────────────┘
                     │
         ┌─────────────────────────┐
         │   Master Polling Task  │
         │   (Polling Only)        │
         └─────────────────────────┘
                     │
         ┌─────────────────────────┐
         │   Slave Polling Task   │
         │   (Polling Only)        │
         └─────────────────────────┘
                     │
         ┌─────────────────────────┐
         │   Monitor Task         │
         │   (Statistics)          │
         └─────────────────────────┘
```

### Task Responsibility Separation

1. **Master Task**: Dedicated to sending requests and events
2. **Slave Task**: Dedicated to sending requests and events
3. **Master Polling Task**: Dedicated to polling operations for master handler
4. **Slave Polling Task**: Dedicated to polling operations for slave handler
5. **Communication Relay Task**: Dedicated to message reception and forwarding
6. **Monitor Task**: Responsible for statistics output

## Key Optimization Points

### 1. Avoid Blocking Issues
- **Separate polling**: Polling operations run in independent threads, will not block request operations
- **Separate recv**: Message reception is handled in an independent relay thread
- **Non-blocking design**: Send and receive operations are completely separated to avoid mutual blocking

### 2. Improve Concurrent Performance
- **Independent threads**: Each operation has an independent thread, improving concurrent processing capability
- **Short delay**: Polling tasks use 5ms short delay to ensure timely processing
- **Long delay**: Send tasks use 50ms long delay to reduce CPU usage

### 3. Thread Safety
- **Message queue**: Use CMSIS-OS2 message queue for inter-thread communication
- **Atomic operations**: Statistics updates use atomic operations
- **Synchronization mechanism**: Appropriate task synchronization and delays

## File Structure

```
Custom/Components/ms_bridging/
├── ms_bridging_test.h          # Test header file
├── ms_bridging_test.c          # Optimized test implementation
├── ms_bridging_test_example.c  # Usage example
└── README.md                   # This document
```

## Usage

### Basic Usage
```c
#include "ms_bridging_test.h"

int main(void)
{
    // Run complete test
    int result = ms_bridging_test_run();
    
    if (result == 0) {
        printf("Test completed successfully!\n");
    } else {
        printf("Test failed: %d\n", result);
    }
    
    return 0;
}
```

### Advanced Usage
```c
// Initialize test
ms_bridging_test_init();

// Run custom test logic
while (ms_bridging_test_is_running()) {
    // Get statistics
    test_stats_t master_stats, slave_stats;
    ms_bridging_test_get_stats(&master_stats, &slave_stats);
    
    // Custom logic...
    osDelay(1000);
}

// Stop test
ms_bridging_test_stop();

// Cleanup resources
ms_bridging_test_cleanup();
```

## API Reference

### Main Functions

- `ms_bridging_test_init()`: Initialize test environment
- `ms_bridging_test_run()`: Run complete test
- `ms_bridging_test_cleanup()`: Cleanup test resources
- `ms_bridging_test_stop()`: Stop test
- `ms_bridging_test_is_running()`: Check test status
- `ms_bridging_test_get_stats()`: Get statistics

### Configuration Parameters

```c
#define TEST_QUEUE_SIZE                 (32)        // Message queue size
#define TEST_TASK_STACK_SIZE            (1024)      // Task stack size
#define TEST_TASK_PRIORITY              (osPriorityNormal)  // Task priority
#define TEST_RUN_TIME_SEC               (30)        // Test duration
#define TEST_REQUEST_INTERVAL_MS        (1000)      // Request interval
#define TEST_EVENT_INTERVAL_MS          (2000)      // Event interval
```

## Performance Characteristics

### 1. High Concurrency Processing
- 6 independent threads running in parallel
- Non-blocking message processing
- Efficient resource utilization

### 2. Real-time Response
- 5ms polling interval ensures timely response
- Independent communication processing thread
- Real-time statistics updates

### 3. Stability
- Thread-safe design
- Comprehensive error handling
- Automatic resource cleanup

## Test Scenarios

### 1. Request/Response Tests
- Keep Alive requests
- Time management requests
- Power control requests
- Status query requests

### 2. Event Notification Tests
- Key value events
- PIR sensor events
- Bidirectional event confirmation

### 3. Error Handling Tests
- Timeout handling
- CRC verification
- Memory management
- Queue overflow

## Monitoring and Debugging

### Real-time Statistics
- Request send/receive count
- Response send/receive count
- Event send/receive count
- Confirmation send/receive count
- Error and timeout count

### Log Output
- Detailed operation logs
- Error information records
- Performance statistics reports
- Task status monitoring

## Extensibility

### Adding New Test Scenarios
1. Add new command handling in notification callbacks
2. Add new test cases in send tasks
3. Update statistics structure
4. Adjust test parameters

### Custom Configuration
- Modify test duration
- Adjust request/event intervals
- Change queue size and task priority
- Customize error handling logic

## Summary

The optimized MS Bridging test suite effectively solves the problem of polling and recv operations blocking requests through thread separation design. The new architecture provides:

- **High Concurrency Performance**: 6 independent threads processing in parallel
- **Real-time Response**: 5ms polling interval
- **Stability**: Thread-safe design
- **Usability**: Simple API interface
- **Extensibility**: Flexible configuration and extension capabilities

This optimization ensures test reliability and performance, providing strong support for MS Bridging protocol quality assurance.
