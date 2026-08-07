#pragma once

#include "model/hal_inference.h"
#include "model/hal_postprocess.h"
#include "model/hal_draw.h"
#include "model/hal_clip_text_encoder_ops.h"
#include "model/hal_genai.h"
#include <string>

namespace aipc::ai_runtime {

/// Dynamically loads HAL ML shared library via dlopen and resolves ops symbols.
class HalMlLoader {
public:
    HalMlLoader() = default;
    ~HalMlLoader();

    HalMlLoader(const HalMlLoader&) = delete;
    HalMlLoader& operator=(const HalMlLoader&) = delete;
    HalMlLoader(HalMlLoader&&) = delete;
    HalMlLoader& operator=(HalMlLoader&&) = delete;

    /// Load the HAL .so and resolve ops symbols.
    /// Returns false on failure.
    bool load(const std::string& lib_path);

    /// Unload the library.
    void unload();

    // HAL v2: session-based inference + postprocess + draw
    const HalInferenceOps*          infer_ops()          const { return infer_ops_; }
    const HalPostprocessOps*        post_ops()           const { return post_ops_; }
    const HalDrawOps*               draw_ops()           const { return draw_ops_; }
    const HalClipTextEncoderOps*    clip_text_enc_ops()  const { return clip_text_enc_ops_; }
    const HalGenaiOps*             genai_ops()         const { return genai_ops_; }

private:
    void* dl_handle_ = nullptr;

    HalInferenceOps*          infer_ops_          = nullptr;
    HalPostprocessOps*        post_ops_           = nullptr;
    HalDrawOps*               draw_ops_           = nullptr;
    HalClipTextEncoderOps*    clip_text_enc_ops_  = nullptr;
    HalGenaiOps*              genai_ops_          = nullptr;
};

}  // namespace aipc::ai_runtime
