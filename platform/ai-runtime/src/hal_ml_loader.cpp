#include "hal_ml_loader.h"
#include "log.h"
#include <dlfcn.h>

namespace aipc::ai_runtime {

HalMlLoader::~HalMlLoader() {
    unload();
}

bool HalMlLoader::load(const std::string& lib_path) {
    dl_handle_ = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!dl_handle_) {
        LOG_ERROR("dlopen(%s) failed: %s", lib_path.c_str(), dlerror());
        return false;
    }

    // Resolve HAL_INFERENCE_OPS
    infer_ops_ = static_cast<HalInferenceOps*>(dlsym(dl_handle_, "HAL_INFERENCE_OPS"));
    if (!infer_ops_) {
        LOG_ERROR("dlsym(HAL_INFERENCE_OPS) failed: %s", dlerror());
        dlclose(dl_handle_);
        dl_handle_ = nullptr;
        return false;
    }

    // ABI guard: absent from HALs built before 2026-09-16 — members appended
    // after get_version (tensor_from_frame_ex / bind_dma_frame /
    // probe_capability) are then treated as unavailable so a mixed deploy
    // falls back instead of calling past the end of the provider's table
    // (the R1 SIGILL). Not fatal: the core table still works.
    infer_ops_abi_size_ = static_cast<const uint32_t*>(
        dlsym(dl_handle_, "HAL_INFERENCE_OPS_ABI_SIZE"));
    if (!infer_ops_abi_size_) {
        LOG_WARN("HAL_INFERENCE_OPS_ABI_SIZE not found in %s — HAL predates the "
                 "ABI guard; tensor_from_frame_ex / bind_dma_frame / "
                 "probe_capability disabled (deploy ai-runtime and HAL from "
                 "the same build)",
                 lib_path.c_str());
    }

    // Resolve HAL_POSTPROCESS_OPS (optional)
    post_ops_ = static_cast<HalPostprocessOps*>(dlsym(dl_handle_, "HAL_POSTPROCESS_OPS"));
    if (post_ops_) {
        LOG_INFO("HAL POSTPROCESS loaded");
    } else {
        LOG_WARN("HAL_POSTPROCESS_OPS not found in %s (post-processing unavailable)",
                 lib_path.c_str());
    }

    // Resolve HAL_DRAW_OPS (optional)
    draw_ops_ = static_cast<HalDrawOps*>(dlsym(dl_handle_, "HAL_DRAW_OPS"));
    if (draw_ops_) {
        LOG_INFO("HAL DRAW loaded");
    } else {
        LOG_WARN("HAL_DRAW_OPS not found in %s (drawing unavailable)",
                 lib_path.c_str());
    }

    // Resolve HAL_CLIP_TEXT_ENCODER_OPS (optional)
    clip_text_enc_ops_ = static_cast<HalClipTextEncoderOps*>(
        dlsym(dl_handle_, "HAL_CLIP_TEXT_ENCODER_OPS"));
    if (clip_text_enc_ops_) {
        LOG_INFO("HAL CLIP text encoder loaded");
    } else {
        LOG_WARN("HAL_CLIP_TEXT_ENCODER_OPS not found in %s (CLIP text search unavailable)",
                 lib_path.c_str());
    }

    // Resolve HAL_GENAI_OPS (optional)
    genai_ops_ = static_cast<HalGenaiOps*>(
        dlsym(dl_handle_, "HAL_GENAI_OPS"));
    if (genai_ops_) {
        LOG_INFO("HAL GenAI loaded");
    } else {
        LOG_WARN("HAL_GENAI_OPS not found in %s (GenAI unavailable)",
                 lib_path.c_str());
    }

    if (infer_ops_->get_version) {
        LOG_INFO("HAL Inference loaded: %s", infer_ops_->get_version());
    }

    return true;
}

void HalMlLoader::unload() {
    infer_ops_ = nullptr;
    infer_ops_abi_size_ = nullptr;
    post_ops_  = nullptr;
    draw_ops_  = nullptr;
    clip_text_enc_ops_ = nullptr;
    genai_ops_ = nullptr;
    if (dl_handle_) {
        dlclose(dl_handle_);
        dl_handle_ = nullptr;
    }
}

}  // namespace aipc::ai_runtime
