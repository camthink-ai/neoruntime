/**
 * @file hailo15_ml_frontend_bridge.hpp
 * @brief Shared MediaLibrary frontend subscription helpers (Hailo-15).
 */
#pragma once

#include "common/hal_common.h"
#include "media/hal_video_internal.h"

#include <hailo/media_library/media_library.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct Hailo15MediaPriv;

int hailo15_connect_media_priv_frontend(Hailo15MediaPriv *p);

int hailo15_connect_csi_medialib_frontend(const MediaLibraryPtr &media_lib,
                                          const std::vector<std::string> &frontend_stream_ids,
                                          std::recursive_mutex &mutex, uint64_t *frame_seq,
                                          std::map<std::string, std::pair<HalVideoFrameCallback, void *>> &video_subscribers,
                                          HalVideoContext *single_vc);
