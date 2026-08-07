/**
 * @file hal_io.h
 * @brief HAL IO - SoC-level GPIO and PWM hardware I/O control.
 *
 * Provides direct SoC GPIO and optional PWM operations. On Hailo-15, GPIO uses libgpiod
 * (@c /dev/gpiochip0 and @c /dev/gpiochip1); SoC PWM is not implemented (@c HAL_ERR_NOT_SUPPORTED).
 * This is separate from MCU-controlled peripherals (see hal_mcu.h and
 * include/peripheral/devices/).
 *
 * Lifecycle:
 *   1. HAL_IO_OPS.init(&io_ctx)
 *   2. Export / configure / read / write GPIOs (and PWM where supported)
 *   3. HAL_IO_OPS.deinit(io_ctx)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * GPIO types
 * -------------------------------------------------------------------- */

/** GPIO direction. */
typedef enum {
    HAL_GPIO_DIR_INPUT  = 0,
    HAL_GPIO_DIR_OUTPUT = 1,
} HalGpioDirection;

/** GPIO edge trigger type for event subscription. */
typedef enum {
    HAL_GPIO_EDGE_NONE    = 0,
    HAL_GPIO_EDGE_RISING  = 1,
    HAL_GPIO_EDGE_FALLING = 2,
    HAL_GPIO_EDGE_BOTH    = 3,
} HalGpioEdge;

/** GPIO export configuration. */
typedef struct {
    /**
     * GPIO index. On Hailo-15: @c 0..15 = @c gpiochip0 offset @c 0..15;
     * @c 16..31 = @c gpiochip1 offset @c 0..15 (see @c gpiodetect / @c gpioinfo).
     * Other platforms may interpret this differently.
     */
    uint32_t         gpio_num;
    HalGpioDirection direction;    /**< input or output */
    /**
     * When false: logical value aligns with pin voltage (high = true). When true: line uses
     * active-low / inverted semantics (e.g. libgpiod @c ACTIVE_LOW — logical true vs physical high differ).
     */
    bool             active_low;
    const char      *label;        /**< human-readable label (informational) */
} HalGpioConfig;

/**
 * GPIO event callback.
 * @param io_ctx    IO context.
 * @param gpio_num  GPIO number that triggered the event.
 * @param value     Current logical value (after active_low inversion).
 * @param userdata  Opaque pointer registered at subscribe time.
 */
typedef void (*HalGpioEventCallback)(void *io_ctx, uint32_t gpio_num,
                                      bool value, void *userdata);

/* --------------------------------------------------------------------
 * PWM types
 * -------------------------------------------------------------------- */

/** PWM channel configuration (only used when @ref HalIoOps PWM entries return success). */
typedef struct {
    uint32_t pwm_chip;       /**< Platform-defined PWM controller index */
    uint32_t pwm_channel;    /**< Channel within the controller */
    uint32_t period_ns;      /**< period in nanoseconds */
    uint32_t duty_ns;        /**< duty cycle in nanoseconds */
    bool     enabled;        /**< whether the channel is active */
} HalPwmConfig;

/* --------------------------------------------------------------------
 * IO operations table
 * -------------------------------------------------------------------- */

/**
 * Function-pointer table for SoC-level I/O operations.
 * Platform implementations populate HAL_IO_OPS at link time.
 */
typedef struct {
    /**
     * @brief Initialize the SoC I/O subsystem.
     * @param io_ctx_return Receives the allocated IO context on success.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*init)(void **io_ctx_return);

    /**
     * @brief Tear down the SoC I/O subsystem.
     * @param io_ctx IO context returned by init().
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*deinit)(void *io_ctx);

    /* ---- GPIO ---- */

    /** @brief Export a GPIO and configure its direction. */
    int (*gpio_export)(void *io_ctx, const HalGpioConfig *config);

    /** @brief Unexport (release) a previously exported GPIO. */
    int (*gpio_unexport)(void *io_ctx, uint32_t gpio_num);

    /** @brief Set the output value of a GPIO. */
    int (*gpio_set_value)(void *io_ctx, uint32_t gpio_num, bool value);

    /** @brief Read the current value of a GPIO. */
    int (*gpio_get_value)(void *io_ctx, uint32_t gpio_num, bool *value);

    /** @brief Change the direction of an already-exported GPIO. */
    int (*gpio_set_direction)(void *io_ctx, uint32_t gpio_num, HalGpioDirection dir);

    /**
     * @brief Subscribe to edge-triggered events on a GPIO.
     *
     * The callback is invoked from an internal worker thread.
     *
     * @param io_ctx   IO context.
     * @param gpio_num GPIO number (must be already exported as input).
     * @param edge     Edge type to watch.
     * @param callback Event callback.
     * @param userdata Opaque pointer passed to callback.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*gpio_subscribe)(void *io_ctx, uint32_t gpio_num, HalGpioEdge edge,
                          HalGpioEventCallback callback, void *userdata);

    /** @brief Unsubscribe from GPIO edge events. */
    int (*gpio_unsubscribe)(void *io_ctx, uint32_t gpio_num);

    /* ---- PWM ---- */

    /** @brief Configure a PWM channel (period, duty, enable state). May return @c HAL_ERR_NOT_SUPPORTED. */
    int (*pwm_configure)(void *io_ctx, const HalPwmConfig *config);

    /** @brief Update duty cycle. May return @c HAL_ERR_NOT_SUPPORTED. */
    int (*pwm_set_duty)(void *io_ctx, uint32_t pwm_chip, uint32_t pwm_channel,
                        uint32_t duty_ns);

    /** @brief Enable or disable a PWM channel. May return @c HAL_ERR_NOT_SUPPORTED. */
    int (*pwm_enable)(void *io_ctx, uint32_t pwm_chip, uint32_t pwm_channel,
                      bool enable);

    /**
     * @brief Get the IO HAL version string.
     * @return Static version string e.g. "Hailo15 HAL-IO 2.0.0".
     */
    const char *(*get_version)(void);
} HalIoOps;

/** Platform-specific IO operations (resolved at link time). */
extern HalIoOps HAL_IO_OPS;

#ifdef __cplusplus
}
#endif
