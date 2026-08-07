/**
 * @file hailo15_audio_impl.cpp
 * @brief Hailo-15 HAL audio — ALSA capture + libavcodec encode (AAC / G.711).
 */

#include "common/hal_log.h"
#include "media/hal_audio_internal.h"

#include <alsa/asoundlib.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr uint32_t kDefaultSampleRate = 48000u;
constexpr uint32_t kDefaultChannels = 1u;
constexpr uint32_t kDefaultPeriodFrames = 1024u;
constexpr uint32_t kDefaultAacBitrate = 128000u;

struct HalAudioHeapBuffer
{
    std::vector<uint8_t> bytes;
};

struct HalAudioPacketPriv
{
    std::vector<uint8_t> bytes;
};

struct Hailo15AudioPriv
{
    std::mutex mutex;
    HalAudioConfig effective{};
    snd_pcm_t *pcm{nullptr};
    std::thread worker;
    std::atomic<bool> running{false};
    bool playback_open{false};
    uint64_t pcm_seq{0};
    uint64_t pkt_seq{0};

    const AVCodec *av_codec{nullptr};
    AVCodecContext *avctx{nullptr};
    SwrContext *swr{nullptr};
    AVFrame *frame{nullptr};
    AVPacket *packet{nullptr};
    int encoder_sample_rate{0};
    AVSampleFormat encoder_sample_fmt{AV_SAMPLE_FMT_NONE};
};

static HalAudioContext *ctx_ptr(void *audio_ctx)
{
    return static_cast<HalAudioContext *>(audio_ctx);
}

static int hailo15_audio_stop(void *audio_ctx);

static Hailo15AudioPriv *audio_priv(HalAudioContext *ac)
{
    if (!ac || !ac->priv)
    {
        return nullptr;
    }
    return static_cast<Hailo15AudioPriv *>(ac->priv);
}

static void apply_config_defaults(HalAudioConfig *cfg)
{
    if (!cfg->device[0])
    {
        std::strncpy(cfg->device, "default", sizeof(cfg->device) - 1);
    }
    if (cfg->sample_rate == 0u)
    {
        cfg->sample_rate = kDefaultSampleRate;
    }
    if (cfg->channels == 0u)
    {
        cfg->channels = kDefaultChannels;
    }
    if (cfg->channels > 2u)
    {
        cfg->channels = 2u;
    }
    if (cfg->period_frames == 0u)
    {
        cfg->period_frames = kDefaultPeriodFrames;
    }
    if (cfg->bitrate == 0u && cfg->codec == HAL_AUDIO_CODEC_AAC)
    {
        cfg->bitrate = kDefaultAacBitrate;
    }
    if (cfg->volume <= 0.0f)
    {
        cfg->volume = 1.0f;
    }
}

static snd_pcm_format_t alsa_format_from_hal(HalAudioSampleFormat fmt)
{
    switch (fmt)
    {
        case HAL_AUDIO_SAMPLE_FMT_S32LE:
            return SND_PCM_FORMAT_S32_LE;
        case HAL_AUDIO_SAMPLE_FMT_F32LE:
            return SND_PCM_FORMAT_FLOAT_LE;
        case HAL_AUDIO_SAMPLE_FMT_S16LE:
        default:
            return SND_PCM_FORMAT_S16_LE;
    }
}

static AVSampleFormat av_sample_fmt_from_hal(HalAudioSampleFormat fmt)
{
    switch (fmt)
    {
        case HAL_AUDIO_SAMPLE_FMT_S32LE:
            return AV_SAMPLE_FMT_S32;
        case HAL_AUDIO_SAMPLE_FMT_F32LE:
            return AV_SAMPLE_FMT_FLT;
        case HAL_AUDIO_SAMPLE_FMT_S16LE:
        default:
            return AV_SAMPLE_FMT_S16;
    }
}

static HalPacketType packet_type_from_codec(HalAudioCodecType codec)
{
    switch (codec)
    {
        case HAL_AUDIO_CODEC_AAC:
            return HAL_PACKET_TYPE_AAC;
        case HAL_AUDIO_CODEC_G711A:
            return HAL_PACKET_TYPE_G711A;
        case HAL_AUDIO_CODEC_G711U:
            return HAL_PACKET_TYPE_G711U;
        case HAL_AUDIO_CODEC_PCM:
        default:
            return HAL_PACKET_TYPE_PCM;
    }
}

static enum AVCodecID av_codec_id_from_hal(HalAudioCodecType codec)
{
    switch (codec)
    {
        case HAL_AUDIO_CODEC_AAC:
            return AV_CODEC_ID_AAC;
        case HAL_AUDIO_CODEC_G711A:
            return AV_CODEC_ID_PCM_ALAW;
        case HAL_AUDIO_CODEC_G711U:
            return AV_CODEC_ID_PCM_MULAW;
        case HAL_AUDIO_CODEC_PCM:
        default:
            return AV_CODEC_ID_NONE;
    }
}

static void destroy_encoder(Hailo15AudioPriv *priv)
{
    if (!priv)
    {
        return;
    }
    if (priv->packet)
    {
        av_packet_free(&priv->packet);
        priv->packet = nullptr;
    }
    if (priv->frame)
    {
        av_frame_free(&priv->frame);
        priv->frame = nullptr;
    }
    if (priv->swr)
    {
        swr_free(&priv->swr);
        priv->swr = nullptr;
    }
    if (priv->avctx)
    {
        avcodec_free_context(&priv->avctx);
        priv->avctx = nullptr;
    }
    priv->av_codec = nullptr;
}

static int setup_encoder(Hailo15AudioPriv *priv, const HalAudioConfig *cfg)
{
    if (!priv || !cfg || cfg->codec == HAL_AUDIO_CODEC_PCM)
    {
        return HAL_OK;
    }

    const enum AVCodecID cid = av_codec_id_from_hal(cfg->codec);
    priv->av_codec = avcodec_find_encoder(cid);
    if (!priv->av_codec)
    {
        HAL_LOG_ERROR("audio: avcodec_find_encoder failed for codec %d", static_cast<int>(cfg->codec));
        return HAL_ERR_NOT_SUPPORTED;
    }

    priv->avctx = avcodec_alloc_context3(priv->av_codec);
    if (!priv->avctx)
    {
        return HAL_ERR_NO_MEM;
    }

    priv->avctx->sample_rate = static_cast<int>(cfg->sample_rate);
    priv->avctx->channel_layout = av_get_default_channel_layout(static_cast<int>(cfg->channels));
    priv->avctx->channels = static_cast<int>(cfg->channels);
    priv->avctx->bit_rate = static_cast<int64_t>(cfg->bitrate > 0 ? cfg->bitrate : kDefaultAacBitrate);

    if (cfg->codec == HAL_AUDIO_CODEC_AAC)
    {
        priv->avctx->sample_fmt = priv->av_codec->sample_fmts ? priv->av_codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        priv->avctx->profile = FF_PROFILE_AAC_LOW;
    }
    else
    {
        priv->avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    }

    if (avcodec_open2(priv->avctx, priv->av_codec, nullptr) < 0)
    {
        HAL_LOG_ERROR("audio: avcodec_open2 failed");
        destroy_encoder(priv);
        return HAL_ERR_RESULT;
    }

    priv->encoder_sample_rate = priv->avctx->sample_rate;
    priv->encoder_sample_fmt = priv->avctx->sample_fmt;

    priv->frame = av_frame_alloc();
    priv->packet = av_packet_alloc();
    if (!priv->frame || !priv->packet)
    {
        destroy_encoder(priv);
        return HAL_ERR_NO_MEM;
    }

    priv->frame->format = priv->encoder_sample_fmt;
    priv->frame->sample_rate = priv->encoder_sample_rate;
    priv->frame->channel_layout = priv->avctx->channel_layout;
    priv->frame->channels = priv->avctx->channels;

    const AVSampleFormat in_fmt = av_sample_fmt_from_hal(cfg->sample_format);
    const int64_t in_layout = av_get_default_channel_layout(static_cast<int>(cfg->channels));

    priv->swr = swr_alloc_set_opts(
        nullptr,
        priv->avctx->channel_layout,
        priv->encoder_sample_fmt,
        priv->encoder_sample_rate,
        in_layout,
        in_fmt,
        static_cast<int>(cfg->sample_rate),
        0,
        nullptr);
    if (!priv->swr || swr_init(priv->swr) < 0)
    {
        HAL_LOG_ERROR("audio: swresample init failed");
        destroy_encoder(priv);
        return HAL_ERR_RESULT;
    }

    return HAL_OK;
}

static void apply_volume(HalAudioConfig *cfg, uint8_t *data, uint32_t size)
{
    if (!cfg || !data || size == 0 || cfg->mute)
    {
        if (data && size > 0 && cfg && cfg->mute)
        {
            std::memset(data, 0, size);
        }
        return;
    }
    if (std::fabs(cfg->volume - 1.0f) < 0.001f)
    {
        return;
    }
    const float gain = cfg->volume;
    if (cfg->sample_format == HAL_AUDIO_SAMPLE_FMT_S16LE)
    {
        auto *s = reinterpret_cast<int16_t *>(data);
        const size_t n = size / sizeof(int16_t);
        for (size_t i = 0; i < n; ++i)
        {
            const float v = static_cast<float>(s[i]) * gain;
            if (v > 32767.0f)
            {
                s[i] = 32767;
            }
            else if (v < -32768.0f)
            {
                s[i] = -32768;
            }
            else
            {
                s[i] = static_cast<int16_t>(v);
            }
        }
    }
    else if (cfg->sample_format == HAL_AUDIO_SAMPLE_FMT_F32LE)
    {
        auto *s = reinterpret_cast<float *>(data);
        const size_t n = size / sizeof(float);
        for (size_t i = 0; i < n; ++i)
        {
            s[i] *= gain;
        }
    }
}

static int open_alsa_stream(Hailo15AudioPriv *priv, snd_pcm_stream_t stream)
{
    snd_pcm_hw_params_t *hw = nullptr;
    int err = snd_pcm_open(&priv->pcm, priv->effective.device, stream, 0);
    if (err < 0)
    {
        HAL_LOG_ERROR("audio: snd_pcm_open(%s, %s) failed: %s",
                      priv->effective.device,
                      stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                      snd_strerror(err));
        return HAL_ERR_RESULT;
    }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(priv->pcm, hw);
    snd_pcm_hw_params_set_access(priv->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(priv->pcm, hw, alsa_format_from_hal(priv->effective.sample_format));
    snd_pcm_hw_params_set_channels(priv->pcm, hw, priv->effective.channels);
    unsigned int rate = priv->effective.sample_rate;
    snd_pcm_hw_params_set_rate_near(priv->pcm, hw, &rate, nullptr);
    priv->effective.sample_rate = rate;

    snd_pcm_uframes_t period = priv->effective.period_frames;
    snd_pcm_hw_params_set_period_size_near(priv->pcm, hw, &period, nullptr);
    unsigned int periods = 4u;
    snd_pcm_hw_params_set_periods_near(priv->pcm, hw, &periods, nullptr);

    err = snd_pcm_hw_params(priv->pcm, hw);
    if (err < 0)
    {
        HAL_LOG_ERROR("audio: snd_pcm_hw_params failed: %s", snd_strerror(err));
        snd_pcm_close(priv->pcm);
        priv->pcm = nullptr;
        return HAL_ERR_RESULT;
    }
    snd_pcm_prepare(priv->pcm);
    return HAL_OK;
}

static void close_alsa(Hailo15AudioPriv *priv)
{
    if (priv && priv->pcm)
    {
        snd_pcm_close(priv->pcm);
        priv->pcm = nullptr;
    }
}

static void deliver_pcm(HalAudioContext *ac, Hailo15AudioPriv *priv, const uint8_t *data, uint32_t size, uint32_t samples)
{
    HalAudioFrameCallback cb = nullptr;
    void *ud = nullptr;
    {
        std::lock_guard<std::mutex> lock(priv->mutex);
        cb = ac->pcm_callback;
        ud = ac->pcm_userdata;
    }
    if (!cb)
    {
        return;
    }

    auto *heap = new HalAudioHeapBuffer{};
    heap->bytes.assign(data, data + size);

    HalAudioBuffer frame{};
    frame.sample_rate = priv->effective.sample_rate;
    frame.channels = priv->effective.channels;
    frame.format = priv->effective.sample_format;
    frame.samples = samples;
    frame.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    frame.sequence = static_cast<uint32_t>(++priv->pcm_seq);
    frame.mem_type = HAL_MEM_MALLOC;
    frame.data = heap->bytes.data();
    frame.size = size;
    frame.priv = heap;
    cb(ac, &frame, ud);
}

static void deliver_encoded_packets(HalAudioContext *ac, Hailo15AudioPriv *priv)
{
    HalAudioPacketCallback cb = nullptr;
    void *ud = nullptr;
    {
        std::lock_guard<std::mutex> lock(priv->mutex);
        cb = ac->packet_callback;
        ud = ac->packet_userdata;
    }
    if (!cb || !priv->avctx)
    {
        return;
    }

    while (true)
    {
        const int rr = avcodec_receive_packet(priv->avctx, priv->packet);
        if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF)
        {
            break;
        }
        if (rr < 0)
        {
            HAL_LOG_WARNING("audio: avcodec_receive_packet error %d", rr);
            break;
        }

        auto *pkt_priv = new HalAudioPacketPriv{};
        pkt_priv->bytes.assign(priv->packet->data, priv->packet->data + priv->packet->size);

        HalPacketBuffer pkt{};
        pkt.type = packet_type_from_codec(priv->effective.codec);
        pkt.mem_type = HAL_MEM_MALLOC;
        pkt.dma_fd = -1;
        pkt.data = pkt_priv->bytes.data();
        pkt.size = static_cast<uint32_t>(pkt_priv->bytes.size());
        pkt.sequence = static_cast<uint32_t>(++priv->pkt_seq);
        pkt.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        pkt.priv = pkt_priv;
        cb(ac, &pkt, ud);
        av_packet_unref(priv->packet);
    }
}

static void encode_buffer(HalAudioContext *ac, Hailo15AudioPriv *priv, const uint8_t *data, uint32_t samples)
{
    if (!priv->avctx || !priv->swr || !priv->frame)
    {
        return;
    }

    priv->frame->nb_samples = static_cast<int>(samples);
    if (av_frame_get_buffer(priv->frame, 0) < 0)
    {
        return;
    }

    const uint8_t *in_data[1] = {data};
    const int converted = swr_convert(
        priv->swr,
        priv->frame->data,
        priv->frame->nb_samples,
        in_data,
        static_cast<int>(samples));
    if (converted < 0)
    {
        av_frame_unref(priv->frame);
        return;
    }
    priv->frame->nb_samples = converted;
    priv->frame->pts = static_cast<int64_t>(priv->pcm_seq);

    if (avcodec_send_frame(priv->avctx, priv->frame) < 0)
    {
        av_frame_unref(priv->frame);
        return;
    }
    av_frame_unref(priv->frame);
    deliver_encoded_packets(ac, priv);
}

static void capture_loop(HalAudioContext *ac)
{
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!priv || !priv->pcm)
    {
        return;
    }

    const uint32_t bps = hal_audio_sample_format_bytes(priv->effective.sample_format);
    const uint32_t frame_bytes = priv->effective.period_frames * priv->effective.channels * bps;
    std::vector<uint8_t> buffer(frame_bytes);

    while (priv->running.load())
    {
        const snd_pcm_sframes_t n = snd_pcm_readi(
            priv->pcm,
            buffer.data(),
            priv->effective.period_frames);
        if (n < 0)
        {
            if (n == -EPIPE)
            {
                snd_pcm_prepare(priv->pcm);
            }
            continue;
        }
        if (n == 0)
        {
            continue;
        }

        const uint32_t samples = static_cast<uint32_t>(n);
        const uint32_t size = samples * priv->effective.channels * bps;

        HalAudioConfig cfg_copy{};
        {
            std::lock_guard<std::mutex> lock(priv->mutex);
            cfg_copy = priv->effective;
        }
        apply_volume(&cfg_copy, buffer.data(), size);

        deliver_pcm(ac, priv, buffer.data(), size, samples);

        if (priv->effective.codec != HAL_AUDIO_CODEC_PCM && priv->avctx)
        {
            encode_buffer(ac, priv, buffer.data(), samples);
        }
    }
}

static int hailo15_audio_init(const HalAudioConfig *config, void **audio_ctx_return)
{
    if (!config || !audio_ctx_return || config->type != HAL_AUDIO_TYPE_ALSA)
    {
        return HAL_ERR_INVALID_ARG;
    }

    auto *ac = static_cast<HalAudioContext *>(std::calloc(1, sizeof(HalAudioContext)));
    if (!ac)
    {
        return HAL_ERR_NO_MEM;
    }
    auto *priv = new Hailo15AudioPriv{};
    ac->config = *config;
    apply_config_defaults(&ac->config);
    priv->effective = ac->config;
    ac->priv = priv;
    ac->status = HAL_STATUS_INITIALIZED;

    if (priv->effective.direction == HAL_AUDIO_IO_CAPTURE)
    {
        const int enc_rc = setup_encoder(priv, &priv->effective);
        if (enc_rc != HAL_OK)
        {
            delete priv;
            std::free(ac);
            return enc_rc;
        }
    }
    else
    {
        priv->effective.codec = HAL_AUDIO_CODEC_PCM;
    }

    *audio_ctx_return = ac;
    return HAL_OK;
}

static int hailo15_audio_deinit(void *audio_ctx)
{
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    if (!ac)
    {
        return HAL_ERR_INVALID_ARG;
    }
    (void)hailo15_audio_stop(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    destroy_encoder(priv);
    delete priv;
    ac->priv = nullptr;
    std::free(ac);
    return HAL_OK;
}

static int hailo15_audio_start(void *audio_ctx)
{
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }

    if (priv->effective.direction == HAL_AUDIO_IO_PLAYBACK)
    {
        if (priv->playback_open)
        {
            return HAL_OK;
        }
        const int open_rc = open_alsa_stream(priv, SND_PCM_STREAM_PLAYBACK);
        if (open_rc != HAL_OK)
        {
            return open_rc;
        }
        priv->playback_open = true;
        ac->status = HAL_STATUS_RUNNING;
        return HAL_OK;
    }

    if (priv->running.load())
    {
        return HAL_OK;
    }

    const int open_rc = open_alsa_stream(priv, SND_PCM_STREAM_CAPTURE);
    if (open_rc != HAL_OK)
    {
        return open_rc;
    }

    priv->running.store(true);
    priv->worker = std::thread(capture_loop, ac);
    ac->status = HAL_STATUS_RUNNING;
    return HAL_OK;
}

static int hailo15_audio_stop(void *audio_ctx)
{
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }

    if (priv->effective.direction == HAL_AUDIO_IO_PLAYBACK)
    {
        // Serialize with hailo15_audio_write_pcm(): it holds priv->mutex across
        // snd_pcm_writei, so by the time we acquire it here the in-flight write
        // has completed and priv->pcm is stable. close_alsa() is lock-free —
        // callers (this path) hold the mutex.
        std::lock_guard<std::mutex> lock(priv->mutex);
        if (!priv->playback_open)
        {
            return HAL_OK;
        }
        if (priv->pcm)
        {
            // drop, not drain: push-to-talk stop wants immediate release, and
            // snd_pcm_drain() leaves the speexrate resampler in a state that
            // intermittently double-frees inside snd_pcm_hw_free on close
            // (core.72183: speex_resampler_destroy -> double free or corruption).
            (void)snd_pcm_drop(priv->pcm);
        }
        close_alsa(priv);
        priv->playback_open = false;
        ac->status = HAL_STATUS_INITIALIZED;
        return HAL_OK;
    }

    if (!priv->running.load())
    {
        return HAL_OK;
    }
    priv->running.store(false);
    if (priv->worker.joinable())
    {
        priv->worker.join();
    }
    close_alsa(priv);
    ac->status = HAL_STATUS_INITIALIZED;
    return HAL_OK;
}

static int hailo15_audio_get_status(void *audio_ctx)
{
    const HalAudioContext *ac = ctx_ptr(audio_ctx);
    if (!ac)
    {
        return HAL_ERR_INVALID_ARG;
    }
    return static_cast<int>(ac->status);
}

static int hailo15_audio_get_current_config(void *audio_ctx, HalAudioConfig *config)
{
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (priv)
    {
        std::lock_guard<std::mutex> lock(priv->mutex);
        *config = priv->effective;
    }
    else
    {
        *config = ac->config;
    }
    return HAL_OK;
}

static int hailo15_audio_subscribe_pcm(void *audio_ctx, HalAudioFrameCallback callback, void *userdata)
{
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !priv || !callback)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (priv->effective.direction == HAL_AUDIO_IO_PLAYBACK)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    std::lock_guard<std::mutex> lock(priv->mutex);
    ac->pcm_callback = callback;
    ac->pcm_userdata = userdata;
    return HAL_OK;
}

static int hailo15_audio_unsubscribe_pcm(void *audio_ctx, HalAudioFrameCallback callback)
{
    (void)callback;
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(priv->mutex);
    ac->pcm_callback = nullptr;
    ac->pcm_userdata = nullptr;
    return HAL_OK;
}

static int hailo15_audio_subscribe_packet(void *audio_ctx, HalAudioPacketCallback callback, void *userdata)
{
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !priv || !callback)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (priv->effective.direction == HAL_AUDIO_IO_PLAYBACK)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (priv->effective.codec == HAL_AUDIO_CODEC_PCM)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    std::lock_guard<std::mutex> lock(priv->mutex);
    ac->packet_callback = callback;
    ac->packet_userdata = userdata;
    return HAL_OK;
}

static int hailo15_audio_unsubscribe_packet(void *audio_ctx, HalAudioPacketCallback callback)
{
    (void)callback;
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(priv->mutex);
    ac->packet_callback = nullptr;
    ac->packet_userdata = nullptr;
    return HAL_OK;
}

static int hailo15_audio_release_frame(void *audio_ctx, HalAudioBuffer *frame)
{
    (void)audio_ctx;
    if (frame && frame->priv)
    {
        delete static_cast<HalAudioHeapBuffer *>(frame->priv);
        frame->priv = nullptr;
        frame->data = nullptr;
    }
    return HAL_OK;
}

static int hailo15_audio_release_packet(void *audio_ctx, HalPacketBuffer *packet)
{
    (void)audio_ctx;
    if (packet && packet->priv)
    {
        delete static_cast<HalAudioPacketPriv *>(packet->priv);
        packet->priv = nullptr;
        packet->data = nullptr;
    }
    return HAL_OK;
}

static int list_alsa_devices(HalAudioDeviceInfo *devices,
                             uint32_t max_devices,
                             uint32_t *count_out,
                             bool playback_list)
{
    if (!devices || !count_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *count_out = 0;

    void **hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
    {
        return HAL_ERR_RESULT;
    }

    for (void **n = hints; *n && *count_out < max_devices; ++n)
    {
        char *io = snd_device_name_get_hint(*n, "IOID");
        if (io)
        {
            const bool is_output = (std::strcmp(io, "Output") == 0);
            free(io);
            if (playback_list ? !is_output : is_output)
            {
                continue;
            }
        }

        char *name = snd_device_name_get_hint(*n, "NAME");
        if (!name)
        {
            continue;
        }

        HalAudioDeviceInfo &ent = devices[*count_out];
        std::memset(&ent, 0, sizeof(ent));
        std::strncpy(ent.name, name, sizeof(ent.name) - 1);
        char *desc = snd_device_name_get_hint(*n, "DESC");
        if (desc)
        {
            std::strncpy(ent.description, desc, sizeof(ent.description) - 1);
            free(desc);
        }
        free(name);
        ++(*count_out);
    }
    snd_device_name_free_hint(hints);

    if (*count_out == 0 && max_devices > 0)
    {
        std::strncpy(devices[0].name, "default", sizeof(devices[0].name) - 1);
        std::strncpy(
            devices[0].description,
            playback_list ? "ALSA default PCM playback" : "ALSA default PCM capture",
            sizeof(devices[0].description) - 1);
        *count_out = 1;
    }
    return HAL_OK;
}

static int hailo15_audio_list_devices(HalAudioDeviceInfo *devices, uint32_t max_devices, uint32_t *count_out)
{
    return list_alsa_devices(devices, max_devices, count_out, false);
}

static int hailo15_audio_list_playback_devices(HalAudioDeviceInfo *devices,
                                               uint32_t max_devices,
                                               uint32_t *count_out)
{
    return list_alsa_devices(devices, max_devices, count_out, true);
}

static int hailo15_audio_write_pcm(void *audio_ctx, const HalAudioBuffer *frame)
{
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !priv || !frame || !frame->data || frame->size == 0 || frame->samples == 0)
    {
        return HAL_ERR_INVALID_ARG;
    }

    // Hold priv->mutex across the pcm lifecycle check and snd_pcm_writei so that
    // a concurrent hailo15_audio_stop()/close_alsa() cannot free priv->pcm
    // (and snd_pcm_t) underneath us. Playback has no worker thread, so holding
    // the mutex here cannot deadlock with capture_loop/deliver_pcm.
    std::lock_guard<std::mutex> lock(priv->mutex);
    if (priv->effective.direction != HAL_AUDIO_IO_PLAYBACK || !priv->playback_open || !priv->pcm)
    {
        return HAL_ERR_INVALID_STATE;
    }

    const uint32_t bps = hal_audio_sample_format_bytes(priv->effective.sample_format);
    if (bps == 0 ||
        frame->format != priv->effective.sample_format ||
        frame->sample_rate != priv->effective.sample_rate ||
        frame->channels != priv->effective.channels)
    {
        return HAL_ERR_INVALID_FMT;
    }
    const uint32_t expected = frame->samples * frame->channels * bps;
    if (frame->size < expected)
    {
        return HAL_ERR_INVALID_SIZE;
    }

    std::vector<uint8_t> tmp(frame->data, frame->data + expected);
    HalAudioConfig cfg_copy = priv->effective;
    apply_volume(&cfg_copy, tmp.data(), expected);

    snd_pcm_sframes_t written = snd_pcm_writei(priv->pcm, tmp.data(), frame->samples);
    if (written < 0)
    {
        if (written == -EPIPE)
        {
            (void)snd_pcm_prepare(priv->pcm);
            written = snd_pcm_writei(priv->pcm, tmp.data(), frame->samples);
        }
        if (written < 0)
        {
            HAL_LOG_WARNING("audio: snd_pcm_writei failed: %s", snd_strerror(static_cast<int>(written)));
            return HAL_ERR_RESULT;
        }
    }
    return HAL_OK;
}

static int hailo15_audio_dynamic_change_config(void *audio_ctx, const HalAudioConfig *config)
{
    HalAudioContext *ac = ctx_ptr(audio_ctx);
    Hailo15AudioPriv *priv = audio_priv(ac);
    if (!ac || !priv || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }

    const bool was_running =
        (priv->effective.direction == HAL_AUDIO_IO_PLAYBACK) ? priv->playback_open : priv->running.load();
    if (was_running)
    {
        (void)hailo15_audio_stop(audio_ctx);
    }

    std::lock_guard<std::mutex> lock(priv->mutex);
    priv->effective.volume = config->volume > 0.0f ? config->volume : priv->effective.volume;
    priv->effective.mute = config->mute;
    if (config->bitrate > 0)
    {
        priv->effective.bitrate = config->bitrate;
    }

    if (priv->effective.direction == HAL_AUDIO_IO_PLAYBACK)
    {
        ac->config = priv->effective;
        if (was_running)
        {
            return hailo15_audio_start(audio_ctx);
        }
        return HAL_OK;
    }

    const bool codec_changed = config->codec != priv->effective.codec;
    const bool rate_changed =
        (config->sample_rate > 0 && config->sample_rate != priv->effective.sample_rate);
    const bool ch_changed =
        (config->channels > 0 && config->channels != priv->effective.channels);

    if (codec_changed || rate_changed || ch_changed)
    {
        if (config->codec != HAL_AUDIO_CODEC_PCM)
        {
            priv->effective.codec = config->codec;
        }
        if (config->sample_rate > 0)
        {
            priv->effective.sample_rate = config->sample_rate;
        }
        if (config->channels > 0)
        {
            priv->effective.channels = config->channels;
        }
        destroy_encoder(priv);
        const int enc_rc = setup_encoder(priv, &priv->effective);
        if (enc_rc != HAL_OK)
        {
            return enc_rc;
        }
    }
    else if (priv->avctx && config->bitrate > 0)
    {
        priv->effective.bitrate = config->bitrate;
        priv->avctx->bit_rate = static_cast<int64_t>(config->bitrate);
    }

    ac->config = priv->effective;

    if (was_running)
    {
        return hailo15_audio_start(audio_ctx);
    }
    return HAL_OK;
}

static const char *hailo15_audio_get_version(void)
{
    return "Hailo15 HAL-AUDIO 2.0.0";
}

} // namespace

extern "C" {

HalAudioOps HAL_AUDIO_OPS = {
    .init = hailo15_audio_init,
    .deinit = hailo15_audio_deinit,
    .start = hailo15_audio_start,
    .stop = hailo15_audio_stop,
    .get_status = hailo15_audio_get_status,
    .get_current_config = hailo15_audio_get_current_config,
    .subscribe_pcm = hailo15_audio_subscribe_pcm,
    .unsubscribe_pcm = hailo15_audio_unsubscribe_pcm,
    .subscribe_packet = hailo15_audio_subscribe_packet,
    .unsubscribe_packet = hailo15_audio_unsubscribe_packet,
    .release_frame = hailo15_audio_release_frame,
    .release_packet = hailo15_audio_release_packet,
    .list_devices = hailo15_audio_list_devices,
    .list_playback_devices = hailo15_audio_list_playback_devices,
    .write_pcm = hailo15_audio_write_pcm,
    .dynamic_change_config = hailo15_audio_dynamic_change_config,
    .get_version = hailo15_audio_get_version,
};

} // extern "C"
