/**
 * @file hailo15_dsp_priv.hpp
 * @brief Internal types for Hailo-15 DSP HAL implementation.
 */

#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <thread>

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

    /* Ownership handoff between the worker thread and job_release():
     * - worker_done: set by the worker under mtx as its FINAL access to the job
     *   (cv notification happens before it). Once visible, a racing
     *   job_release() may delete the job, so the worker must not touch it after.
     * - release_requested: set by job_release() under mtx when the caller released
     *   the handle before the worker finished. Exactly one side (whoever observes
     *   both flags) deletes the job, preventing use-after-free on cancel+release
     *   while an operation is executing. */
    std::atomic<bool> worker_done{false};
    std::atomic<bool> release_requested{false};
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

