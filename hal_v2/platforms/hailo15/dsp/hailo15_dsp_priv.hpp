/**
 * @file hailo15_dsp_priv.hpp
 * @brief Internal types for Hailo-15 DSP HAL implementation.
 */

#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

#include "dsp/hal_dsp.h"
#include <hailo/hailodsp.h>

struct HalDspJobTag {
    HalDspOpType     op_type;
    HalDspJobResult  result;
    std::atomic<bool> completed{false};
    std::mutex       mtx;
    std::condition_variable cv;

    /* Opaque pointer to operation-specific params (copied by implementation). */
    void            *params_copy;
};

struct Hailo15DspJobItem {
    HalDspJobHandle job;
};

struct Hailo15DspContext {
    dsp_device          device;
    int                 device_priority;

    std::mutex          queue_mtx;
    std::condition_variable queue_cv;
    std::queue<Hailo15DspJobItem> job_queue;
    std::atomic<bool>   stop_flag{false};
    std::thread         worker;
};

