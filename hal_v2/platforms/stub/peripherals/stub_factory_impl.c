/** @file stub_factory_impl.c — HAL_FACTORY_OPS (stub) */
#include "peripheral/devices/hal_factory.h"

#include <stddef.h>
#include <string.h>

static int stub_factory_init(const HalFactoryConfig *config, void **factory_ctx_return)
{
    (void)config;
    if (!factory_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *factory_ctx_return = NULL;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_factory_deinit(void *factory_ctx)
{
    (void)factory_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_factory_read_all(void *factory_ctx, HalFactoryInfo *out)
{
    (void)factory_ctx;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_factory_get(void *factory_ctx, HalFactoryField field, char *value, uint32_t value_size)
{
    (void)factory_ctx;
    (void)field;
    if (value && value_size > 0)
    {
        value[0] = '\0';
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_factory_set(void *factory_ctx, HalFactoryField field, const char *value)
{
    (void)factory_ctx;
    (void)field;
    (void)value;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_factory_erase(void *factory_ctx)
{
    (void)factory_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_factory_get_version(void)
{
    return "HAL-FACTORY stub 2.0.0 (platform stub)";
}

HalFactoryOps HAL_FACTORY_OPS = {
    .init = stub_factory_init,
    .deinit = stub_factory_deinit,
    .read_all = stub_factory_read_all,
    .get = stub_factory_get,
    .set = stub_factory_set,
    .erase = stub_factory_erase,
    .get_version = stub_factory_get_version,
};
