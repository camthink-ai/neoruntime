/**
 * @file hailo15_default_medialib.cpp
 * @brief Extract the compiled-in default medialib bundle to the scratch dir at runtime.
 */
#include "hailo15_default_medialib.hpp"

#include "common/hal_log.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

/* Defined in the build-generated hailo15_default_medialib_bundle.cpp */
extern const char *kHailo15DefaultMedialibBundle;

namespace hailo15
{
bool materialize_default_medialib_config(std::string &container_json_out, std::string *err_out)
{
    try
    {
        nlohmann::json bundle = nlohmann::json::parse(kHailo15DefaultMedialibBundle);
        const std::string scratch_root = bundle.at("scratch_root").get<std::string>();

        std::error_code ec;
        std::filesystem::create_directories(scratch_root, ec);

        const auto &files = bundle.at("files");
        for (auto it = files.begin(); it != files.end(); ++it)
        {
            const std::filesystem::path dst{it.key()};
            std::filesystem::create_directories(dst.parent_path(), ec);
            std::ofstream f(dst, std::ios::out | std::ios::trunc);
            if (!f.is_open())
            {
                if (err_out)
                {
                    *err_out = std::string("failed to open for write: ") + std::string(it.key());
                }
                HAL_LOG_ERROR("hailo15_media: default medialib: cannot write '%s'", it.key().c_str());
                return false;
            }
            f << it.value().dump();
            f.close();
        }

        container_json_out = bundle.at("container").dump();
        HAL_LOG_INFO("hailo15_media: default medialib: extracted %zu file(s) under '%s'",
                     static_cast<size_t>(files.size()), scratch_root.c_str());
        return true;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("hailo15_media: default medialib materialize failed: %s", e.what());
        if (err_out)
        {
            *err_out = e.what();
        }
        return false;
    }
}
} // namespace hailo15
