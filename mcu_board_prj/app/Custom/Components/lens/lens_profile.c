#include "lens_profile.h"

#if defined(CONFIG_LENS_PROFILE_AF0832) && defined(CONFIG_LENS_PROFILE_FG2009)
#error "Only one lens profile may be selected"
#endif

const lens_profile_t *lens_profile_get_active(void)
{
#if defined(CONFIG_LENS_PROFILE_FG2009)
    return &g_lens_profile_fg2009;
#else
    return &g_lens_profile_af0832;
#endif
}
