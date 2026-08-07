/**
 * @file genai_example_v2.cpp
 * @brief Local HTTP server + web UI for HAL GenAI (LLM/VLM) streaming chat.
 */

#include "common/hal_common.h"
#include "image_decode_resize.hpp"
#include "model/hal_genai.h"

#include <hailo/hailort.h>

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using json = nlohmann::json;
namespace fs = std::filesystem;

struct ServerConfig {
    std::string hef_path;
    HalGenaiKind kind{HAL_GENAI_KIND_LLM};
    std::string host{"0.0.0.0"};
    int port{8080};
    std::string vdevice_group;
    /** Resolved manifest path (file), for /api/config only */
    std::string manifest_path;
    /** Optional decode size (CLI or manifest vlm_params); 0 = use SDK layout after load. */
    uint32_t vlm_decode_w{0};
    uint32_t vlm_decode_h{0};
    /** True when manifest.json supplies both vlm_params.frame_width and frame_height (manifest wins over CLI). */
    bool vlm_manifest_frame_dims{false};
};

/** Defaults merged into each /api/chat request (from manifest generation_params or built-ins). */
static HalGenaiGeneratorParams g_chat_gen_defaults{};
static std::string g_loaded_manifest_path;
static std::vector<std::string> g_manifest_stop_tokens;

struct ChatState {
    std::mutex mtx;
    std::vector<std::string> messages_json;
    HalGenaiSession *session{nullptr};
    HalGenaiKind kind{HAL_GENAI_KIND_LLM};
    HalGenaiVlmInputLayout vlm_layout{};
    bool vlm_layout_valid{false};
    /** Decode/resize target for uploads; may differ from SDK WxH when manifest requests another size (then linear resize to SDK). */
    uint32_t vlm_decode_w{0};
    uint32_t vlm_decode_h{0};
};

struct StreamTokenUserData {
    httplib::DataSink *sink{};
    std::shared_ptr<std::string> acc;
};

/** Remove chat special tokens that should not appear in UI or stored history (best-effort per fragment). */
static std::string strip_genai_special_tokens(std::string_view in)
{
    const std::string im_end = std::string("<|") + "im" + "_end|>";
    std::string s(in);
    for (;;)
    {
        const size_t p = s.find(im_end);
        if (p == std::string::npos)
            break;
        s.erase(p, im_end.size());
    }
    for (;;)
    {
        const size_t p = s.find("<|endoftext|>");
        if (p == std::string::npos)
            break;
        s.erase(p, sizeof("<|endoftext|>") - 1);
    }
    return s;
}

static void on_stream_token(const char *utf8_fragment, void *user)
{
    auto *u = static_cast<StreamTokenUserData *>(user);
    if (!utf8_fragment || !u->acc)
        return;
    *u->acc += utf8_fragment;
    if (!u->sink || !*utf8_fragment)
        return;
    const std::string out = strip_genai_special_tokens(utf8_fragment);
    if (out.empty())
        return;
    const json chunk = {{"token", out}};
    const std::string line = std::string("data: ") + chunk.dump() + "\n\n";
    u->sink->write(line.c_str(), line.size());
}

ServerConfig g_cfg;
ChatState g_state;

/** Match hailo15_genai_impl.cpp escape so LLM continuation prefixes align with HAL session history. */
std::string escape_json_content(const std::string &content)
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

std::string make_assistant_message_line(const std::string &plain_content)
{
    return std::string(R"({"role": "assistant", "content": ")") + escape_json_content(plain_content) + R"("})";
}

std::string make_user_message_llm(const std::string &text)
{
    return std::string(R"({"role":"user","content":")") + escape_json_content(text) + "\"}";
}

std::string make_user_message_vlm_text_only(const std::string &text)
{
    return make_user_message_llm(text);
}

std::string make_user_message_vlm_with_image(const std::string &text)
{
    return std::string(R"({"role":"user","content":[{"type":"text","text":")") + escape_json_content(text) +
           R"("},{"type":"image"}]})";
}

static HalGenaiGeneratorParams make_builtin_chat_defaults()
{
    HalGenaiGeneratorParams g{};
    /* Conservative defaults; manifest generation_params overrides. Keep frequency_penalty at 0 so HAL
     * skips set_frequency_penalty and the SDK uses its own default (nonzero fp harmed Qwen on HailoRT here). */
    g.temperature = 0.8f;
    g.top_p = 0.95f;
    g.top_k = 40;
    g.frequency_penalty = 0.f;
    g.max_generated_tokens = 256;
    g.do_sample = true;
    g.use_fixed_seed = false;
    g.seed = HAILO_RANDOM_SEED;
    return g;
}

static std::string json_string_lower(const json &j, const char *key)
{
    if (!j.contains(key) || !j[key].is_string())
        return {};
    std::string s = j[key].get<std::string>();
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool load_genai_manifest(const std::string &path_in, ServerConfig &out, std::string &err)
{
    fs::path p(path_in);
    try
    {
        if (fs::is_directory(p))
            p /= "manifest.json";
    }
    catch (const std::exception &e)
    {
        err = std::string("manifest path: ") + e.what();
        return false;
    }

    std::ifstream f(p);
    if (!f)
    {
        err = "cannot open manifest: " + p.string();
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception &e)
    {
        err = std::string("manifest JSON: ") + e.what();
        return false;
    }

    g_manifest_stop_tokens.clear();
    out.vlm_manifest_frame_dims = false;

    const std::string type = json_string_lower(j, "type");
    if (type == "vlm")
        out.kind = HAL_GENAI_KIND_VLM;
    else if (type == "llm")
        out.kind = HAL_GENAI_KIND_LLM;

    for (const char *key : {"hef_path", "model_path", "hef"})
    {
        if (j.contains(key) && j[key].is_string())
        {
            out.hef_path = j[key].get<std::string>();
            break;
        }
    }

    if (j.contains("host") && j["host"].is_string())
        out.host = j["host"].get<std::string>();
    if (j.contains("port") && j["port"].is_number_integer())
        out.port = j["port"].get<int>();
    if (j.contains("vdevice_group") && j["vdevice_group"].is_string())
        out.vdevice_group = j["vdevice_group"].get<std::string>();

    if (j.contains("vlm_params") && j["vlm_params"].is_object())
    {
        const json &vp = j["vlm_params"];
        bool have_w = false, have_h = false;
        if (vp.contains("frame_width") && vp["frame_width"].is_number())
        {
            out.vlm_decode_w = vp["frame_width"].get<uint32_t>();
            have_w = true;
        }
        if (vp.contains("frame_height") && vp["frame_height"].is_number())
        {
            out.vlm_decode_h = vp["frame_height"].get<uint32_t>();
            have_h = true;
        }
        out.vlm_manifest_frame_dims = have_w && have_h;
    }

    g_chat_gen_defaults = make_builtin_chat_defaults();
    if (j.contains("generation_params") && j["generation_params"].is_object())
    {
        const json &gp = j["generation_params"];
        if (gp.contains("temperature") && gp["temperature"].is_number())
            g_chat_gen_defaults.temperature = gp["temperature"].get<float>();
        if (gp.contains("top_p") && gp["top_p"].is_number())
            g_chat_gen_defaults.top_p = gp["top_p"].get<float>();
        if (gp.contains("top_k") && gp["top_k"].is_number_integer())
            g_chat_gen_defaults.top_k = gp["top_k"].get<uint32_t>();
        if (gp.contains("max_tokens") && gp["max_tokens"].is_number_integer())
            g_chat_gen_defaults.max_generated_tokens = gp["max_tokens"].get<uint32_t>();
        else if (gp.contains("max_generated_tokens") && gp["max_generated_tokens"].is_number_integer())
            g_chat_gen_defaults.max_generated_tokens = gp["max_generated_tokens"].get<uint32_t>();
        if (gp.contains("frequency_penalty") && gp["frequency_penalty"].is_number())
            g_chat_gen_defaults.frequency_penalty = gp["frequency_penalty"].get<float>();
        if (gp.contains("do_sample") && gp["do_sample"].is_boolean())
            g_chat_gen_defaults.do_sample = gp["do_sample"].get<bool>();
        if (gp.contains("stop_tokens") && gp["stop_tokens"].is_array())
        {
            for (const auto &el : gp["stop_tokens"])
            {
                if (el.is_string())
                    g_manifest_stop_tokens.push_back(el.get<std::string>());
            }
        }
    }

    if (j.contains("template_params") && j["template_params"].is_object())
    {
        const json &tp = j["template_params"];
        if (tp.contains("eos_token") && tp["eos_token"].is_string())
        {
            const std::string eos = tp["eos_token"].get<std::string>();
            if (!eos.empty() &&
                std::find(g_manifest_stop_tokens.begin(), g_manifest_stop_tokens.end(), eos) ==
                    g_manifest_stop_tokens.end())
                g_manifest_stop_tokens.push_back(eos);
        }
    }

    out.manifest_path = p.string();
    g_loaded_manifest_path = out.manifest_path;
    return true;
}

bool parse_args(int argc, char **argv, ServerConfig &out)
{
    out = ServerConfig{};
    g_chat_gen_defaults = make_builtin_chat_defaults();
    g_loaded_manifest_path.clear();
    g_manifest_stop_tokens.clear();

    int i = 1;
    if (i < argc && !std::strcmp(argv[i], "serve"))
        ++i;

    std::optional<std::string> manifest_path_arg;
    for (int j = i; j < argc; ++j)
    {
        if ((!std::strcmp(argv[j], "--manifest") || !std::strcmp(argv[j], "-manifest")) && j + 1 < argc)
            manifest_path_arg = argv[j + 1];
    }

    for (; i < argc; ++i)
    {
        const char *a = argv[i];
        if ((!std::strcmp(a, "--manifest") || !std::strcmp(a, "-manifest")) && i + 1 < argc)
        {
            ++i;
            continue;
        }
        if ((!std::strcmp(a, "--hef") || !std::strcmp(a, "-hef")) && i + 1 < argc)
            out.hef_path = argv[++i];
        else if ((!std::strcmp(a, "--kind") || !std::strcmp(a, "-kind")) && i + 1 < argc)
        {
            const char *k = argv[++i];
            if (!std::strcmp(k, "vlm"))
                out.kind = HAL_GENAI_KIND_VLM;
            else
                out.kind = HAL_GENAI_KIND_LLM;
        }
        else if ((!std::strcmp(a, "--host") || !std::strcmp(a, "-host")) && i + 1 < argc)
            out.host = argv[++i];
        else if ((!std::strcmp(a, "--port") || !std::strcmp(a, "-port")) && i + 1 < argc)
            out.port = std::atoi(argv[++i]);
        else if ((!std::strcmp(a, "--vdevice-group") || !std::strcmp(a, "-vdevice-group")) && i + 1 < argc)
            out.vdevice_group = argv[++i];
        else if ((!std::strcmp(a, "--vlm-frame-w") || !std::strcmp(a, "-vlm-frame-w")) && i + 1 < argc)
            out.vlm_decode_w = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if ((!std::strcmp(a, "--vlm-frame-h") || !std::strcmp(a, "-vlm-frame-h")) && i + 1 < argc)
            out.vlm_decode_h = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help"))
        {
            std::fprintf(stderr,
                "Usage: %s [serve] [--manifest <manifest.json|dir>] [--hef|-hef <path.hef>] "
                "[--kind|-kind llm|vlm] [--host|-host 0.0.0.0] [--port|-port 8080] [--vdevice-group|-vdevice-group ID]\n"
                "       [--vlm-frame-w W --vlm-frame-h H]   (CLI decode size; overridden by manifest vlm_params when both present)\n"
                "  Zoo-style manifest.json may set \"type\" (llm|vlm), vlm_params.frame_width/height, generation_params (incl. stop_tokens).\n"
                "  Manifest is loaded after CLI and overrides overlapping settings (manifest preferred).\n",
                argv[0]);
            return false;
        }
    }

    if (manifest_path_arg.has_value())
    {
        std::string err;
        if (!load_genai_manifest(*manifest_path_arg, out, err))
        {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    }

    if (out.hef_path.empty())
    {
        std::fprintf(stderr, "error: set hef path via --hef or manifest hef_path/model_path\n");
        return false;
    }
    return true;
}

std::vector<uint8_t> base64_decode(const std::string &encoded)
{
    static unsigned char kDec[256];
    static bool table_ready = false;
    if (!table_ready)
    {
        std::memset(kDec, 64, sizeof(kDec));
        const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            kDec[static_cast<unsigned char>(alpha[i])] = static_cast<unsigned char>(i);
        table_ready = true;
    }

    std::string in;
    for (char c : encoded)
    {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        in.push_back(c);
    }
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3 + 4);
    int val = 0;
    int valb = -8;
    for (unsigned char c : in)
    {
        if (kDec[c] >= 64)
            break;
        val = (val << 6) + static_cast<int>(kDec[c]);
        valb += 6;
        if (valb >= 0)
        {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return out;
}

HalGenaiGeneratorParams parse_generator_params(const json &body)
{
    HalGenaiGeneratorParams g = g_chat_gen_defaults;
    if (body.contains("temperature") && !body["temperature"].is_null())
        g.temperature = body["temperature"].get<float>();
    if (body.contains("top_p") && !body["top_p"].is_null())
        g.top_p = body["top_p"].get<float>();
    if (body.contains("top_k") && !body["top_k"].is_null())
        g.top_k = body["top_k"].get<uint32_t>();
    if (body.contains("frequency_penalty") && !body["frequency_penalty"].is_null())
        g.frequency_penalty = body["frequency_penalty"].get<float>();
    if (body.contains("max_tokens") && !body["max_tokens"].is_null())
        g.max_generated_tokens = body["max_tokens"].get<uint32_t>();
    g.do_sample = body.value("do_sample", g.do_sample);
    if (body.contains("seed") && !body["seed"].is_null())
    {
        g.use_fixed_seed = true;
        g.seed = body["seed"].get<uint32_t>();
    }
    else
    {
        g.use_fixed_seed = false;
        g.seed = HAILO_RANDOM_SEED;
    }
    return g;
}

/** Qwen-style chat EOS strings (built in pieces so tooling does not alter literals). */
static std::string qwen_im_end_token()
{
    return std::string("<|") + "im" + "_end|>";
}

static void append_unique_stop(std::vector<std::string> &v, const std::string &t)
{
    if (t.empty())
        return;
    if (std::find(v.begin(), v.end(), t) != v.end())
        return;
    v.push_back(t);
}

/** Ensure primary Qwen chat EOS is present (manifest copies sometimes omit it). */
static void enrich_qwen_like_stop_tokens(std::vector<std::string> &v)
{
    append_unique_stop(v, qwen_im_end_token());
}

static void apply_manifest_stop_tokens(HalGenaiSession *sess)
{
    std::vector<std::string> toks = g_manifest_stop_tokens;
    if (!g_loaded_manifest_path.empty() || g_cfg.kind == HAL_GENAI_KIND_VLM)
        enrich_qwen_like_stop_tokens(toks);
    std::vector<const char *> ptrs;
    ptrs.reserve(toks.size());
    for (const auto &t : toks)
        ptrs.push_back(t.c_str());
    const int rc =
        HAL_GENAI_OPS.set_stop_tokens(sess, ptrs.empty() ? nullptr : ptrs.data(), static_cast<int>(ptrs.size()));
    if (rc != HAL_OK)
        std::fprintf(stderr, "warning: HAL_GENAI_OPS.set_stop_tokens failed (%d)\n", rc);
}

/** Resolve VLM decode WxH: manifest full frame pair may differ from SDK (then we decode at manifest size and resize to SDK in /api/chat). */
static void configure_vlm_decode_and_layout(HalGenaiSession *sess)
{
    HalGenaiVlmInputLayout lay{};
    if (HAL_GENAI_OPS.get_vlm_input_layout(sess, &lay) != HAL_OK)
        return;

    uint32_t dw = g_cfg.vlm_decode_w;
    uint32_t dh = g_cfg.vlm_decode_h;
    const uint32_t f = lay.features ? lay.features : 3u;
    if (dw > 0 && dh > 0)
    {
        const uint64_t need = static_cast<uint64_t>(dw) * static_cast<uint64_t>(dh) * static_cast<uint64_t>(f);
        if (need != lay.bytes_per_frame)
        {
            if (g_cfg.vlm_manifest_frame_dims)
            {
                std::fprintf(stderr,
                    "info: manifest VLM frame %ux%u x%u features => %llu bytes; SDK expects %u bytes (%ux%u); "
                    "images decode at manifest size then resize to SDK before inference.\n",
                    (unsigned)dw, (unsigned)dh, (unsigned)f, (unsigned long long)need, (unsigned)lay.bytes_per_frame,
                    (unsigned)lay.width, (unsigned)lay.height);
            }
            else
            {
                std::fprintf(stderr,
                    "warning: requested VLM frame %ux%u x%u features => %llu bytes, SDK expects %u bytes (%ux%u); "
                    "using SDK dimensions for decode.\n",
                    (unsigned)dw, (unsigned)dh, (unsigned)f, (unsigned long long)need, (unsigned)lay.bytes_per_frame,
                    (unsigned)lay.width, (unsigned)lay.height);
                dw = lay.width;
                dh = lay.height;
            }
        }
    }
    else
    {
        dw = lay.width;
        dh = lay.height;
    }

    std::lock_guard<std::mutex> lock(g_state.mtx);
    g_state.vlm_layout = lay;
    g_state.vlm_layout_valid = true;
    g_state.vlm_decode_w = dw;
    g_state.vlm_decode_h = dh;
}

json api_config_json()
{
    std::lock_guard<std::mutex> lock(g_state.mtx);
    json j;
    j["kind"] = (g_state.kind == HAL_GENAI_KIND_VLM) ? "vlm" : "llm";
    if (!g_loaded_manifest_path.empty())
        j["manifest"] = g_loaded_manifest_path;
    j["hef"] = g_cfg.hef_path;
    if (g_state.vlm_layout_valid)
    {
        j["vlm"] = {
            {"sdk", {{"width", g_state.vlm_layout.width},
                     {"height", g_state.vlm_layout.height},
                     {"features", g_state.vlm_layout.features},
                     {"bytes_per_frame", g_state.vlm_layout.bytes_per_frame}}},
            {"decode", {{"width", g_state.vlm_decode_w}, {"height", g_state.vlm_decode_h}}}};
    }
    return j;
}

const char *kIndexHtml = R"HTML(
<!DOCTYPE html>
<html><head><meta charset="utf-8"/><title>HAL GenAI</title>
<style>
body{font-family:system-ui,sans-serif;max-width:720px;margin:24px auto;padding:0 12px}
#log{white-space:pre-wrap;border:1px solid #ccc;padding:12px;min-height:200px;background:#fafafa}
.row{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0}
button{padding:8px 14px;cursor:pointer}
textarea{width:100%;min-height:72px}
.status{font-size:13px;color:#444}
</style></head>
<body>
<h2>hal-genai-example-v2</h2>
<p class="status" id="cfg"></p>
<textarea id="in" placeholder="Message"></textarea>
<div class="row"><input type="file" id="img" accept="image/*"/><span id="imgnote"></span></div>
<div class="row">
<button id="send">Send</button>
<button id="stop">Stop</button>
<button id="clear">Clear context</button>
</div>
<div id="log"></div>
<script>
const log = document.getElementById('log');
const cfgEl = document.getElementById('cfg');
let streaming = false;
async function refreshCfg(){
  const r = await fetch('/api/config');
  const j = await r.json();
  let s = 'Mode: '+j.kind;
  if(j.hef) s += ' — HEF '+j.hef;
  if(j.manifest) s += ' — manifest '+j.manifest;
  if(j.vlm) {
    const d = j.vlm.decode || {};
    const sdk = j.vlm.sdk || {};
    s += ' — decode '+(d.width||'?')+'x'+(d.height||'?')+', SDK '+(sdk.width||'?')+'x'+(sdk.height||'?');
  }
  cfgEl.textContent = s;
  document.getElementById('imgnote').textContent = (j.kind==='vlm') ? 'Optional image (VLM); each send is one turn (no server-side VLM history)' : 'Image ignored (LLM)';
}
function stripGenTokens(s){
  const im = '<|' + 'im' + '_end|>';
  return s.split(im).join('').split('<|endoftext|>').join('');
}
function appendRole(role, text){
  log.textContent += '['+role+'] '+text+'\n';
  log.scrollTop = log.scrollHeight;
}
async function onSend(){
  if(streaming) return;
  const text = document.getElementById('in').value.trim();
  if(!text) return;
  const f = document.getElementById('img').files[0];
  let image_b64 = null;
  if(f){
    const buf = await f.arrayBuffer();
    let bin = '';
    const bytes = new Uint8Array(buf);
    for(let i=0;i<bytes.byteLength;i++) bin += String.fromCharCode(bytes[i]);
    image_b64 = btoa(bin);
  }
  const body = { text, image_base64: image_b64 };
  streaming = true;
  appendRole('user', text);
  log.textContent += '[assistant] ';
  try{
    const resp = await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    if(!resp.ok){
      const t = await resp.text();
      log.textContent += '\n[http '+resp.status+'] '+(t||'')+'\n';
      streaming = false;
      document.getElementById('in').value = '';
      return;
    }
    if(!resp.body){
      log.textContent += '\n[fetch error] no response body\n';
      streaming = false;
      document.getElementById('in').value = '';
      return;
    }
    const rd = resp.body.getReader();
    const dec = new TextDecoder();
    let buf = '';
    while(true){
      const {value,done} = await rd.read();
      if(done) break;
      buf += dec.decode(value,{stream:true});
      let idx;
      while((idx = buf.indexOf('\n\n')) >= 0){
        const block = buf.slice(0,idx);
        buf = buf.slice(idx+2);
        const lines = block.split('\n');
        for(const line of lines){
          if(line.startsWith('data: ')){
            const js = line.slice(6);
            if(js.trim()==='[DONE]') break;
            try{
              const o = JSON.parse(js);
              if(o.token) log.textContent += stripGenTokens(o.token);
              if(o.error) log.textContent += '\n[error] '+o.error+'\n';
            }catch(e){}
          }
        }
        log.scrollTop = log.scrollHeight;
      }
    }
    const lines = log.textContent.split('\n');
    if(lines.length) lines[lines.length-1] = stripGenTokens(lines[lines.length-1]);
    log.textContent = lines.join('\n');
    log.textContent += '\n';
  }catch(e){
    log.textContent += '\n[fetch error] '+e+'\n';
  }
  streaming = false;
  document.getElementById('in').value = '';
}
async function onStop(){ await fetch('/api/chat/abort',{method:'POST'}); }
async function onClear(){ await fetch('/api/session/reset',{method:'POST'}); log.textContent = ''; }
document.getElementById('send').onclick = onSend;
document.getElementById('stop').onclick = onStop;
document.getElementById('clear').onclick = onClear;
refreshCfg();
</script>
</body></html>
)HTML";

} // namespace

int main(int argc, char **argv)
{
    if (!parse_args(argc, argv, g_cfg))
        return 1;

    HalGenaiCreateParams cp{};
    std::strncpy(cp.hef_path, g_cfg.hef_path.c_str(), sizeof(cp.hef_path) - 1);
    cp.hef_path[sizeof(cp.hef_path) - 1] = '\0';
    cp.kind = g_cfg.kind;
    cp.vdevice_group_id = g_cfg.vdevice_group.empty() ? nullptr : g_cfg.vdevice_group.c_str();
    cp.lora_name = nullptr;
    cp.optimize_memory_on_device = false;

    HalGenaiSession *sess = HAL_GENAI_OPS.create(&cp);
    if (!sess)
    {
        std::fprintf(stderr, "HAL_GENAI_OPS.create failed (check --hef / device)\n");
        return 1;
    }

    // configure_vlm_decode_and_layout locks g_state.mtx internally — must not call it while already holding the lock.
    if (g_cfg.kind == HAL_GENAI_KIND_VLM)
        configure_vlm_decode_and_layout(sess);
    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.session = sess;
        g_state.kind = g_cfg.kind;
    }
    apply_manifest_stop_tokens(sess);
    if (HAL_GENAI_OPS.set_generator_params(sess, &g_chat_gen_defaults) != HAL_OK)
        std::fprintf(stderr, "warning: HAL_GENAI_OPS.set_generator_params failed\n");

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(kIndexHtml, "text/html; charset=utf-8");
    });

    svr.Get("/api/config", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(api_config_json().dump(), "application/json");
    });

    svr.Post("/api/session/reset", [](const httplib::Request &, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        if (!g_state.session)
        {
            res.status = 500;
            return;
        }
        if (HAL_GENAI_OPS.clear_context(g_state.session) != HAL_OK)
        {
            res.status = 400;
            res.set_content(json{{"error", "clear_context failed (generation active?)"}}.dump(), "application/json");
            return;
        }
        g_state.messages_json.clear();
        res.set_content(R"({"ok":true})", "application/json");
    });

    svr.Post("/api/chat/abort", [](const httplib::Request &, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        if (g_state.session)
            HAL_GENAI_OPS.abort_generation(g_state.session);
        res.set_content(R"({"ok":true})", "application/json");
    });

    svr.Post("/api/chat", [](const httplib::Request &req, httplib::Response &res) {
        json body;
        try
        {
            body = json::parse(req.body);
        }
        catch (...)
        {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }

        const std::string user_text = body.value("text", "");
        if (user_text.empty())
        {
            res.status = 400;
            res.set_content(R"({"error":"empty text"})", "application/json");
            return;
        }

        const HalGenaiGeneratorParams gp = parse_generator_params(body);

        std::optional<std::vector<uint8_t>> image_bytes;
        if (body.contains("image_base64") && !body["image_base64"].is_null())
        {
            try
            {
                image_bytes = base64_decode(body["image_base64"].get<std::string>());
            }
            catch (...)
            {
                res.status = 400;
                res.set_content(R"({"error":"bad image_base64"})", "application/json");
                return;
            }
        }

        std::shared_ptr<std::vector<std::string>> batch = std::make_shared<std::vector<std::string>>();
        std::shared_ptr<std::vector<uint8_t>> rgb_storage = std::make_shared<std::vector<uint8_t>>();
        HalGenaiKind kind_copy{};
        HalGenaiSession *sess_copy{};

        {
            std::lock_guard<std::mutex> lock(g_state.mtx);
            kind_copy = g_state.kind;
            sess_copy = g_state.session;
            if (!sess_copy)
            {
                res.status = 500;
                res.set_content(R"({"error":"no session"})", "application/json");
                return;
            }

            /* VLM: HAL clears device context every generate; keeping multi-turn JSON here causes
             * placeholder/frame mismatches and "cannot see image" answers to poison later turns. */
            if (kind_copy == HAL_GENAI_KIND_VLM)
                g_state.messages_json.clear();

            std::string user_line;
            if (kind_copy == HAL_GENAI_KIND_VLM && image_bytes.has_value())
            {
                if (!g_state.vlm_layout_valid)
                {
                    res.status = 500;
                    res.set_content(R"({"error":"VLM layout unknown"})", "application/json");
                    return;
                }
                user_line = make_user_message_vlm_with_image(user_text);
                try
                {
                    DecodedRgbFrame dec =
                        decode_and_resize_image(*image_bytes, g_state.vlm_decode_w, g_state.vlm_decode_h);
                    if (dec.pixels.size() != g_state.vlm_layout.bytes_per_frame)
                    {
                        if (!g_cfg.vlm_manifest_frame_dims)
                        {
                            res.status = 500;
                            res.set_content(json{{"error", "decoded image size mismatch SDK input_frame_size"},
                                                 {"expected", g_state.vlm_layout.bytes_per_frame},
                                                 {"got", dec.pixels.size()}}
                                                    .dump(),
                                            "application/json");
                            return;
                        }
                        dec = resize_rgb888_linear(dec.pixels, dec.width, dec.height, g_state.vlm_layout.width,
                                                   g_state.vlm_layout.height);
                        if (dec.pixels.size() != g_state.vlm_layout.bytes_per_frame)
                        {
                            res.status = 500;
                            res.set_content(json{{"error", "resize to SDK frame size failed size check"},
                                                 {"expected", g_state.vlm_layout.bytes_per_frame},
                                                 {"got", dec.pixels.size()}}
                                                    .dump(),
                                            "application/json");
                            return;
                        }
                    }
                    *rgb_storage = std::move(dec.pixels);
                }
                catch (const std::exception &e)
                {
                    res.status = 400;
                    res.set_content(json{{"error", std::string("image decode/resize: ") + e.what()}}.dump(),
                                    "application/json");
                    return;
                }
            }
            else if (kind_copy == HAL_GENAI_KIND_VLM)
                user_line = make_user_message_vlm_text_only(user_text);
            else
                user_line = make_user_message_llm(user_text);

            g_state.messages_json.push_back(user_line);
            *batch = g_state.messages_json;
        }

        res.set_header("Cache-Control", "no-cache");
        const HalGenaiGeneratorParams gp_copy = gp;
        res.set_chunked_content_provider(
            "text/event-stream; charset=utf-8",
            [batch, gp_copy, kind_copy, sess_copy, rgb_storage](size_t offset,
                                                              httplib::DataSink &sink) mutable -> bool {
                if (offset != 0)
                    return false;

                std::vector<const char *> ptrs;
                ptrs.reserve(batch->size());
                for (auto &m : *batch)
                    ptrs.push_back(m.c_str());

                std::vector<HalGenaiImageFrame> frames;
                if (kind_copy == HAL_GENAI_KIND_VLM && !rgb_storage->empty())
                    frames.push_back({rgb_storage->data(), rgb_storage->size()});

                auto acc = std::make_shared<std::string>();
                StreamTokenUserData ud{};
                ud.sink = &sink;
                ud.acc = acc;

                const int rc = HAL_GENAI_OPS.generate_stream(
                    sess_copy, ptrs.data(), static_cast<int>(ptrs.size()),
                    frames.empty() ? nullptr : frames.data(), static_cast<int>(frames.size()), &gp_copy,
                    on_stream_token, &ud,
                    [](HalGenaiFinishReason, int, void *) {}, nullptr);

                if (rc != HAL_OK)
                {
                    json er;
                    er["error"] = "generate_stream failed";
                    er["code"] = rc;
                    const std::string line = std::string("data: ") + er.dump() + "\n\n";
                    sink.write(line.c_str(), line.size());
                    sink.write("data: [DONE]\n\n", 16);
                    return false;
                }

                if (!acc->empty())
                {
                    const std::string cleaned = strip_genai_special_tokens(*acc);
                    std::lock_guard<std::mutex> lock(g_state.mtx);
                    g_state.messages_json.push_back(make_assistant_message_line(cleaned));
                }

                sink.write("data: [DONE]\n\n", 16);
                return false;
            });
    });

    std::fprintf(stderr, "hal-genai-example-v2 listening on http://%s:%d\n", g_cfg.host.c_str(), g_cfg.port);
    if (!svr.listen(g_cfg.host.c_str(), g_cfg.port))
    {
        std::fprintf(stderr, "listen failed\n");
        HAL_GENAI_OPS.destroy(sess);
        return 1;
    }

    HAL_GENAI_OPS.destroy(sess);
    return 0;
}
