#include "config.h"
#include "log.h"
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace aipc::ai_runtime {

// Minimal YAML parser: supports flat key: value and simple nested blocks.
// For production, replace with yaml-cpp.

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string strip_comment(const std::string& s) {
    bool in_quote = false;
    char quote_char = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (!in_quote && (s[i] == '"' || s[i] == '\'')) {
            in_quote = true;
            quote_char = s[i];
        } else if (in_quote && s[i] == quote_char) {
            in_quote = false;
        } else if (!in_quote && s[i] == '#') {
            return s.substr(0, i);
        }
    }
    return s;
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

int indent_level(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else break;
    }
    return n;
}

std::invalid_argument config_parse_error(
    const char* key,
    const std::string& raw,
    const char* reason) {
    std::ostringstream oss;
    oss << "Invalid numeric config value for " << key << ": " << reason
        << " ('" << raw << "')";
    return std::invalid_argument(oss.str());
}

const char* consume_number_suffix(char* end) {
    while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    return end;
}

uint32_t parse_u32_config(
    const std::string& raw,
    const char* key,
    uint32_t max_value = std::numeric_limits<uint32_t>::max()) {
    std::string val = trim(raw);
    if (val.empty()) {
        throw config_parse_error(key, raw, "empty value");
    }

    char* end = nullptr;
    errno = 0;
    double parsed = std::strtod(val.c_str(), &end);
    if (end == val.c_str()) {
        throw config_parse_error(key, raw, "not a number");
    }
    if (*consume_number_suffix(end) != '\0') {
        throw config_parse_error(key, raw, "trailing characters");
    }
    if (errno == ERANGE || !std::isfinite(parsed)) {
        throw config_parse_error(key, raw, "out of range");
    }
    if (parsed < 0 || parsed > static_cast<double>(max_value)) {
        throw config_parse_error(key, raw, "outside unsigned integer range");
    }

    double rounded = std::round(parsed);
    if (parsed != rounded) {
        throw config_parse_error(key, raw, "must be an integer");
    }
    return static_cast<uint32_t>(rounded);
}

}  // namespace

Config load_config(const std::string& path) {
    Config cfg;

    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Cannot open config file: %s, using defaults", path.c_str());
        return cfg;
    }

    std::string section;     // e.g. "service", "hal", "scheduler"
    std::string subsection;  // e.g. "default_session"
    bool in_preload_item = false;
    PreloadModel current_preload;
    bool in_pipeline_item = false;
    AutoInferPipeline current_pipeline;

    // Block scalar state (for platform_config: |)
    std::string block_key;
    std::string block_content;
    int block_indent = -1;

    std::string line;
    while (std::getline(file, line)) {
        line = strip_comment(line);
        auto trimmed = trim(line);

        // Inside a block scalar: collect indented lines
        if (!block_key.empty()) {
            int indent = indent_level(line);
            if (!trimmed.empty() && indent > block_indent) {
                if (!block_content.empty()) block_content += '\n';
                block_content += trimmed;
                continue;
            }
            // Block ended — flush accumulated content
            if (section == "hal" && block_key == "platform_config") {
                cfg.hal_platform_config = block_content;
            }
            block_key.clear();
            block_content.clear();
            block_indent = -1;
            // Fall through to process current line normally
        }

        if (trimmed.empty() || trimmed[0] == '#') continue;

        int indent = indent_level(line);

        // Top-level section header (indent 0)
        if (indent == 0 && trimmed.back() == ':' && trimmed.find(' ') == std::string::npos) {
            if (in_preload_item && !current_preload.id.empty()) {
                cfg.preload_models.push_back(current_preload);
                current_preload = {};
            }
            if (in_pipeline_item && !current_pipeline.model_id.empty()) {
                cfg.auto_infer_pipelines.push_back(current_pipeline);
                current_pipeline = {};
            }
            section = trimmed.substr(0, trimmed.size() - 1);
            subsection.clear();
            in_preload_item = false;
            in_pipeline_item = false;
            continue;
        }

        // Sub-section header (indent 2)
        if (indent == 2 && trimmed.back() == ':' && trimmed.find(' ') == std::string::npos) {
            if (in_preload_item && !current_preload.id.empty()) {
                cfg.preload_models.push_back(current_preload);
                current_preload = {};
            }
            if (in_pipeline_item && !current_pipeline.model_id.empty()) {
                cfg.auto_infer_pipelines.push_back(current_pipeline);
                current_pipeline = {};
            }
            subsection = trimmed.substr(0, trimmed.size() - 1);
            in_preload_item = false;
            in_pipeline_item = false;
            continue;
        }

        // Preload list item start
        if (trimmed.substr(0, 2) == "- " && section == "models" && subsection == "preload") {
            if (in_preload_item && !current_preload.id.empty()) {
                cfg.preload_models.push_back(current_preload);
            }
            current_preload = {};
            in_preload_item = true;
            // Parse "- id: xxx"
            auto rest = trim(trimmed.substr(2));
            auto colon = rest.find(':');
            if (colon != std::string::npos) {
                auto key = trim(rest.substr(0, colon));
                auto val = unquote(trim(rest.substr(colon + 1)));
                if (key == "id") current_preload.id = val;
                else if (key == "path") current_preload.path = val;
                else if (key == "type") current_preload.type = val;
                else if (key == "postprocess_json") current_preload.postprocess_json = val;
            }
            continue;
        }

        // Continuation of preload item
        if (in_preload_item && indent >= 6) {
            auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                auto key = trim(trimmed.substr(0, colon));
                auto val = unquote(trim(trimmed.substr(colon + 1)));
                if (key == "id") current_preload.id = val;
                else if (key == "path") current_preload.path = val;
                else if (key == "type") current_preload.type = val;
                else if (key == "postprocess_json") current_preload.postprocess_json = val;
            }
            continue;
        }

        // Auto-infer pipeline list item start
        if (trimmed.substr(0, 2) == "- " && section == "auto_infer" && subsection == "pipelines") {
            if (in_pipeline_item && !current_pipeline.model_id.empty()) {
                cfg.auto_infer_pipelines.push_back(current_pipeline);
            }
            current_pipeline = {};
            in_pipeline_item = true;
            auto rest = trim(trimmed.substr(2));
            auto colon = rest.find(':');
            if (colon != std::string::npos) {
                auto key = trim(rest.substr(0, colon));
                auto val = unquote(trim(rest.substr(colon + 1)));
                if (key == "model_id") current_pipeline.model_id = val;
                else if (key == "stream_id") current_pipeline.stream_id = val;
                else if (key == "fps") current_pipeline.fps = parse_u32_config(val, "auto_infer.pipelines.fps");
            }
            continue;
        }

        // Continuation of pipeline item
        if (in_pipeline_item && indent >= 6) {
            auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                auto key = trim(trimmed.substr(0, colon));
                auto val = unquote(trim(trimmed.substr(colon + 1)));
                if (key == "model_id") current_pipeline.model_id = val;
                else if (key == "stream_id") current_pipeline.stream_id = val;
                else if (key == "fps") current_pipeline.fps = parse_u32_config(val, "auto_infer.pipelines.fps");
            }
            continue;
        }

        // Key: value pair
        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;
        auto key = trim(trimmed.substr(0, colon));
        auto val = unquote(trim(trimmed.substr(colon + 1)));
        if (val.empty()) continue;

        // Route to config fields
        if (section == "service") {
            if (key == "name")      cfg.service_name = val;
            else if (key == "listen")    cfg.listen_address = val;
            else if (key == "log_level") cfg.log_level = val;
            else if (key == "log_file")  cfg.log_file = val;
        } else if (section == "hal") {
            if (key == "library_path") cfg.hal_library_path = val;
            else if (key == "device_path") cfg.hal_device_path = val;
            else if (key == "platform_config" && (val == "|" || val == ">")) {
                // Start block scalar collection
                block_key = "platform_config";
                block_content.clear();
                block_indent = indent;
            }
        } else if (section == "models") {
            if (key == "repository_path") cfg.model_repository_path = val;
            else if (key == "cache_path") cfg.model_cache_path = val;
        } else if (section == "scheduler") {
            if (subsection == "default_session") {
                if (key == "max_qps")    cfg.default_session_max_qps = parse_u32_config(val, "scheduler.default_session.max_qps");
                else if (key == "priority") cfg.default_session_priority = parse_u32_config(val, "scheduler.default_session.priority");
            } else {
                if (key == "queue_size")              cfg.scheduler_queue_size = parse_u32_config(val, "scheduler.queue_size");
                else if (key == "timeout_ms")         cfg.scheduler_timeout_ms = parse_u32_config(val, "scheduler.timeout_ms");
                else if (key == "global_qps_limit")   cfg.global_qps_limit = parse_u32_config(val, "scheduler.global_qps_limit");
                else if (key == "global_concurrent_limit") cfg.scheduler_workers = parse_u32_config(val, "scheduler.global_concurrent_limit");
            }
        } else if (section == "postprocess") {
            if (key == "workers")         cfg.postprocess_workers = parse_u32_config(val, "postprocess.workers");
            else if (key == "queue_size") cfg.postprocess_queue_size = parse_u32_config(val, "postprocess.queue_size");
        } else if (section == "fd_receiver") {
            if (key == "socket_path") cfg.fd_socket_path = val;
        } else if (section == "performance") {
            if (key == "device_mode") cfg.device_mode = val;
        } else if (section == "event_bus") {
            if (key == "enabled")              cfg.event_bus_enabled = (val == "true");
            else if (key == "endpoint")        cfg.event_bus_endpoint = val;
            else if (key == "auto_publish_results") cfg.event_bus_auto_publish = (val == "true");
            else if (key == "result_topic_prefix")  cfg.event_bus_result_topic_prefix = val;
        } else if (section == "auto_infer") {
            if (key == "enabled") cfg.auto_infer_enabled = (val == "true");
        }
    }

    // Flush pending block scalar (in case file ended while in block)
    if (!block_key.empty() && section == "hal" && block_key == "platform_config") {
        cfg.hal_platform_config = block_content;
    }

    // Flush last preload item
    if (in_preload_item && !current_preload.id.empty()) {
        cfg.preload_models.push_back(current_preload);
    }

    // Flush last pipeline item
    if (in_pipeline_item && !current_pipeline.model_id.empty()) {
        cfg.auto_infer_pipelines.push_back(current_pipeline);
    }

    return cfg;
}

std::string parse_unix_address(const std::string& addr) {
    const std::string prefix = "unix://";
    if (addr.compare(0, prefix.size(), prefix) == 0) {
        return addr.substr(prefix.size());
    }
    return addr;
}

}  // namespace aipc::ai_runtime
