#include "lens_profile.h"
#include "sys_config.h"

/* Keep AF0832 as the power-on default for backwards compatibility. */
static const lens_profile_t *s_active_profile = &g_lens_profile_af0832;

const lens_profile_t *lens_profile_get(lens_model_t model)
{
    switch (model) {
    case LENS_MODEL_AF0832:
        return &g_lens_profile_af0832;
    case LENS_MODEL_FG2009:
        return &g_lens_profile_fg2009;
    default:
        return NULL;
    }
}

const lens_profile_t *lens_profile_get_active(void)
{
    return s_active_profile;
}

lens_model_t lens_profile_get_active_model(void)
{
    return s_active_profile->model;
}

int lens_profile_select(lens_model_t model)
{
    const lens_profile_t *profile = lens_profile_get(model);

    if (profile == NULL) {
        return SYS_ERR_INVALID_ARG;
    }
    s_active_profile = profile;
    return SYS_OK;
}
