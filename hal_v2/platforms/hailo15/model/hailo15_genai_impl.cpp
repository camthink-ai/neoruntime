/**
 * @file hailo15_genai_impl.cpp
 * @brief Hailo-15 HAL GenAI — LLM/VLM via HailoRT GenAI API.
 */

#include "common/hal_common.h"
#include "common/hal_log.h"
#include "model/hal_genai.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(HAL_HAVE_HAL_GENAI)

#include <hailo/expected.hpp>
#include <hailo/genai/llm/llm.hpp>
#include <hailo/genai/vlm/vlm.hpp>
#include <hailo/hailort.h>
#include <hailo/hailort_defaults.hpp>
#include <hailo/vdevice.hpp>

namespace
{

using hailort::genai::LLMGeneratorCompletion;

// group_id shared across the Hailo-15 HAL so every VDevice consumer (inference, GenAI, and
// the medialib AI-ISP) joins the SAME cross-process group and shares the single NPU via
// hailort_server. MUST equal the medialib's hailort.device-id ("device0"). A mismatch makes
// hailort_server refuse this attach with HAILO_DEVICE_IN_USE(73) as soon as AI-ISP is active.
// Kept in sync with kSharedVDeviceGroupId in hailo15_inference_impl.cpp.
static constexpr const char *kSharedVDeviceGroupId = "device0";

static int map_hailo_status(hailo_status s)
{
    if (s == HAILO_SUCCESS)
        return HAL_OK;
    return HAL_ERR_RESULT;
}

static void apply_hal_generator_params(const HalGenaiGeneratorParams *hp, hailort::genai::LLMGeneratorParams &out)
{
    if (!hp)
        return;
    out.set_temperature(hp->temperature);
    out.set_top_p(hp->top_p);
    /* HailoRT GenAI rejects top_k == 0; leave SDK default when unset / zero. */
    if (hp->top_k > 0)
        out.set_top_k(hp->top_k);
    /* HailoRT GenAI rejects frequency_penalty == 0; leave SDK default when unset / zero. */
    if (hp->frequency_penalty != 0.0f)
        out.set_frequency_penalty(hp->frequency_penalty);
    out.set_max_generated_tokens(hp->max_generated_tokens);
    out.set_do_sample(hp->do_sample);
    if (hp->use_fixed_seed)
        out.set_seed(hp->seed);
    else
        out.set_seed(HAILO_RANDOM_SEED);
}

static size_t count_image_placeholders_heuristic(const std::vector<std::string> &messages_json)
{
    auto count_in = [](const std::string &s, const std::string &needle) -> size_t {
        size_t count = 0;
        size_t pos = 0;
        while (true)
        {
            pos = s.find(needle, pos);
            if (pos == std::string::npos)
                return count;
            ++count;
            pos += needle.size();
        }
    };

    size_t total = 0;
    for (const auto &msg : messages_json)
    {
        total += count_in(msg, "\"type\":\"image\"");
        total += count_in(msg, "\"type\": \"image\"");
    }
    return total;
}

static std::string escape_for_json_string_content(const std::string &content)
{
    static constexpr char QUOTE_CHAR = '"';
    static constexpr char BACKSLASH_CHAR = '\\';
    static constexpr std::string_view ESCAPED_QUOTE = "\\\"";
    static constexpr std::string_view ESCAPED_BACKSLASH = "\\\\";

    std::string escaped_content = content;
    size_t pos = 0;
    while ((pos = escaped_content.find(QUOTE_CHAR, pos)) != std::string::npos)
    {
        escaped_content.replace(pos, 1, ESCAPED_QUOTE);
        pos += ESCAPED_QUOTE.size();
    }
    pos = 0;
    while ((pos = escaped_content.find(BACKSLASH_CHAR, pos)) != std::string::npos)
    {
        if (pos + 1 < escaped_content.size() && escaped_content[pos + 1] != QUOTE_CHAR)
        {
            escaped_content.replace(pos, 1, ESCAPED_BACKSLASH);
            pos += ESCAPED_BACKSLASH.size();
        }
        else
        {
            pos++;
        }
    }
    return escaped_content;
}

static void append_assistant_json_message(std::vector<std::string> &history, const std::string &content)
{
    history.push_back(std::string(R"({"role": "assistant", "content": ")") + escape_for_json_string_content(content) +
                        R"("})");
}

struct HalGenaiSessionImpl
{
    std::mutex mtx;
    std::atomic<bool> generating{false};

    std::unique_ptr<LLMGeneratorCompletion> completion;

    HalGenaiKind kind{HAL_GENAI_KIND_LLM};
    std::shared_ptr<hailort::VDevice> vdevice;
    std::unique_ptr<hailort::genai::LLM> llm;
    std::unique_ptr<hailort::genai::VLM> vlm;

    std::vector<std::string> conversation_history;

    bool has_default_gen_params{false};
    HalGenaiGeneratorParams default_gen_params{};

    /** Last UTF-8 stop sequences passed to set_stop_tokens (re-applied after VLM clear_context / LLM clear_context). */
    std::vector<std::string> stop_tokens_cache;
};

static HalGenaiGeneratorParams merge_gen_params(const HalGenaiSessionImpl *s, const HalGenaiGeneratorParams *call_params)
{
    if (call_params)
        return *call_params;
    if (s->has_default_gen_params)
        return s->default_gen_params;
    HalGenaiGeneratorParams d{};
    d.temperature = 0.8f;
    d.top_p = 0.95f;
    d.top_k = 40;
    d.frequency_penalty = 0.f;
    d.max_generated_tokens = 256;
    d.do_sample = true;
    d.use_fixed_seed = false;
    d.seed = HAILO_RANDOM_SEED;
    return d;
}

static hailort::genai::LLMGeneratorCompletion generate_llm(HalGenaiSessionImpl *session,
                                                           const std::vector<std::string> &full_messages,
                                                           const HalGenaiGeneratorParams &gp)
{
    auto &llm = *session->llm;

    const bool is_continuation =
        full_messages.size() > session->conversation_history.size() &&
        std::equal(session->conversation_history.begin(), session->conversation_history.end(), full_messages.begin());

    std::vector<std::string> messages_to_send;
    if (is_continuation)
    {
        messages_to_send.assign(full_messages.begin() + static_cast<std::ptrdiff_t>(session->conversation_history.size()),
                                full_messages.end());
    }
    else
    {
        const hailo_status cs = llm.clear_context();
        if (cs != HAILO_SUCCESS)
            throw hailort::hailort_error(cs, "LLM clear_context failed");
        if (!session->stop_tokens_cache.empty())
        {
            const hailo_status ts = llm.set_stop_tokens(session->stop_tokens_cache);
            if (ts != HAILO_SUCCESS)
                throw hailort::hailort_error(ts, "LLM set_stop_tokens after clear_context failed");
        }
        messages_to_send = full_messages;
    }

    hailort::genai::LLMGeneratorParams params =
        llm.create_generator_params().expect("create_generator_params failed");
    apply_hal_generator_params(&gp, params);

    session->conversation_history = full_messages;

    return llm.generate(params, messages_to_send).expect("LLM generate failed");
}

static hailort::genai::LLMGeneratorCompletion generate_vlm(HalGenaiSessionImpl *session,
                                                           const std::vector<std::string> &messages_json,
                                                           const std::vector<hailort::MemoryView> &frames,
                                                           const HalGenaiGeneratorParams &gp)
{
    auto &vlm = *session->vlm;

    const hailo_status cs = vlm.clear_context();
    if (cs != HAILO_SUCCESS)
        throw hailort::hailort_error(cs, "VLM clear_context failed");
    if (!session->stop_tokens_cache.empty())
    {
        const hailo_status ts = vlm.set_stop_tokens(session->stop_tokens_cache);
        if (ts != HAILO_SUCCESS)
            throw hailort::hailort_error(ts, "VLM set_stop_tokens after clear_context failed");
    }

    hailort::genai::LLMGeneratorParams params =
        vlm.create_generator_params().expect("VLM create_generator_params failed");
    apply_hal_generator_params(&gp, params);

    auto generator = vlm.create_generator(params).expect("VLM create_generator failed");
    return generator.generate(messages_json, frames).expect("VLM generator.generate failed");
}

struct ActiveGenerationGuard
{
    HalGenaiSessionImpl *impl;
    explicit ActiveGenerationGuard(HalGenaiSessionImpl *s) : impl(s) {}
    ~ActiveGenerationGuard()
    {
        std::lock_guard<std::mutex> lock(impl->mtx);
        impl->completion.reset();
        impl->generating.store(false);
    }
};

static void run_completion_stream(HalGenaiSessionImpl *session, LLMGeneratorCompletion &&comp,
                                  HalGenaiTokenCallback on_token, void *token_user, HalGenaiFinishCallback on_finish,
                                  void *finish_user, bool append_assistant_to_llm_history)
{
    using GenerationStatus = hailort::genai::LLMGeneratorCompletion::Status;

    std::string assistant_accum;

    {
        std::lock_guard<std::mutex> lock(session->mtx);
        session->completion = std::make_unique<LLMGeneratorCompletion>(std::move(comp));
        session->generating.store(true);
    }

    ActiveGenerationGuard guard(session);

    bool notified_finish = false;
    auto notify_finish = [&](HalGenaiFinishReason fr, int code) {
        if (notified_finish)
            return;
        notified_finish = true;
        if (on_finish)
            on_finish(fr, code, finish_user);
    };

    try
    {
        while (true)
        {
            LLMGeneratorCompletion *active = nullptr;
            {
                std::lock_guard<std::mutex> lock(session->mtx);
                active = session->completion.get();
            }
            if (!active)
                break;

            auto tok_exp = active->read();
            if (!tok_exp.has_value())
            {
                notify_finish(HAL_GENAI_FINISH_ERROR, map_hailo_status(tok_exp.status()));
                break;
            }

            std::string token = std::move(tok_exp.value());
            const GenerationStatus st = active->generation_status();

            if (!token.empty())
            {
                assistant_accum += token;
                if (on_token)
                    on_token(token.c_str(), token_user);
            }

            if (st != GenerationStatus::GENERATING)
            {
                HalGenaiFinishReason fr = HAL_GENAI_FINISH_DONE;
                if (st == GenerationStatus::ABORTED)
                    fr = HAL_GENAI_FINISH_ABORTED;
                else if (st == GenerationStatus::MAX_TOKENS_REACHED)
                    fr = HAL_GENAI_FINISH_MAX_TOKENS;

                if (append_assistant_to_llm_history && fr != HAL_GENAI_FINISH_ABORTED && !assistant_accum.empty())
                    append_assistant_json_message(session->conversation_history, assistant_accum);

                notify_finish(fr, HAL_OK);
                break;
            }
        }
    }
    catch (...)
    {
        notify_finish(HAL_GENAI_FINISH_ERROR, HAL_ERROR);
    }

    if (!notified_finish)
        notify_finish(HAL_GENAI_FINISH_ERROR, HAL_ERR_RESULT);
}

static int genai_abort_generation(HalGenaiSession *session);

static HalGenaiSession *genai_create(const HalGenaiCreateParams *params)
{
    if (!params || params->hef_path[0] == '\0')
        return nullptr;

    try
    {
        auto vdevice_params = hailort::HailoRTDefaults::get_vdevice_params();
        vdevice_params.multi_process_service = true;
        // Default to the shared group so a VLM/LLM coexists with inference + AI-ISP on the
        // one NPU via hailort_server. params->vdevice_group_id still lets a caller override
        // for an exclusive run.
        vdevice_params.group_id = kSharedVDeviceGroupId;
        std::string group_storage;
        if (params->vdevice_group_id && params->vdevice_group_id[0] != '\0')
        {
            group_storage = params->vdevice_group_id;
            vdevice_params.group_id = group_storage.c_str();
        }

        auto impl = std::make_unique<HalGenaiSessionImpl>();
        impl->vdevice = hailort::VDevice::create_shared(vdevice_params).expect("VDevice::create_shared failed");
        impl->kind = params->kind;

        if (params->kind == HAL_GENAI_KIND_VLM)
        {
            hailort::genai::VLMParams vp(std::string(params->hef_path), params->optimize_memory_on_device);
            impl->vlm = std::make_unique<hailort::genai::VLM>(
                hailort::genai::VLM::create(impl->vdevice, vp).expect("VLM::create failed"));
        }
        else
        {
            hailort::genai::LLMParams lp;
            const std::string lora = (params->lora_name && params->lora_name[0] != '\0') ? params->lora_name : "";
            lp.set_model(std::string(params->hef_path), lora);
            lp.set_optimize_memory_on_device(params->optimize_memory_on_device);
            impl->llm = std::make_unique<hailort::genai::LLM>(
                hailort::genai::LLM::create(impl->vdevice, lp).expect("LLM::create failed"));
        }

        return reinterpret_cast<HalGenaiSession *>(impl.release());
    }
    catch (...)
    {
        return nullptr;
    }
}

static void genai_destroy(HalGenaiSession *session)
{
    if (!session)
        return;
    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    genai_abort_generation(session);
    delete impl;
}

static int genai_clear_context(HalGenaiSession *session)
{
    if (!session)
        return HAL_ERR_INVALID_ARG;
    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    if (impl->generating.load())
        return HAL_ERR_INVALID_STATE;

    try
    {
        if (impl->kind == HAL_GENAI_KIND_LLM && impl->llm)
        {
            const hailo_status s = impl->llm->clear_context();
            if (s != HAILO_SUCCESS)
                return map_hailo_status(s);
            impl->conversation_history.clear();
            if (!impl->stop_tokens_cache.empty())
            {
                const hailo_status ts = impl->llm->set_stop_tokens(impl->stop_tokens_cache);
                if (ts != HAILO_SUCCESS)
                    return map_hailo_status(ts);
            }
            return HAL_OK;
        }
        if (impl->kind == HAL_GENAI_KIND_VLM && impl->vlm)
        {
            const hailo_status s = impl->vlm->clear_context();
            if (s != HAILO_SUCCESS)
                return map_hailo_status(s);
            if (!impl->stop_tokens_cache.empty())
            {
                const hailo_status ts = impl->vlm->set_stop_tokens(impl->stop_tokens_cache);
                if (ts != HAILO_SUCCESS)
                    return map_hailo_status(ts);
            }
            return HAL_OK;
        }
    }
    catch (...)
    {
        return HAL_ERROR;
    }
    return HAL_ERR_NOT_INITIALIZED;
}

static int genai_set_generator_params(HalGenaiSession *session, const HalGenaiGeneratorParams *params)
{
    if (!session || !params)
        return HAL_ERR_INVALID_ARG;
    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    impl->default_gen_params = *params;
    impl->has_default_gen_params = true;
    return HAL_OK;
}

static int genai_generate_stream(HalGenaiSession *session, const char *const *messages_json, int num_messages,
                                 const HalGenaiImageFrame *frames, int num_frames,
                                 const HalGenaiGeneratorParams *generator_params, HalGenaiTokenCallback on_token,
                                 void *token_user, HalGenaiFinishCallback on_finish, void *finish_user)
{
    if (!session || !messages_json || num_messages <= 0)
        return HAL_ERR_INVALID_ARG;

    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    if (impl->generating.load())
        return HAL_ERR_NOT_FINISHED;

    std::vector<std::string> msgs;
    msgs.reserve(static_cast<size_t>(num_messages));
    for (int i = 0; i < num_messages; ++i)
    {
        if (!messages_json[i])
            return HAL_ERR_INVALID_ARG;
        msgs.emplace_back(messages_json[i]);
    }

    const HalGenaiGeneratorParams gp = merge_gen_params(impl, generator_params);

    try
    {
        if (impl->kind == HAL_GENAI_KIND_LLM)
        {
            if (!impl->llm)
                return HAL_ERR_NOT_INITIALIZED;
            auto comp = generate_llm(impl, msgs, gp);
            run_completion_stream(impl, std::move(comp), on_token, token_user, on_finish, finish_user, true);
            return HAL_OK;
        }

        if (!impl->vlm)
            return HAL_ERR_NOT_INITIALIZED;

        const size_t expected_images = count_image_placeholders_heuristic(msgs);
        if (expected_images != static_cast<size_t>(num_frames))
            return HAL_ERR_INVALID_ARG;

        std::vector<hailort::MemoryView> mem_views;
        mem_views.reserve(static_cast<size_t>(num_frames));
        for (int i = 0; i < num_frames; ++i)
        {
            if (!frames[i].data || frames[i].byte_size == 0)
                return HAL_ERR_INVALID_ARG;
            mem_views.emplace_back(const_cast<uint8_t *>(frames[i].data), frames[i].byte_size);
        }

        auto comp = generate_vlm(impl, msgs, mem_views, gp);
        run_completion_stream(impl, std::move(comp), on_token, token_user, on_finish, finish_user, false);
        return HAL_OK;
    }
    catch (const hailort::hailort_error &e)
    {
        if (on_finish)
            on_finish(HAL_GENAI_FINISH_ERROR, map_hailo_status(e.status()), finish_user);
        return map_hailo_status(e.status());
    }
    catch (...)
    {
        if (on_finish)
            on_finish(HAL_GENAI_FINISH_ERROR, HAL_ERROR, finish_user);
        return HAL_ERROR;
    }
}

static int genai_abort_generation(HalGenaiSession *session)
{
    if (!session)
        return HAL_ERR_INVALID_ARG;
    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    std::lock_guard<std::mutex> lock(impl->mtx);
    if (!impl->completion)
        return HAL_OK;
    const hailo_status s = impl->completion->abort();
    return map_hailo_status(s);
}

static int genai_get_vlm_input_layout(HalGenaiSession *session, HalGenaiVlmInputLayout *out)
{
    if (!session || !out)
        return HAL_ERR_INVALID_ARG;
    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    if (impl->kind != HAL_GENAI_KIND_VLM || !impl->vlm)
        return HAL_ERR_INVALID_STATE;

    try
    {
        const auto &shape = impl->vlm->input_frame_shape();
        out->width = shape.width;
        out->height = shape.height;
        out->features = shape.features;
        out->bytes_per_frame = impl->vlm->input_frame_size();
        out->format_type = static_cast<uint32_t>(impl->vlm->input_frame_format_type());
        out->format_order = static_cast<uint32_t>(impl->vlm->input_frame_format_order());
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

static int genai_set_stop_tokens(HalGenaiSession *session, const char *const *utf8_sequences, int num_sequences)
{
    if (!session)
        return HAL_ERR_INVALID_ARG;
    if (num_sequences < 0)
        return HAL_ERR_INVALID_ARG;
    if (num_sequences > 0 && !utf8_sequences)
        return HAL_ERR_INVALID_ARG;
    for (int i = 0; i < num_sequences; ++i)
    {
        if (!utf8_sequences[i])
            return HAL_ERR_INVALID_ARG;
    }

    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    if (impl->generating.load())
        return HAL_ERR_INVALID_STATE;

    std::vector<std::string> tokens;
    tokens.reserve(static_cast<size_t>(num_sequences));
    for (int i = 0; i < num_sequences; ++i)
        tokens.emplace_back(utf8_sequences[i]);
    impl->stop_tokens_cache = std::move(tokens);

    try
    {
        if (impl->kind == HAL_GENAI_KIND_VLM && impl->vlm)
            return map_hailo_status(impl->vlm->set_stop_tokens(impl->stop_tokens_cache));
        if (impl->llm)
            return map_hailo_status(impl->llm->set_stop_tokens(impl->stop_tokens_cache));
    }
    catch (...)
    {
        return HAL_ERROR;
    }
    return HAL_ERR_NOT_INITIALIZED;
}

/* ---- M3: context (KV cache) persistence ---- */

static int genai_save_context(HalGenaiSession *session, void **buf, size_t *len)
{
    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    if (!impl || !buf || !len)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        hailort::Expected<hailort::BufferPtr> exp =
            (impl->kind == HAL_GENAI_KIND_VLM) ? impl->vlm->save_context() : impl->llm->save_context();
        if (!exp)
        {
            return map_hailo_status(exp.status());
        }
        hailort::BufferPtr b = exp.value();
        const size_t sz = b->size();
        void *out = std::malloc(sz ? sz : 1U);
        if (!out)
        {
            return HAL_ERR_NO_MEM;
        }
        std::memcpy(out, b->data(), sz);
        *buf = out;
        *len = sz;
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

static int genai_load_context(HalGenaiSession *session, const void *buf, size_t len)
{
    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    if (!impl || !buf || len == 0U)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        /* load_context takes a non-const MemoryView over caller memory; the SDK
         * documents it as read-only context data. */
        hailort::MemoryView view(const_cast<void *>(buf), len);
        const hailo_status st =
            (impl->kind == HAL_GENAI_KIND_VLM) ? impl->vlm->load_context(view) : impl->llm->load_context(view);
        return map_hailo_status(st);
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

static int genai_get_context_usage(HalGenaiSession *session, size_t *used, size_t *capacity)
{
    auto *impl = reinterpret_cast<HalGenaiSessionImpl *>(session);
    if (!impl || !used || !capacity)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        auto u = (impl->kind == HAL_GENAI_KIND_VLM) ? impl->vlm->get_context_usage_size()
                                                    : impl->llm->get_context_usage_size();
        if (!u)
        {
            return map_hailo_status(u.status());
        }
        auto c = (impl->kind == HAL_GENAI_KIND_VLM) ? impl->vlm->max_context_capacity()
                                                    : impl->llm->max_context_capacity();
        if (!c)
        {
            return map_hailo_status(c.status());
        }
        *used = u.value();
        *capacity = c.value();
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

static void genai_free_context_buffer(void *buf)
{
    std::free(buf);
}

static const char *genai_get_version(void)
{
    return "HAL-GenAI hailo15 (HailoRT GenAI)";
}

} // namespace

HalGenaiOps HAL_GENAI_OPS = {
    .create = genai_create,
    .destroy = genai_destroy,
    .clear_context = genai_clear_context,
    .set_generator_params = genai_set_generator_params,
    .generate_stream = genai_generate_stream,
    .abort_generation = genai_abort_generation,
    .get_vlm_input_layout = genai_get_vlm_input_layout,
    .set_stop_tokens = genai_set_stop_tokens,
    .save_context = genai_save_context,
    .load_context = genai_load_context,
    .get_context_usage = genai_get_context_usage,
    .free_context_buffer = genai_free_context_buffer,
    .get_version = genai_get_version,
};

#endif /* HAL_HAVE_HAL_GENAI */
