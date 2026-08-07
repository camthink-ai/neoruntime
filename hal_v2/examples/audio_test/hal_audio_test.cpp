/**
 * @file hal_audio_test.cpp
 * @brief Example: HAL audio capture, encode, and playback.
 *
 * Capture / encode:
 *   hal-audio-test [--list]
 *   hal-audio-test --device default --duration 5 --codec aac
 *
 * Save to file:
 *   hal-audio-test --duration 10 --record-to /tmp/rec.raw
 *   hal-audio-test --duration 10 --codec aac --record-encoded-to /tmp/rec.aac
 *
 * Playback:
 *   hal-audio-test --play-file rec.raw --rate 48000 --channels 1
 *   hal-audio-test --loopback 3
 */

#include "common/hal_common.h"
#include "common/hal_log.h"
#include "media/hal_audio.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <signal.h>

namespace
{

struct Stats
{
    std::atomic<uint64_t> pcm_frames{0};
    std::atomic<uint64_t> pcm_bytes{0};
    std::atomic<uint64_t> pkt_count{0};
    std::atomic<uint64_t> pkt_bytes{0};
};

struct RecordBuffer
{
    std::mutex mutex;
    std::vector<uint8_t> pcm;
    uint32_t sample_rate{48000};
    uint32_t channels{1};
};

/** Streams PCM capture to a raw file (S16LE interleaved). */
struct PcmFileWriter
{
    std::mutex mutex;
    std::ofstream out;
    uint64_t bytes_written{0};
    bool open{false};
};

/** Appends encoded packets (e.g. AAC ADTS) to a binary file. */
struct EncodedFileWriter
{
    std::mutex mutex;
    std::ofstream out;
    uint64_t bytes_written{0};
    uint64_t packet_count{0};
    bool open{false};
};

struct PcmCaptureSink
{
    PcmFileWriter *file{nullptr};
    Stats *stats{nullptr};
};

std::atomic<bool> g_stop{false};
volatile sig_atomic_t g_signal_pending = 0;

void on_signal(int)
{
    g_signal_pending = 1;
}

void install_signal_handlers()
{
    struct sigaction sa
    {
    };
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);
}

static HalAudioCodecType parse_codec(const char *name)
{
    if (!name || !name[0])
    {
        return HAL_AUDIO_CODEC_AAC;
    }
    if (std::strcmp(name, "pcm") == 0)
    {
        return HAL_AUDIO_CODEC_PCM;
    }
    if (std::strcmp(name, "aac") == 0)
    {
        return HAL_AUDIO_CODEC_AAC;
    }
    if (std::strcmp(name, "g711a") == 0 || std::strcmp(name, "alaw") == 0)
    {
        return HAL_AUDIO_CODEC_G711A;
    }
    if (std::strcmp(name, "g711u") == 0 || std::strcmp(name, "ulaw") == 0 || std::strcmp(name, "mulaw") == 0)
    {
        return HAL_AUDIO_CODEC_G711U;
    }
    return HAL_AUDIO_CODEC_AAC;
}

static const char *codec_name(HalAudioCodecType c)
{
    switch (c)
    {
        case HAL_AUDIO_CODEC_PCM:
            return "pcm";
        case HAL_AUDIO_CODEC_AAC:
            return "aac";
        case HAL_AUDIO_CODEC_G711A:
            return "g711a";
        case HAL_AUDIO_CODEC_G711U:
            return "g711u";
        default:
            return "?";
    }
}

static const char *packet_type_str(HalPacketType pt)
{
    switch (pt)
    {
        case HAL_PACKET_TYPE_AAC:
            return "AAC";
        case HAL_PACKET_TYPE_G711A:
            return "G711A";
        case HAL_PACKET_TYPE_G711U:
            return "G711U";
        case HAL_PACKET_TYPE_PCM:
            return "PCM";
        default:
            return "?";
    }
}

static void apply_volume_mute(HalAudioConfig *cfg, float volume, bool mute)
{
    cfg->volume = volume;
    cfg->mute = mute;
}

static void fill_base_config(HalAudioConfig *cfg,
                             const std::string &device,
                             uint32_t sample_rate,
                             uint32_t channels,
                             float volume,
                             bool mute)
{
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->type = HAL_AUDIO_TYPE_ALSA;
    cfg->direction = HAL_AUDIO_IO_CAPTURE;
    std::strncpy(cfg->device, device.c_str(), sizeof(cfg->device) - 1);
    cfg->sample_rate = sample_rate;
    cfg->channels = channels;
    cfg->sample_format = HAL_AUDIO_SAMPLE_FMT_S16LE;
    apply_volume_mute(cfg, volume, mute);
}

static void print_usage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s --list | --list-playback\n"
                 "  %s [capture options]\n"
                 "  %s --loopback <sec> [options]\n"
                 "  %s --play-file <file.raw> [options]\n\n"
                 "Capture options:\n"
                 "  --device <name>       Capture device (default: default)\n"
                 "  --play-device <name>  Playback device (default: default)\n"
                 "  --duration <sec>      Capture time (default: 10)\n"
                 "  --codec <name>        pcm | aac | g711a | g711u (default: aac)\n"
                 "  --rate <Hz>           Sample rate (default: 48000)\n"
                 "  --channels <n>        1 or 2 (default: 1)\n"
                 "  --bitrate <bps>       AAC bitrate (default: 128000)\n"
                 "  --pcm                 Also tap raw PCM when encoding\n"
                 "  --volume <gain>       Software gain 0.0..2.0, 1.0=normal (default: 1.0)\n"
                 "  --mute                Silence audio (gain ignored)\n"
                 "  --capture-volume <g>  Capture gain (overrides --volume for record)\n"
                 "  --play-volume <g>     Playback gain (overrides --volume for play)\n"
                 "  --record-to <path>    Save PCM to file (S16LE; use same --rate/--channels to play)\n"
                 "  --output <path>       Alias of --record-to\n"
                 "  --record-encoded-to <path>  Save encoded stream (e.g. AAC) to file\n\n"
                 "Playback:\n"
                 "  --loopback <sec>      Record PCM then play back on speaker\n"
                 "  --play-file <path>    Play raw S16LE interleaved PCM file\n",
                 prog, prog, prog, prog);
}

static int list_capture_devices(void)
{
    if (!HAL_AUDIO_OPS.list_devices)
    {
        std::fprintf(stderr, "list_devices not implemented\n");
        return 1;
    }
    HalAudioDeviceInfo devs[HAL_AUDIO_MAX_LIST_DEVICES]{};
    uint32_t count = 0;
    const int rc = HAL_AUDIO_OPS.list_devices(devs, HAL_AUDIO_MAX_LIST_DEVICES, &count);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "list_devices failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
        return 1;
    }
    std::printf("HAL audio version: %s\n", HAL_AUDIO_OPS.get_version ? HAL_AUDIO_OPS.get_version() : "?");
    std::printf("Capture devices (%u):\n", count);
    for (uint32_t i = 0; i < count; ++i)
    {
        std::printf("  [%u] %s\n", i, devs[i].name);
        if (devs[i].description[0])
        {
            std::printf("       %s\n", devs[i].description);
        }
    }
    return 0;
}

static int list_playback_devices(void)
{
    if (!HAL_AUDIO_OPS.list_playback_devices)
    {
        std::fprintf(stderr, "list_playback_devices not implemented\n");
        return 1;
    }
    HalAudioDeviceInfo devs[HAL_AUDIO_MAX_LIST_DEVICES]{};
    uint32_t count = 0;
    const int rc = HAL_AUDIO_OPS.list_playback_devices(devs, HAL_AUDIO_MAX_LIST_DEVICES, &count);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "list_playback_devices failed: %s\n",
                     hal_error_to_string(static_cast<HalErrorCode>(rc)));
        return 1;
    }
    std::printf("HAL audio version: %s\n", HAL_AUDIO_OPS.get_version ? HAL_AUDIO_OPS.get_version() : "?");
    std::printf("Playback devices (%u):\n", count);
    for (uint32_t i = 0; i < count; ++i)
    {
        std::printf("  [%u] %s\n", i, devs[i].name);
        if (devs[i].description[0])
        {
            std::printf("       %s\n", devs[i].description);
        }
    }
    return 0;
}

static void pcm_cb(void *audio_ctx, HalAudioBuffer *frame, void *userdata)
{
    auto *st = static_cast<Stats *>(userdata);
    if (!audio_ctx || !frame || !st)
    {
        return;
    }
    st->pcm_frames.fetch_add(1, std::memory_order_relaxed);
    st->pcm_bytes.fetch_add(frame->size, std::memory_order_relaxed);
    (void)HAL_AUDIO_OPS.release_frame(audio_ctx, frame);
}

static bool open_pcm_file(PcmFileWriter *writer, const std::string &path)
{
    if (!writer)
    {
        return false;
    }
    writer->out.open(path, std::ios::binary | std::ios::trunc);
    writer->open = writer->out.good();
    if (!writer->open)
    {
        std::fprintf(stderr, "cannot open PCM output file: %s\n", path.c_str());
    }
    return writer->open;
}

static bool open_encoded_file(EncodedFileWriter *writer, const std::string &path)
{
    if (!writer)
    {
        return false;
    }
    writer->out.open(path, std::ios::binary | std::ios::trunc);
    writer->open = writer->out.good();
    if (!writer->open)
    {
        std::fprintf(stderr, "cannot open encoded output file: %s\n", path.c_str());
    }
    return writer->open;
}

static void record_pcm_cb(void *audio_ctx, HalAudioBuffer *frame, void *userdata)
{
    auto *rec = static_cast<RecordBuffer *>(userdata);
    if (!audio_ctx || !frame || !rec || !frame->data)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(rec->mutex);
        rec->pcm.insert(rec->pcm.end(), frame->data, frame->data + frame->size);
        rec->sample_rate = frame->sample_rate;
        rec->channels = frame->channels;
    }
    (void)HAL_AUDIO_OPS.release_frame(audio_ctx, frame);
}

static void pcm_capture_sink_cb(void *audio_ctx, HalAudioBuffer *frame, void *userdata)
{
    auto *sink = static_cast<PcmCaptureSink *>(userdata);
    if (!audio_ctx || !frame || !sink || !frame->data)
    {
        return;
    }
    if (sink->file && sink->file->open)
    {
        std::lock_guard<std::mutex> lock(sink->file->mutex);
        sink->file->out.write(reinterpret_cast<const char *>(frame->data),
                              static_cast<std::streamsize>(frame->size));
        sink->file->bytes_written += frame->size;
    }
    if (sink->stats)
    {
        sink->stats->pcm_frames.fetch_add(1, std::memory_order_relaxed);
        sink->stats->pcm_bytes.fetch_add(frame->size, std::memory_order_relaxed);
    }
    (void)HAL_AUDIO_OPS.release_frame(audio_ctx, frame);
}

static void packet_to_file_cb(void *audio_ctx, HalPacketBuffer *pkt, void *userdata)
{
    auto *writer = static_cast<EncodedFileWriter *>(userdata);
    if (!audio_ctx || !pkt || !writer || !pkt->data || pkt->size == 0)
    {
        return;
    }
    if (writer->open)
    {
        std::lock_guard<std::mutex> lock(writer->mutex);
        writer->out.write(reinterpret_cast<const char *>(pkt->data),
                        static_cast<std::streamsize>(pkt->size));
        writer->bytes_written += pkt->size;
        ++writer->packet_count;
    }
    (void)HAL_AUDIO_OPS.release_packet(audio_ctx, pkt);
}

static void packet_cb(void *audio_ctx, HalPacketBuffer *pkt, void *userdata)
{
    auto *st = static_cast<Stats *>(userdata);
    if (!audio_ctx || !pkt || !st)
    {
        return;
    }
    const uint64_t n = st->pkt_count.fetch_add(1, std::memory_order_relaxed);
    st->pkt_bytes.fetch_add(pkt->size, std::memory_order_relaxed);
    if (n < 5)
    {
        std::printf("  pkt#%llu type=%s size=%u seq=%u\n",
                    static_cast<unsigned long long>(n),
                    packet_type_str(pkt->type),
                    pkt->size,
                    pkt->sequence);
    }
    (void)HAL_AUDIO_OPS.release_packet(audio_ctx, pkt);
}

static int wait_seconds(int duration_sec)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load())
    {
        if (g_signal_pending)
        {
            g_signal_pending = 0;
            break;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0);
        if (elapsed.count() >= duration_sec)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}

static int playback_pcm_buffer(const std::string &play_device,
                              uint32_t sample_rate,
                              uint32_t channels,
                              float play_volume,
                              bool mute,
                              const std::vector<uint8_t> &pcm)
{
    if (pcm.empty())
    {
        std::fprintf(stderr, "playback: no PCM data\n");
        return 1;
    }
    if (!HAL_AUDIO_OPS.write_pcm)
    {
        std::fprintf(stderr, "write_pcm not implemented\n");
        return 1;
    }

    const uint32_t bps = hal_audio_sample_format_bytes(HAL_AUDIO_SAMPLE_FMT_S16LE);
    const uint32_t frame_bytes = channels * bps;
    if (pcm.size() % frame_bytes != 0)
    {
        std::fprintf(stderr, "playback: PCM size %zu not aligned to %u bytes/frame\n", pcm.size(), frame_bytes);
        return 1;
    }
    const uint32_t total_samples = static_cast<uint32_t>(pcm.size() / frame_bytes);

    HalAudioConfig pcfg{};
    pcfg.type = HAL_AUDIO_TYPE_ALSA;
    pcfg.direction = HAL_AUDIO_IO_PLAYBACK;
    std::strncpy(pcfg.device, play_device.c_str(), sizeof(pcfg.device) - 1);
    pcfg.sample_rate = sample_rate;
    pcfg.channels = channels;
    pcfg.sample_format = HAL_AUDIO_SAMPLE_FMT_S16LE;
    pcfg.codec = HAL_AUDIO_CODEC_PCM;
    apply_volume_mute(&pcfg, play_volume, mute);

    void *play_ctx = nullptr;
    int rc = HAL_AUDIO_OPS.init(&pcfg, &play_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "playback init failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
        return 1;
    }
    rc = HAL_AUDIO_OPS.start(play_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "playback start failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
        (void)HAL_AUDIO_OPS.deinit(play_ctx);
        return 1;
    }

    std::printf("Playing %u samples on '%s' (volume=%.2f%s)...\n",
                total_samples,
                pcfg.device,
                play_volume,
                mute ? ", muted" : "");

    constexpr uint32_t kChunkSamples = 1024;
    uint32_t offset = 0;
    while (offset < total_samples)
    {
        if (g_signal_pending)
        {
            g_signal_pending = 0;
            break;
        }
        const uint32_t chunk = std::min(kChunkSamples, total_samples - offset);
        const uint32_t byte_off = offset * frame_bytes;
        HalAudioBuffer frame{};
        frame.sample_rate = sample_rate;
        frame.channels = channels;
        frame.format = HAL_AUDIO_SAMPLE_FMT_S16LE;
        frame.samples = chunk;
        frame.data = const_cast<uint8_t *>(pcm.data() + byte_off);
        frame.size = chunk * frame_bytes;
        rc = HAL_AUDIO_OPS.write_pcm(play_ctx, &frame);
        if (rc != HAL_OK)
        {
            std::fprintf(stderr, "write_pcm failed at offset %u: %s\n", offset,
                         hal_error_to_string(static_cast<HalErrorCode>(rc)));
            break;
        }
        offset += chunk;
    }

    (void)HAL_AUDIO_OPS.stop(play_ctx);
    (void)HAL_AUDIO_OPS.deinit(play_ctx);
    std::printf("Playback finished (%u / %u samples written)\n", offset, total_samples);
    return (offset == 0) ? 1 : 0;
}

static int run_loopback(const std::string &capture_device,
                        const std::string &play_device,
                        int duration_sec,
                        uint32_t sample_rate,
                        uint32_t channels,
                        float capture_volume,
                        float play_volume,
                        bool mute,
                        const std::string &record_to_path)
{
    if (duration_sec <= 0)
    {
        duration_sec = 3;
    }

    HalAudioConfig cfg{};
    fill_base_config(&cfg, capture_device, sample_rate, channels, capture_volume, mute);
    cfg.codec = HAL_AUDIO_CODEC_PCM;

    RecordBuffer rec{};
    PcmFileWriter pcm_file{};
    PcmCaptureSink sink{};
    sink.stats = nullptr;
    if (!record_to_path.empty())
    {
        if (!open_pcm_file(&pcm_file, record_to_path))
        {
            return 1;
        }
        sink.file = &pcm_file;
        std::printf("Saving PCM to %s\n", record_to_path.c_str());
    }

    void *cap_ctx = nullptr;
    int rc = HAL_AUDIO_OPS.init(&cfg, &cap_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "capture init failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
        return 1;
    }
    rc = HAL_AUDIO_OPS.subscribe_pcm(cap_ctx, sink.file ? pcm_capture_sink_cb : record_pcm_cb,
                                     sink.file ? static_cast<void *>(&sink) : static_cast<void *>(&rec));
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "subscribe_pcm failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
        (void)HAL_AUDIO_OPS.deinit(cap_ctx);
        return 1;
    }
    rc = HAL_AUDIO_OPS.start(cap_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "capture start failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
        (void)HAL_AUDIO_OPS.deinit(cap_ctx);
        return 1;
    }

    std::printf("Loopback: recording %d s from '%s'...\n", duration_sec, cfg.device);
    (void)wait_seconds(duration_sec);

    (void)HAL_AUDIO_OPS.stop(cap_ctx);
    (void)HAL_AUDIO_OPS.unsubscribe_pcm(
        cap_ctx, sink.file ? pcm_capture_sink_cb : record_pcm_cb);
    (void)HAL_AUDIO_OPS.deinit(cap_ctx);

    if (pcm_file.open)
    {
        pcm_file.out.close();
        std::printf("Saved %llu bytes PCM to %s (%u Hz, %u ch, format S16LE)\n",
                    static_cast<unsigned long long>(pcm_file.bytes_written),
                    record_to_path.c_str(),
                    sample_rate,
                    channels);
    }

    std::vector<uint8_t> pcm_copy;
    uint32_t rec_rate = sample_rate;
    uint32_t rec_ch = channels;
    if (!sink.file)
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        pcm_copy = rec.pcm;
        rec_rate = rec.sample_rate;
        rec_ch = rec.channels;
    }
    else if (pcm_file.bytes_written > 0)
    {
        std::ifstream in(record_to_path, std::ios::binary);
        pcm_copy.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        rec_rate = sample_rate;
        rec_ch = channels;
    }
    std::printf("Recorded %zu bytes in memory (%u Hz, %u ch)\n", pcm_copy.size(), rec_rate, rec_ch);
    if (pcm_copy.empty())
    {
        return 2;
    }

    return playback_pcm_buffer(play_device, rec_rate, rec_ch, play_volume, mute, pcm_copy);
}

static int run_play_file(const std::string &path,
                         const std::string &play_device,
                         uint32_t sample_rate,
                         uint32_t channels,
                         float play_volume,
                         bool mute)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 1;
    }
    std::vector<uint8_t> pcm((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (pcm.empty())
    {
        std::fprintf(stderr, "file is empty: %s\n", path.c_str());
        return 1;
    }
    std::printf("Loaded %zu bytes from %s\n", pcm.size(), path.c_str());
    return playback_pcm_buffer(play_device, sample_rate, channels, play_volume, mute, pcm);
}

static int run_capture_test(const std::string &device,
                            int duration_sec,
                            HalAudioCodecType codec,
                            uint32_t sample_rate,
                            uint32_t channels,
                            uint32_t bitrate,
                            bool also_pcm,
                            float capture_volume,
                            bool mute,
                            const std::string &record_to_path,
                            const std::string &record_encoded_path)
{
    HalAudioConfig cfg{};
    fill_base_config(&cfg, device, sample_rate, channels, capture_volume, mute);
    cfg.codec = codec;
    cfg.bitrate = bitrate;

    std::printf("HAL audio version: %s\n", HAL_AUDIO_OPS.get_version ? HAL_AUDIO_OPS.get_version() : "?");
    std::printf("device=%s codec=%s rate=%u ch=%u duration=%ds volume=%.2f%s\n",
                cfg.device,
                codec_name(codec),
                cfg.sample_rate,
                cfg.channels,
                duration_sec,
                capture_volume,
                mute ? " mute" : "");

    void *audio_ctx = nullptr;
    int rc = HAL_AUDIO_OPS.init(&cfg, &audio_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "init failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
        return 1;
    }

    Stats stats{};
    PcmFileWriter pcm_file{};
    EncodedFileWriter enc_file{};
    PcmCaptureSink pcm_sink{};
    pcm_sink.stats = &stats;

    const bool save_pcm = !record_to_path.empty();
    const bool save_enc = !record_encoded_path.empty();
    const bool want_pcm = save_pcm || (codec == HAL_AUDIO_CODEC_PCM) || also_pcm;
    const bool want_pkt = save_enc || (codec != HAL_AUDIO_CODEC_PCM);

    if (save_pcm && !open_pcm_file(&pcm_file, record_to_path))
    {
        (void)HAL_AUDIO_OPS.deinit(audio_ctx);
        return 1;
    }
    if (save_enc && !open_encoded_file(&enc_file, record_encoded_path))
    {
        (void)HAL_AUDIO_OPS.deinit(audio_ctx);
        return 1;
    }
    if (save_pcm)
    {
        pcm_sink.file = &pcm_file;
        std::printf("Saving PCM to %s (S16LE, %u Hz, %u ch)\n",
                    record_to_path.c_str(),
                    sample_rate,
                    channels);
    }
    if (save_enc)
    {
        std::printf("Saving encoded stream to %s (codec=%s)\n",
                    record_encoded_path.c_str(),
                    codec_name(codec));
    }

    if (want_pcm)
    {
        rc = HAL_AUDIO_OPS.subscribe_pcm(audio_ctx, save_pcm ? pcm_capture_sink_cb : pcm_cb,
                                         save_pcm ? static_cast<void *>(&pcm_sink)
                                                  : static_cast<void *>(&stats));
        if (rc != HAL_OK)
        {
            std::fprintf(stderr, "subscribe_pcm failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
            (void)HAL_AUDIO_OPS.deinit(audio_ctx);
            return 1;
        }
    }
    if (want_pkt)
    {
        rc = HAL_AUDIO_OPS.subscribe_packet(audio_ctx, save_enc ? packet_to_file_cb : packet_cb,
                                            save_enc ? static_cast<void *>(&enc_file)
                                                     : static_cast<void *>(&stats));
        if (rc != HAL_OK)
        {
            std::fprintf(stderr, "subscribe_packet failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
            (void)HAL_AUDIO_OPS.deinit(audio_ctx);
            return 1;
        }
    }

    rc = HAL_AUDIO_OPS.start(audio_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "start failed: %s\n", hal_error_to_string(static_cast<HalErrorCode>(rc)));
        (void)HAL_AUDIO_OPS.deinit(audio_ctx);
        return 1;
    }

    std::printf("Capturing... (Ctrl+C to stop early)\n");
    (void)wait_seconds(duration_sec);

    (void)HAL_AUDIO_OPS.stop(audio_ctx);
    if (want_pcm)
    {
        (void)HAL_AUDIO_OPS.unsubscribe_pcm(audio_ctx, save_pcm ? pcm_capture_sink_cb : pcm_cb);
    }
    if (want_pkt)
    {
        (void)HAL_AUDIO_OPS.unsubscribe_packet(audio_ctx, save_enc ? packet_to_file_cb : packet_cb);
    }
    (void)HAL_AUDIO_OPS.deinit(audio_ctx);

    if (pcm_file.open)
    {
        pcm_file.out.close();
        std::printf("  PCM file: %s (%llu bytes)\n",
                    record_to_path.c_str(),
                    static_cast<unsigned long long>(pcm_file.bytes_written));
        std::printf("  Playback: %s --play-file %s --rate %u --channels %u\n",
                    "hal-audio-test",
                    record_to_path.c_str(),
                    sample_rate,
                    channels);
    }
    if (enc_file.open)
    {
        enc_file.out.close();
        std::printf("  Encoded file: %s (%llu bytes, %llu packets)\n",
                    record_encoded_path.c_str(),
                    static_cast<unsigned long long>(enc_file.bytes_written),
                    static_cast<unsigned long long>(enc_file.packet_count));
    }

    std::printf("\nCapture done:\n");
    if (want_pcm)
    {
        std::printf("  PCM frames: %llu  bytes: %llu\n",
                    static_cast<unsigned long long>(stats.pcm_frames.load()),
                    static_cast<unsigned long long>(stats.pcm_bytes.load()));
    }
    if (want_pkt)
    {
        std::printf("  Encoded packets: %llu  bytes: %llu\n",
                    static_cast<unsigned long long>(stats.pkt_count.load()),
                    static_cast<unsigned long long>(stats.pkt_bytes.load()));
        if (stats.pkt_count.load() == 0)
        {
            std::printf("  (no packets — check microphone and capture device)\n");
        }
    }

    if (save_pcm && pcm_file.bytes_written == 0)
    {
        return 2;
    }
    if (save_enc && enc_file.packet_count == 0)
    {
        return 2;
    }
    return (want_pkt && !save_enc && stats.pkt_count.load() == 0) ? 2 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    install_signal_handlers();

    bool do_list = false;
    bool do_list_playback = false;
    int loopback_sec = -1;
    std::string play_file;
    std::string record_to_path;
    std::string record_encoded_path;
    std::string device = "default";
    std::string play_device = "default";
    int duration_sec = 10;
    HalAudioCodecType codec = HAL_AUDIO_CODEC_AAC;
    uint32_t sample_rate = 48000;
    uint32_t channels = 1;
    uint32_t bitrate = 128000;
    bool also_pcm = false;
    float volume = 1.0f;
    float capture_volume = -1.0f;
    float play_volume = -1.0f;
    bool mute = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--list") == 0)
        {
            do_list = true;
        }
        else if (std::strcmp(argv[i], "--list-playback") == 0)
        {
            do_list_playback = true;
        }
        else if (std::strcmp(argv[i], "--loopback") == 0 && i + 1 < argc)
        {
            loopback_sec = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--play-file") == 0 && i + 1 < argc)
        {
            play_file = argv[++i];
        }
        else if ((std::strcmp(argv[i], "--record-to") == 0 || std::strcmp(argv[i], "--output") == 0) &&
                 i + 1 < argc)
        {
            record_to_path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--record-encoded-to") == 0 && i + 1 < argc)
        {
            record_encoded_path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc)
        {
            device = argv[++i];
        }
        else if (std::strcmp(argv[i], "--play-device") == 0 && i + 1 < argc)
        {
            play_device = argv[++i];
        }
        else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
        {
            duration_sec = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc)
        {
            codec = parse_codec(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
        {
            sample_rate = static_cast<uint32_t>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--channels") == 0 && i + 1 < argc)
        {
            channels = static_cast<uint32_t>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc)
        {
            bitrate = static_cast<uint32_t>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--pcm") == 0)
        {
            also_pcm = true;
        }
        else if (std::strcmp(argv[i], "--volume") == 0 && i + 1 < argc)
        {
            volume = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--capture-volume") == 0 && i + 1 < argc)
        {
            capture_volume = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--play-volume") == 0 && i + 1 < argc)
        {
            play_volume = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--mute") == 0)
        {
            mute = true;
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (do_list)
    {
        return list_capture_devices();
    }
    if (do_list_playback)
    {
        return list_playback_devices();
    }

    const float cap_vol = (capture_volume >= 0.0f) ? capture_volume : volume;
    const float ply_vol = (play_volume >= 0.0f) ? play_volume : volume;

    if (loopback_sec >= 0)
    {
        return run_loopback(device, play_device, loopback_sec, sample_rate, channels, cap_vol, ply_vol, mute,
                            record_to_path);
    }
    if (!play_file.empty())
    {
        return run_play_file(play_file, play_device, sample_rate, channels, ply_vol, mute);
    }

    if (duration_sec <= 0)
    {
        duration_sec = 10;
    }
    return run_capture_test(device, duration_sec, codec, sample_rate, channels, bitrate, also_pcm, cap_vol, mute,
                            record_to_path, record_encoded_path);
}
