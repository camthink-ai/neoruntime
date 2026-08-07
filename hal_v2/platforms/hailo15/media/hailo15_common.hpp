/**
 * @file hailo15_common.hpp
 * @brief Shared helpers for Hailo-15 HAL adapters (MediaLibrary buffers ↔ HAL buffers).
 */
#pragma once

#include "common/hal_buffer.h"
#include "common/hal_common.h"

#include <hailo/media_library/buffer_pool.hpp>
#include <hailo/media_library/media_library_types.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

struct Hailo15FramePriv
{
    HailoMediaLibraryBufferPtr ml_buf;
};

struct Hailo15PacketPriv
{
    HailoMediaLibraryBufferPtr ml_buf;
};

inline HalPixelFormat hailo_format_to_hal(HailoFormat fmt)
{
    switch (fmt)
    {
        case HAILO_FORMAT_NV12:
            return HAL_PIX_FMT_NV12;
        case HAILO_FORMAT_GRAY8:
            return HAL_PIX_FMT_GRAY8;
        case HAILO_FORMAT_RGB:
            return HAL_PIX_FMT_RGB24;
        case HAILO_FORMAT_ARGB:
            return HAL_PIX_FMT_ARGB32;
        default:
            return HAL_PIX_FMT_NV12;
    }
}

inline void hailo15_fill_frame_from_buffer(const HailoMediaLibraryBufferPtr &buf, HalFrameBuffer *out)
{
    if (!buf || !buf->buffer_data || !out)
    {
        return;
    }

    const auto &bd = buf->buffer_data;
    out->width = static_cast<uint32_t>(bd->width);
    out->height = static_cast<uint32_t>(bd->height);
    out->format = hailo_format_to_hal(bd->format);
    out->mem_type = (bd->memory == HAILO_MEMORY_TYPE_DMABUF) ? HAL_MEM_DMABUF : HAL_MEM_MMAP;
    out->num_planes = static_cast<uint32_t>(bd->planes_count);
    if (out->num_planes > HAL_MAX_PLANES)
    {
        out->num_planes = HAL_MAX_PLANES;
    }

    for (uint32_t i = 0; i < out->num_planes; i++)
    {
        out->dma_fds[i] = buf->get_plane_fd(i);
        out->planes[i] = buf->get_plane_ptr(i);
        out->strides[i] = buf->get_plane_stride(i);
        out->sizes[i] = buf->get_plane_size(i);
    }
    for (uint32_t i = out->num_planes; i < HAL_MAX_PLANES; i++)
    {
        out->dma_fds[i] = -1;
        out->planes[i] = nullptr;
        out->strides[i] = 0;
        out->sizes[i] = 0;
    }

    out->timestamp_ns = buf->isp_timestamp_ns ? buf->isp_timestamp_ns : buf->pts;
    out->metadata = nullptr;
    auto *fp = new Hailo15FramePriv{};
    fp->ml_buf = buf;
    out->priv = fp;
}

/**
 * MediaLibrary normally reports the encoded payload length in its callback.
 * After a full pipeline rebuild some BSP versions instead report the backing
 * buffer capacity and leave the unused tail zero-filled. Passing that capacity
 * downstream turns a ~100-byte access unit into a 0.2-2 MB packet.
 *
 * Annex-B H.264/H.265 RBSP data ends with a stop bit, so zero bytes after the
 * last non-zero byte are trailing_zero_8bits / allocator padding and are safe
 * to remove. Only apply this recovery to buffers that actually begin with an
 * Annex-B start code; other packet formats keep the reported size unchanged.
 */
inline uint32_t hailo15_encoded_payload_size(const uint8_t *data, uint32_t reported_size,
                                             HalPacketType type)
{
    if (!data || reported_size < 4 ||
        (type != HAL_PACKET_TYPE_H264 && type != HAL_PACKET_TYPE_H265))
    {
        return reported_size;
    }

    const bool annex_b =
        (data[0] == 0 && data[1] == 0 && data[2] == 1) ||
        (reported_size >= 5 && data[0] == 0 && data[1] == 0 &&
         data[2] == 0 && data[3] == 1);
    if (!annex_b)
    {
        return reported_size;
    }

    uint32_t payload_size = reported_size;
    while (payload_size > 4 && data[payload_size - 1] == 0)
    {
        --payload_size;
    }
    return payload_size;
}

inline uint32_t hailo15_recover_encoded_payload_size(const uint8_t *data, uint32_t plane_size,
                                                     HalPacketType type)
{
    if (!data || plane_size < 4 ||
        (type != HAL_PACKET_TYPE_H264 && type != HAL_PACKET_TYPE_H265))
    {
        return 0;
    }

    const bool annex_b =
        (data[0] == 0 && data[1] == 0 && data[2] == 1) ||
        (plane_size >= 5 && data[0] == 0 && data[1] == 0 &&
         data[2] == 0 && data[3] == 1);
    if (!annex_b)
    {
        return 0;
    }

    uint32_t payload_size = plane_size;
    while (payload_size > 4 && data[payload_size - 1] == 0)
    {
        --payload_size;
    }
    return payload_size;
}

/**
 * @param valid_size Byte length reported by the MediaLibrary encoder callback.
 *                   Zero-filled capacity padding is normalized for Annex-B
 *                   H.264/H.265 by hailo15_encoded_payload_size().
 */
inline void hailo15_fill_packet_from_buffer(const HailoMediaLibraryBufferPtr &buf, HalPacketBuffer *out,
                                            uint32_t valid_size, HalPacketType type)
{
    if (!buf || !buf->buffer_data || !out)
    {
        return;
    }

    out->type = type;
    out->mem_type = HAL_MEM_MALLOC;
    out->dma_fd = -1;
    if (buf->get_num_of_planes() > 0)
    {
        out->data = static_cast<uint8_t *>(buf->get_plane_ptr(0));
        if (valid_size)
        {
            out->size = hailo15_encoded_payload_size(out->data, valid_size, type);
        }
        else
        {
            // libmedialib 1.11 can lose GstHailoBufferMeta::used_size after a
            // pipeline rebuild while the backing buffer still contains a
            // valid, zero-padded Annex-B access unit. Recover only when a start
            // code is present; all-zero/invalid buffers remain empty.
            out->size = hailo15_recover_encoded_payload_size(
                out->data, static_cast<uint32_t>(buf->get_plane_size(0)), type);
        }
    }
    else
    {
        out->data = nullptr;
        out->size = 0;
    }
    out->timestamp_ns = buf->isp_timestamp_ns ? buf->isp_timestamp_ns : buf->pts;
    out->metadata = nullptr;
    auto *pp = new Hailo15PacketPriv{};
    pp->ml_buf = buf;
    out->priv = pp;
}

inline bool hailo15_h264_is_keyframe(const uint8_t *data, uint32_t size)
{
    if (!data || size < 5)
    {
        return false;
    }
    for (uint32_t i = 0; i + 4 < size; i++)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            uint8_t nal = data[i + 3] & 0x1f;
            if (nal == 5 || nal == 7)
            {
                return true;
            }
        }
        if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        {
            uint8_t nal = data[i + 4] & 0x1f;
            if (nal == 5 || nal == 7)
            {
                return true;
            }
        }
    }
    return false;
}

inline std::string hailo15_read_file(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool hailo15_parse_profile_names_from_config_json(const std::string &json, std::vector<std::string> *out_names)
{
    if (!out_names)
    {
        return false;
    }
    out_names->clear();
    try
    {
        nlohmann::json j = nlohmann::json::parse(json);
        if (!j.contains("profiles"))
        {
            return false;
        }
        const auto &prof = j["profiles"];
        if (prof.is_object())
        {
            for (auto it = prof.begin(); it != prof.end(); ++it)
            {
                out_names->push_back(it.key());
            }
            return true;
        }
        if (prof.is_array())
        {
            for (const auto &el : prof)
            {
                if (el.is_object() && el.contains("name") && el["name"].is_string())
                {
                    out_names->push_back(el["name"].get<std::string>());
                }
            }
            return !out_names->empty();
        }
    }
    catch (...)
    {
    }
    return false;
}

inline int hailo15_ml_err(media_library_return r)
{
    switch (r)
    {
        case MEDIA_LIBRARY_SUCCESS:
            return HAL_OK;
        case MEDIA_LIBRARY_INVALID_ARGUMENT:
            return HAL_ERR_INVALID_ARG;
        case MEDIA_LIBRARY_OUT_OF_RESOURCES:
        case MEDIA_LIBRARY_BUFFER_ALLOCATION_ERROR:
            return HAL_ERR_NO_MEM;
        case MEDIA_LIBRARY_UNINITIALIZED:
            return HAL_ERR_NOT_INITIALIZED;
        case MEDIA_LIBRARY_PROFILE_IS_RESTRICTED:
            return HAL_ERR_PROFILE_RESTRICTED;
        case MEDIA_LIBRARY_PROFILE_VALIDATION_FAILED:
            return HAL_ERR_PROFILE_INVALID;
        default:
            return HAL_ERROR;
    }
}
