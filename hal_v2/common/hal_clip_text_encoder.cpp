/**
 * @file hal_clip_text_encoder.cpp
 * @brief CLIP text encoder implementation for HAL v2.
 */

#include "model/hal_clip_text_encoder.hpp"

#include "common/hal_common.h"
#include "common/hal_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numeric>
#include <unordered_map>

#if defined(HAL_HAVE_TOKENIZERS)
#include <tokenizers_cpp.h>
using namespace tokenizers;
#endif

namespace hal_v2
{

HalClipTextEncoderConfig HalClipTextEncoderConfig::default_config()
{
    HalClipTextEncoderConfig c;
    // Matches `clip_app_config.yaml` text_encoders → CLIP Vit B32 (device root /home/root/apps/clip).
    static const char kRoot[] = "/home/root/apps/clip/resources";
    c.hef_path = std::string(kRoot) + "/clip_vit_b_32_text_encoder.hef";
    c.tokenizer_json_path = std::string(kRoot) + "/tokenizer.json";
    c.embedding_lookup_bin_path = std::string(kRoot) + "/clip_vit_b32_embedding_lookup.bin";
    c.projection_weights_bin_path = std::string(kRoot) + "/clip_vit_b32_text_projection_weights.bin";
    c.projection_bias_bin_path = std::string(kRoot) + "/clip_vit_b32_text_projection_bias.bin";
    c.context_length = 77;
    c.embedding_size = 512;
    c.token_start_id = kHalClipTokenStartId;
    c.token_end_id = kHalClipTokenEndId;
    return c;
}

namespace
{

#if defined(HAL_HAVE_TOKENIZERS)
static std::string read_file_to_string(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 0)
        return {};
    f.seekg(0, std::ios::beg);
    std::string s;
    s.resize((size_t)sz);
    f.read(s.data(), sz);
    if (!f)
        return {};
    return s;
}
#endif

struct ClipBinMatrix
{
    uint32_t rows = 0;
    uint32_t cols = 0;
    std::vector<float> data; // row-major

    bool load(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        f.read(reinterpret_cast<char *>(&rows), sizeof(uint32_t));
        f.read(reinterpret_cast<char *>(&cols), sizeof(uint32_t));
        if (!f || rows == 0 || cols == 0)
            return false;
        data.resize((size_t)rows * (size_t)cols);
        f.read(reinterpret_cast<char *>(data.data()), (std::streamsize)(data.size() * sizeof(float)));
        return (bool)f;
    }

    const float *row_ptr(uint32_t r) const
    {
        if (r >= rows)
            return nullptr;
        return &data[(size_t)r * (size_t)cols];
    }

    float at(uint32_t r, uint32_t c) const
    {
        return data[(size_t)r * (size_t)cols + (size_t)c];
    }

    /** Same as reference `ClipBinMatrix::operator[]` — row pointer for token id, or nullptr. */
    const float *operator[](int token_id) const
    {
        if (token_id < 0 || static_cast<uint32_t>(token_id) >= rows)
            return nullptr;
        return &data[static_cast<size_t>(token_id) * static_cast<size_t>(cols)];
    }
};

// Note: normalization is applied on the final projected embedding in encode_prompt().

static void l2_normalize_inplace(std::vector<float> &v)
{
    double s = 0.0;
    for (float x : v)
        s += static_cast<double>(x) * static_cast<double>(x);
    if (s <= 0.0)
        return;
    const float inv = static_cast<float>(1.0 / std::sqrt(s));
    for (float &x : v)
        x *= inv;
}

#if defined(HAL_HAVE_TOKENIZERS)
/**
 * Derive text-encoder I/O widths from HEF stream sizes (authoritative), not from embedding_lookup.bin cols.
 * Example: ViT-B/32 text HEF may expect float32 [1,77,256] input while the lookup table still stores 512-dim rows.
 */
struct ClipTextHeDims
{
    uint32_t seq_len = 0;
    uint32_t in_token_dim = 0;
    uint32_t hidden_dim = 0;
};

/** Logical element count for seq×feat splitting (matches HEF stream frame layout). */
static bool clip_text_input_logical_elems(const HalModelTensorInfo &ii, uint32_t &out_elems, uint32_t &storage_bpe)
{
    switch (ii.dtype)
    {
    case HAL_DTYPE_FLOAT32:
        if (ii.byte_size < 4u || (ii.byte_size % 4u) != 0u)
            return false;
        storage_bpe = 4u;
        out_elems = ii.byte_size / 4u;
        return true;
    case HAL_DTYPE_UINT16:
    case HAL_DTYPE_INT16:
        if (ii.byte_size < 2u || (ii.byte_size % 2u) != 0u)
            return false;
        storage_bpe = 2u;
        out_elems = ii.byte_size / 2u;
        return true;
    case HAL_DTYPE_UINT8:
    case HAL_DTYPE_INT8:
        storage_bpe = 1u;
        out_elems = ii.byte_size;
        return true;
    case HAL_DTYPE_UNKNOWN:
        /* Prefer float32 host layout when unmapped; same byte_size can also be uint16 (e.g. 77×512 vs 77×256×4). */
        if (ii.byte_size >= 4u && (ii.byte_size % 4u) == 0u)
        {
            storage_bpe = 4u;
            out_elems = ii.byte_size / 4u;
            return true;
        }
        if (ii.byte_size >= 2u && (ii.byte_size % 2u) == 0u)
        {
            storage_bpe = 2u;
            out_elems = ii.byte_size / 2u;
            return true;
        }
        return false;
    default:
        return false;
    }
}

static uint16_t clip_quant_f32_to_u16(float f, float scale, float zp)
{
    const double s = (scale != 0.0f) ? static_cast<double>(scale) : 1.0;
    long long q = std::llround(static_cast<double>(f) / s + static_cast<double>(zp));
    if (q < 0)
        q = 0;
    if (q > 65535)
        q = 65535;
    return static_cast<uint16_t>(q);
}

static int16_t clip_quant_f32_to_i16(float f, float scale, float zp)
{
    const double s = (scale != 0.0f) ? static_cast<double>(scale) : 1.0;
    long long q = std::llround(static_cast<double>(f) / s + static_cast<double>(zp));
    if (q < -32768)
        q = -32768;
    if (q > 32767)
        q = 32767;
    return static_cast<int16_t>(q);
}

static bool clip_text_output_elems(const HalModelTensorInfo &oo, uint32_t &out_elems, bool &out_is_float_storage)
{
    if (oo.dtype == HAL_DTYPE_FLOAT32 && oo.byte_size >= 4u && (oo.byte_size % 4u) == 0u)
    {
        out_elems = oo.byte_size / 4u;
        out_is_float_storage = true;
        return true;
    }
    if (oo.dtype == HAL_DTYPE_UNKNOWN && oo.byte_size >= 4u && (oo.byte_size % 4u) == 0u)
    {
        out_elems = oo.byte_size / 4u;
        out_is_float_storage = true;
        return true;
    }
    if ((oo.dtype == HAL_DTYPE_UINT16 || oo.dtype == HAL_DTYPE_INT16) && oo.byte_size >= 2u &&
        (oo.byte_size % 2u) == 0u)
    {
        out_elems = oo.byte_size / 2u;
        out_is_float_storage = false;
        return true;
    }
    if (oo.dtype == HAL_DTYPE_UINT8 || oo.dtype == HAL_DTYPE_INT8)
    {
        out_elems = oo.byte_size;
        out_is_float_storage = false;
        return true;
    }
    return false;
}

static bool clip_text_try_split_seq_feat(uint32_t total_elems, uint32_t prefer_seq, uint32_t &seq_len,
                                         uint32_t &feat_dim)
{
    auto plausible = [](uint32_t s, uint32_t f) -> bool {
        return s >= 1u && s <= 512u && f >= 8u && f <= 8192u;
    };

    if (prefer_seq > 0u && total_elems % prefer_seq == 0u)
    {
        const uint32_t f = total_elems / prefer_seq;
        if (plausible(prefer_seq, f))
        {
            seq_len = prefer_seq;
            feat_dim = f;
            return true;
        }
    }

    static const uint32_t kCommonSeq[] = {77u, 76u, 128u, 64u, 32u, 256u};
    for (uint32_t s : kCommonSeq)
    {
        if (s == 0u || total_elems % s != 0u)
            continue;
        const uint32_t f = total_elems / s;
        if (plausible(s, f))
        {
            seq_len = s;
            feat_dim = f;
            return true;
        }
    }
    return false;
}

static bool clip_text_he_dims_from_model(const HalModelInfo &mi, uint32_t cfg_ctx_len, ClipTextHeDims &out)
{
    if (mi.num_inputs < 1u || mi.num_outputs < 1u)
    {
        HAL_LOG_ERROR("HalClipTextEncoder: HEF model info missing inputs/outputs (in=%u out=%u)", mi.num_inputs,
                      mi.num_outputs);
        return false;
    }
    const HalModelTensorInfo &ii = mi.inputs[0];
    const HalModelTensorInfo &oo = mi.outputs[0];

    uint32_t in_logical_elems = 0;
    uint32_t in_storage_bpe = 0;
    if (!clip_text_input_logical_elems(ii, in_logical_elems, in_storage_bpe))
    {
        HAL_LOG_ERROR("HalClipTextEncoder: text input stream dtype=%d byte_size=%u (unsupported layout)", (int)ii.dtype,
                      ii.byte_size);
        return false;
    }

    uint32_t seq_len = 0;
    uint32_t in_token_dim = 0;

    if (ii.ndim >= 4 && ii.shape[3] > 0)
    {
        const uint32_t f = static_cast<uint32_t>(ii.shape[3]);
        const uint32_t h = std::max<int32_t>(1, ii.shape[1]);
        const uint32_t w = std::max<int32_t>(1, ii.shape[2]);
        const uint64_t prod = static_cast<uint64_t>(h) * static_cast<uint64_t>(w) * static_cast<uint64_t>(f);
        if (prod == static_cast<uint64_t>(in_logical_elems) && h * w >= 1u && h * w <= 512u && f >= 8u && f <= 8192u)
        {
            in_token_dim = f;
            seq_len = h * w;
        }
    }

    if (in_token_dim == 0u)
    {
        if (!clip_text_try_split_seq_feat(in_logical_elems, cfg_ctx_len, seq_len, in_token_dim))
        {
            HAL_LOG_ERROR("HalClipTextEncoder: cannot split text input logical_elems=%u (bpe=%u) ctx=%u (ndim=%d)",
                          in_logical_elems, in_storage_bpe, cfg_ctx_len, (int)ii.ndim);
            return false;
        }
    }

    uint32_t out_elems = 0;
    bool out_float = false;
    if (!clip_text_output_elems(oo, out_elems, out_float))
    {
        HAL_LOG_ERROR("HalClipTextEncoder: text output stream dtype=%d byte_size=%u not handled", (int)oo.dtype,
                      oo.byte_size);
        return false;
    }

    uint32_t hidden_dim = 0;
    if (out_float)
    {
        if (out_elems % seq_len != 0u)
        {
            HAL_LOG_ERROR(
                "HalClipTextEncoder: text output float elems=%u not divisible by seq_len=%u (out dtype=%d ndim=%d "
                "byte_size=%u)",
                out_elems, seq_len, (int)oo.dtype, (int)oo.ndim, oo.byte_size);
            return false;
        }
        hidden_dim = out_elems / seq_len;
    }
    else
    {
        if (out_elems % seq_len != 0u)
        {
            HAL_LOG_ERROR("HalClipTextEncoder: quantized text output bytes=%u not divisible by seq_len=%u", out_elems,
                          seq_len);
            return false;
        }
        hidden_dim = out_elems / seq_len;
    }

    out.seq_len = seq_len;
    out.in_token_dim = in_token_dim;
    out.hidden_dim = hidden_dim;
    return true;
}
#endif

} // namespace

struct HalClipTextEncoder::Impl
{
    HalClipTextEncoderConfig cfg{};

    HalInferenceSession *infer = nullptr;
    HalModelInfo mi{};

#if defined(HAL_HAVE_TOKENIZERS)
    std::shared_ptr<Tokenizer> tokenizer;
    /** Per HEF input stream: token embedding width (may differ from embedding_lookup.cols). */
    uint32_t text_in_token_dim = 0;
    /** Sequence length from HEF (usually 77). */
    uint32_t text_seq_len = 0;
    /** Per-position transformer hidden width from HEF output (used for EOT slice + projection rows). */
    uint32_t text_hidden_dim = 0;
    /** Declared HEF input dtype (UINT16 token stream vs float32). */
    HalDataType text_in_stream_dtype = HAL_DTYPE_FLOAT32;
    float text_in_qscale = 1.0f;
    float text_in_qzp = 0.0f;
    std::vector<uint16_t> text_in_buf_u16;
    std::vector<int16_t> text_in_buf_i16;
#endif
    ClipBinMatrix embedding_lookup;
    ClipBinMatrix proj_w;
    ClipBinMatrix proj_b;

    uint32_t embedding_dim = 0;

#if defined(HAL_HAVE_TOKENIZERS)
    /** Serializes NPU text-encoder runs and tokenizer use across threads. */
    std::mutex infer_mu;
#endif

    // Same math as reference `ClipTextEncoder::apply_text_projection` (matvec then bias).
    std::vector<float> apply_text_projection(const std::vector<float> &last_hidden_state) const
    {
        const uint32_t output_dim = proj_w.cols;
        const uint32_t input_dim = proj_w.rows;
        std::vector<float> projected(output_dim, 0.0f);

        for (uint32_t i = 0; i < output_dim; i++)
        {
            float sum = 0.0f;
            for (uint32_t j = 0; j < input_dim; j++)
                sum += last_hidden_state[j] * proj_w.at(j, i);
            projected[i] = sum;
        }
        for (uint32_t i = 0; i < output_dim; i++)
            projected[i] += proj_b.data[i];
        return projected;
    }

#if defined(HAL_HAVE_TOKENIZERS)
    /**
     * Aligns with `ClipTextEncoder::build_sentence_embedding`: SOS, tokens, EOS, pad id 0; lookup each row.
     */
    std::vector<float> build_sentence_embedding(const std::vector<int> &tokens, uint32_t &out_eot_index) const
    {
        const uint32_t max_len = text_seq_len;
        const uint32_t dim = text_in_token_dim;
        const uint32_t src_cols = embedding_lookup.cols;
        const uint32_t copy_w = std::min(src_cols, dim);
        std::vector<float> tensor((size_t)max_len * (size_t)dim, 0.0f);

        std::vector<int> token_ids;
        token_ids.reserve(max_len);

        token_ids.push_back(static_cast<int>(cfg.token_start_id));
        for (int t : tokens)
        {
            if (static_cast<int>(token_ids.size()) >= static_cast<int>(max_len) - 1)
                break;
            token_ids.push_back(t);
        }
        token_ids.push_back(static_cast<int>(cfg.token_end_id));

        out_eot_index = static_cast<uint32_t>(token_ids.size() - 1);

        while (static_cast<int>(token_ids.size()) < static_cast<int>(max_len))
            token_ids.push_back(0);

        for (int i = 0; i < static_cast<int>(max_len); i++)
        {
            const float *emb = embedding_lookup[token_ids[i]];
            if (!emb)
                continue;
            std::memcpy(&tensor[static_cast<size_t>(i) * dim], emb, (size_t)copy_w * sizeof(float));
        }
        return tensor;
    }
#endif

    int infer_text_encoder(const std::vector<float> &sentence_embedding, std::vector<float> &out_last_hidden_state)
    {
        if (!infer)
            return HAL_ERR_NOT_READY;

#if defined(HAL_HAVE_TOKENIZERS)
        const uint32_t expected_bytes = mi.inputs[0].byte_size;
        const uint32_t n_float = text_seq_len * text_in_token_dim;
        if (expected_bytes == 0u || sentence_embedding.size() != (size_t)n_float)
        {
            HAL_LOG_ERROR("HalClipTextEncoder: text float embedding count mismatch (got=%zu expected=%u)",
                          sentence_embedding.size(), n_float);
            return HAL_ERR_INVALID_SIZE;
        }

        const float in_scale = (text_in_qscale != 0.0f) ? text_in_qscale : 1.0f;
        const float in_zp = text_in_qzp;

        HalTensor in{};
        in.ndim = 3;
        in.shape[0] = 1;
        in.shape[1] = (int32_t)text_seq_len;
        in.shape[2] = (int32_t)text_in_token_dim;
        in.byte_size = expected_bytes;
        in.dma_fd = -1;

        switch (text_in_stream_dtype)
        {
        case HAL_DTYPE_FLOAT32:
        case HAL_DTYPE_UNKNOWN:
            if ((uint32_t)(sentence_embedding.size() * sizeof(float)) != expected_bytes)
            {
                HAL_LOG_ERROR("HalClipTextEncoder: text input bytes mismatch (got=%zu expected=%u)",
                              sentence_embedding.size() * sizeof(float), expected_bytes);
                return HAL_ERR_INVALID_SIZE;
            }
            in.data = (void *)sentence_embedding.data();
            in.dtype = HAL_DTYPE_FLOAT32;
            break;
        case HAL_DTYPE_UINT16:
        {
            text_in_buf_u16.resize(n_float);
            for (uint32_t i = 0; i < n_float; i++)
                text_in_buf_u16[i] = clip_quant_f32_to_u16(sentence_embedding[i], in_scale, in_zp);
            if (text_in_buf_u16.size() * sizeof(uint16_t) != (size_t)expected_bytes)
            {
                HAL_LOG_ERROR("HalClipTextEncoder: internal uint16 input size mismatch");
                return HAL_ERR_INVALID_SIZE;
            }
            in.data = text_in_buf_u16.data();
            in.dtype = HAL_DTYPE_UINT16;
            break;
        }
        case HAL_DTYPE_INT16:
        {
            text_in_buf_i16.resize(n_float);
            for (uint32_t i = 0; i < n_float; i++)
                text_in_buf_i16[i] = clip_quant_f32_to_i16(sentence_embedding[i], in_scale, in_zp);
            if (text_in_buf_i16.size() * sizeof(int16_t) != (size_t)expected_bytes)
            {
                HAL_LOG_ERROR("HalClipTextEncoder: internal int16 input size mismatch");
                return HAL_ERR_INVALID_SIZE;
            }
            in.data = text_in_buf_i16.data();
            in.dtype = HAL_DTYPE_INT16;
            break;
        }
        default:
            HAL_LOG_ERROR("HalClipTextEncoder: text input dtype=%d not supported for inference", (int)text_in_stream_dtype);
            return HAL_ERR_NOT_SUPPORTED;
        }
#else
        HalTensor in{};
        in.data = (void *)sentence_embedding.data();
        in.ndim = 3;
        in.shape[0] = 1;
        in.shape[1] = (int32_t)cfg.context_length;
        in.shape[2] = (int32_t)embedding_lookup.cols;
        in.dtype = HAL_DTYPE_FLOAT32;
        in.byte_size = (uint32_t)(sentence_embedding.size() * sizeof(float));
        in.dma_fd = -1;
#endif

        HalTensor out[HAL_MAX_TENSORS]{};
        const int rc = HAL_INFERENCE_OPS.run(infer, &in, 1, out, (int)mi.num_outputs);
        if (rc != HAL_OK)
            return rc;

        // Use first output.
        if (mi.num_outputs < 1 || !out[0].data || out[0].byte_size == 0)
        {
            HAL_INFERENCE_OPS.free_tensor(&out[0]);
            return HAL_ERR_NOT_READY;
        }

        const uint32_t out_bytes = out[0].byte_size;
        const auto &ti = mi.outputs[0];
        const float scale = (ti.quant_scale != 0.0f) ? ti.quant_scale : 1.0f;
        const float zp = ti.quant_zero_point;

        // Dequantize to float if needed.
        out_last_hidden_state.clear();
        if (out[0].dtype == HAL_DTYPE_FLOAT32)
        {
            const uint32_t n = out_bytes / sizeof(float);
            out_last_hidden_state.resize(n);
            std::memcpy(out_last_hidden_state.data(), out[0].data, n * sizeof(float));
        }
        else if (out[0].dtype == HAL_DTYPE_UINT8)
        {
            const uint32_t n = out_bytes / sizeof(uint8_t);
            out_last_hidden_state.resize(n);
            const auto *p = static_cast<const uint8_t *>(out[0].data);
            for (uint32_t i = 0; i < n; i++)
                out_last_hidden_state[i] = (p[i] - zp) * scale;
        }
        else if (out[0].dtype == HAL_DTYPE_INT8)
        {
            const uint32_t n = out_bytes / sizeof(int8_t);
            out_last_hidden_state.resize(n);
            const auto *p = static_cast<const int8_t *>(out[0].data);
            for (uint32_t i = 0; i < n; i++)
                out_last_hidden_state[i] = ((float)p[i] - zp) * scale;
        }
        else if (out[0].dtype == HAL_DTYPE_UINT16)
        {
            const uint32_t n = out_bytes / sizeof(uint16_t);
            out_last_hidden_state.resize(n);
            const auto *p = static_cast<const uint16_t *>(out[0].data);
            for (uint32_t i = 0; i < n; i++)
                out_last_hidden_state[i] = ((float)p[i] - zp) * scale;
        }
        else if (out[0].dtype == HAL_DTYPE_INT16)
        {
            const uint32_t n = out_bytes / sizeof(int16_t);
            out_last_hidden_state.resize(n);
            const auto *p = static_cast<const int16_t *>(out[0].data);
            for (uint32_t i = 0; i < n; i++)
                out_last_hidden_state[i] = ((float)p[i] - zp) * scale;
        }
        else if (out[0].dtype == HAL_DTYPE_UNKNOWN && out_bytes >= 4u && (out_bytes % 4u) == 0u)
        {
            const uint32_t n = out_bytes / sizeof(float);
            out_last_hidden_state.resize(n);
            std::memcpy(out_last_hidden_state.data(), out[0].data, n * sizeof(float));
        }
        else
        {
            HAL_INFERENCE_OPS.free_tensor(&out[0]);
            return HAL_ERR_NOT_SUPPORTED;
        }

        HAL_INFERENCE_OPS.free_tensor(&out[0]);
        return HAL_OK;
    }
};

#if defined(HAL_HAVE_TOKENIZERS)
int HalClipTextEncoder::encode_prompt_impl(Impl *m_impl, const std::string &prompt,
                                           std::vector<float> &out_embedding)
{
    const std::vector<int> enc = m_impl->tokenizer->Encode(prompt);
    if (enc.empty())
        return HAL_ERR_RESULT;

    uint32_t eot_index = 0;
    const std::vector<float> sentence_emb = m_impl->build_sentence_embedding(enc, eot_index);

    std::vector<float> last_hidden;
    const int rc = m_impl->infer_text_encoder(sentence_emb, last_hidden);
    if (rc != HAL_OK)
        return rc;

    const uint32_t dim = m_impl->text_hidden_dim;
    const size_t base = (size_t)eot_index * (size_t)dim;
    if (base + dim > last_hidden.size())
        return HAL_ERR_RESULT;

    std::vector<float> eot_hidden(dim);
    std::memcpy(eot_hidden.data(), &last_hidden[base], dim * sizeof(float));

    out_embedding = m_impl->apply_text_projection(eot_hidden);
    l2_normalize_inplace(out_embedding);
    return HAL_OK;
}
#endif

HalClipTextEncoder::~HalClipTextEncoder()
{
    deinit();
}

int HalClipTextEncoder::init()
{
    return init(HalClipTextEncoderConfig::default_config());
}

int HalClipTextEncoder::init(const HalClipTextEncoderConfig &cfg)
{
    deinit();
    m_impl = new (std::nothrow) Impl();
    if (!m_impl)
        return HAL_ERR_NO_MEM;
    m_impl->cfg = cfg;

#if !defined(HAL_HAVE_TOKENIZERS)
    (void)cfg;
    HAL_LOG_ERROR("HalClipTextEncoder: tokenizers not available at build time");
    delete m_impl;
    m_impl = nullptr;
    return HAL_ERR_NOT_SUPPORTED;
#else
    // Tokenizer
    const std::string tok_json = read_file_to_string(cfg.tokenizer_json_path);
    if (tok_json.empty())
        return HAL_ERR_NOT_FOUND;
    auto tok = Tokenizer::FromBlobJSON(tok_json);
    if (!tok)
        return HAL_ERR_RESULT;
    m_impl->tokenizer = std::move(tok);

    // Binaries
    if (!m_impl->embedding_lookup.load(cfg.embedding_lookup_bin_path))
        return HAL_ERR_NOT_FOUND;
    if (!m_impl->proj_w.load(cfg.projection_weights_bin_path))
        return HAL_ERR_NOT_FOUND;
    if (!m_impl->proj_b.load(cfg.projection_bias_bin_path))
        return HAL_ERR_NOT_FOUND;

    // Inference session
    HalInferenceConfig ic{};
    std::snprintf(ic.model_path, sizeof(ic.model_path), "%s", cfg.hef_path.c_str());
    ic.batch_size = 1;
    ic.timeout_ms = 1000;
    ic.use_dma = false;
    m_impl->infer = HAL_INFERENCE_OPS.create(&ic);
    if (!m_impl->infer)
        return HAL_ERR_RESULT;
    if (HAL_INFERENCE_OPS.get_model_info(m_impl->infer, &m_impl->mi) != HAL_OK)
        return HAL_ERR_RESULT;

    ClipTextHeDims hed{};
    if (!clip_text_he_dims_from_model(m_impl->mi, m_impl->cfg.context_length, hed))
    {
        HAL_LOG_ERROR("HalClipTextEncoder: could not derive text encoder I/O dims from HEF");
        HAL_INFERENCE_OPS.destroy(m_impl->infer);
        m_impl->infer = nullptr;
        delete m_impl;
        m_impl = nullptr;
        return HAL_ERR_RESULT;
    }
    m_impl->text_seq_len = hed.seq_len;
    m_impl->text_in_token_dim = hed.in_token_dim;
    m_impl->text_hidden_dim = hed.hidden_dim;
    m_impl->cfg.context_length = hed.seq_len;
    m_impl->text_in_stream_dtype = m_impl->mi.inputs[0].dtype;
    m_impl->text_in_qscale = m_impl->mi.inputs[0].quant_scale;
    m_impl->text_in_qzp = m_impl->mi.inputs[0].quant_zero_point;

    if (m_impl->embedding_lookup.cols < m_impl->text_in_token_dim)
    {
        HAL_LOG_ERROR(
            "HalClipTextEncoder: embedding_lookup cols=%u < HEF input token dim=%u (wrong clip_* resource files?)",
            m_impl->embedding_lookup.cols, m_impl->text_in_token_dim);
        HAL_INFERENCE_OPS.destroy(m_impl->infer);
        m_impl->infer = nullptr;
        delete m_impl;
        m_impl = nullptr;
        return HAL_ERR_INVALID_ARG;
    }
    if (m_impl->embedding_lookup.cols != m_impl->text_in_token_dim)
    {
        HAL_LOG_WARNING("HalClipTextEncoder: embedding_lookup cols=%u, HEF expects token dim=%u — using first %u floats per row",
                        m_impl->embedding_lookup.cols, m_impl->text_in_token_dim, m_impl->text_in_token_dim);
    }
    if (m_impl->proj_w.rows != m_impl->text_hidden_dim)
    {
        HAL_LOG_ERROR(
            "HalClipTextEncoder: projection weights rows=%u != HEF hidden dim=%u (check clip_*text_projection*.bin)",
            m_impl->proj_w.rows, m_impl->text_hidden_dim);
        HAL_INFERENCE_OPS.destroy(m_impl->infer);
        m_impl->infer = nullptr;
        delete m_impl;
        m_impl = nullptr;
        return HAL_ERR_INVALID_ARG;
    }
    if (m_impl->cfg.embedding_size != 0u && m_impl->proj_w.cols != m_impl->cfg.embedding_size)
    {
        HAL_LOG_ERROR("HalClipTextEncoder: projection cols=%u != config embedding_size=%u", m_impl->proj_w.cols,
                      m_impl->cfg.embedding_size);
        HAL_INFERENCE_OPS.destroy(m_impl->infer);
        m_impl->infer = nullptr;
        delete m_impl;
        m_impl = nullptr;
        return HAL_ERR_INVALID_ARG;
    }

    // Determine embedding dim from projection weights output dim.
    m_impl->embedding_dim = m_impl->proj_w.cols;
    m_embedding_dim = m_impl->embedding_dim;
    m_ready = true;
    HAL_LOG_INFO("HalClipTextEncoder: HEF text seq=%u in_dim=%u in_dtype=%d hidden=%u proj_out=%u q_scale=%g q_zp=%g",
                 m_impl->text_seq_len, m_impl->text_in_token_dim, (int)m_impl->text_in_stream_dtype,
                 m_impl->text_hidden_dim, m_impl->embedding_dim, (double)m_impl->text_in_qscale,
                 (double)m_impl->text_in_qzp);
    return HAL_OK;
#endif
}

void HalClipTextEncoder::deinit()
{
    if (!m_impl)
        return;
#if defined(HAL_HAVE_TOKENIZERS)
    std::lock_guard<std::mutex> lk(m_impl->infer_mu);
#endif
    if (m_impl->infer)
        HAL_INFERENCE_OPS.destroy(m_impl->infer);
    m_impl->infer = nullptr;
    delete m_impl;
    m_impl = nullptr;
    m_ready = false;
    m_embedding_dim = 0;
}

int HalClipTextEncoder::encode_prompt(const std::string &prompt, std::vector<float> &out_embedding)
{
    if (!m_impl || !m_ready)
        return HAL_ERR_NOT_READY;

#if !defined(HAL_HAVE_TOKENIZERS)
    (void)prompt;
    (void)out_embedding;
    return HAL_ERR_NOT_SUPPORTED;
#else
    std::lock_guard<std::mutex> lk(m_impl->infer_mu);
    return encode_prompt_impl(m_impl, prompt, out_embedding);
#endif
}

int HalClipTextEncoder::encode_prompts(const std::vector<std::string> &prompts,
                                       std::vector<std::vector<float>> &out_embeddings)
{
    if (!m_impl || !m_ready)
        return HAL_ERR_NOT_READY;

#if !defined(HAL_HAVE_TOKENIZERS)
    (void)prompts;
    (void)out_embeddings;
    return HAL_ERR_NOT_SUPPORTED;
#else
    std::lock_guard<std::mutex> lk(m_impl->infer_mu);
    out_embeddings.clear();
    out_embeddings.reserve(prompts.size());
    for (const auto &p : prompts)
    {
        std::vector<float> emb;
        const int rc = encode_prompt_impl(m_impl, p, emb);
        if (rc != HAL_OK)
            return rc;
        out_embeddings.push_back(std::move(emb));
    }
    return HAL_OK;
#endif
}

} // namespace hal_v2

// ── C-linkage ops for dynamic loading ──────────────────────────────────────────

#include "model/hal_clip_text_encoder_ops.h"

struct HalClipTextEncoderHandle_ {
    hal_v2::HalClipTextEncoder enc;
};

static HalClipTextEncoderHandle *clip_enc_create(void) {
    auto *h = new (std::nothrow) HalClipTextEncoderHandle;
    if (!h) return nullptr;
    if (h->enc.init() != 0) {
        delete h;
        return nullptr;
    }
    return h;
}

static void clip_enc_destroy(HalClipTextEncoderHandle *h) {
    delete h;
}

static int clip_enc_encode(HalClipTextEncoderHandle *h, const char *text,
                           float *out_buf, uint32_t *out_dim) {
    if (!h || !text || !out_buf || !out_dim) return -1;
    std::vector<float> emb;
    int rc = h->enc.encode_prompt(text, emb);
    if (rc != 0) return rc;
    uint32_t dim = static_cast<uint32_t>(emb.size());
    if (*out_dim < dim) { *out_dim = dim; return -2; }
    std::memcpy(out_buf, emb.data(), dim * sizeof(float));
    *out_dim = dim;
    return 0;
}

static uint32_t clip_enc_embedding_dim(HalClipTextEncoderHandle *h) {
    if (!h) return 0;
    return h->enc.embedding_dim();
}

extern "C" {

HalClipTextEncoderOps HAL_CLIP_TEXT_ENCODER_OPS = {
    clip_enc_create,
    clip_enc_destroy,
    clip_enc_encode,
    clip_enc_embedding_dim,
};

} // extern "C"

