// Unit tests for the P2-13 overlay session lifecycle: the session/end
// topic discriminator and the polygon-sidecar sweep it triggers
// (is_overlay_session_end_topic / handle_event / handle_session_end /
// polygon_count). The harness drives handle_event directly — the same
// body the subscriber loop runs per bus event — so the tag-and-sweep
// semantics are testable with no live event bus.

#include "ai_overlay_subscriber.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>

namespace {

const char* kZonePayload =
    "{\"polygons\":[{\"points\":[[0.1,0.1],[0.9,0.1],[0.9,0.9]],"
    "\"label\":\"zone\"}]}";

} // namespace

int main() {
    /* ---- is_overlay_session_end_topic: exact prefix+session/end match ---- */
    {
        assert(is_overlay_session_end_topic("inference/session/end", "inference/"));
        assert(!is_overlay_session_end_topic("inference/main", "inference/"));
        assert(!is_overlay_session_end_topic("inference/session/end/x", "inference/"));
        assert(!is_overlay_session_end_topic("inference/session/end", "events/"));
        assert(!is_overlay_session_end_topic("inference/session", "inference/"));
        assert(!is_overlay_session_end_topic("", "inference/"));
        /* A result topic literally named model="session", stream="end"
         * would collide; the exact-match contract reserves this topic. */
    }

    /* ---- tagged polygons are swept by their session's end ---- */
    {
        /* 3-arg handle_event calls are platform-source events; admission
         * requires the legacy switch under the binding-decoupling default */
        AiOverlayConfig cfg;
        cfg.legacy_auto_bind = true;
        AiOverlaySubscriber sub(cfg);
        std::map<std::string, std::string> md;
        md["stream_id"] = "main";
        md["session_id"] = "s1";
        sub.handle_event("inference/main", md, kZonePayload);
        assert(sub.polygon_count("main") == 1);

        md["stream_id"] = "sub";
        sub.handle_event("inference/sub", md, kZonePayload);
        assert(sub.polygon_count("sub") == 1);

        /* a different session's end sweeps nothing */
        md.clear();
        md["session_id"] = "other";
        sub.handle_event("inference/session/end", md, "");
        assert(sub.polygon_count("main") == 1);
        assert(sub.polygon_count("sub") == 1);

        /* the owning session's end clears both streams' sidecars */
        md["session_id"] = "s1";
        sub.handle_event("inference/session/end", md, "");
        assert(sub.polygon_count("main") == 0);
        assert(sub.polygon_count("sub") == 0);
    }

    /* ---- untagged writes are never swept (latest write re-owns) ---- */
    {
        /* 3-arg handle_event calls are platform-source events; admission
         * requires the legacy switch under the binding-decoupling default */
        AiOverlayConfig cfg;
        cfg.legacy_auto_bind = true;
        AiOverlaySubscriber sub(cfg);
        std::map<std::string, std::string> md;
        md["stream_id"] = "main";
        md["session_id"] = "s1";
        sub.handle_event("inference/main", md, kZonePayload);
        assert(sub.polygon_count("main") == 1);

        /* follow-up write without a session tag resets ownership */
        md.erase("session_id");
        sub.handle_event("inference/main", md, kZonePayload);
        assert(sub.polygon_count("main") == 1);

        md["session_id"] = "s1";
        sub.handle_event("inference/session/end", md, "");
        assert(sub.polygon_count("main") == 1); /* untagged write survives */
    }

    /* ---- handle_session_end direct: return value + idempotence ---- */
    {
        /* 3-arg handle_event calls are platform-source events; admission
         * requires the legacy switch under the binding-decoupling default */
        AiOverlayConfig cfg;
        cfg.legacy_auto_bind = true;
        AiOverlaySubscriber sub(cfg);
        std::map<std::string, std::string> md;
        md["stream_id"] = "main";
        md["session_id"] = "s1";
        sub.handle_event("inference/main", md, kZonePayload);
        md["stream_id"] = "sub";
        sub.handle_event("inference/sub", md, kZonePayload);
        assert(sub.handle_session_end("s1") == 2);
        assert(sub.handle_session_end("s1") == 0); /* idempotent */
        assert(sub.handle_session_end("") == 0);   /* empty id is a no-op */
        assert(sub.polygon_count("main") == 0);
        assert(sub.polygon_count("sub") == 0);
    }

    /* ---- session/end without session_id metadata is ignored ---- */
    {
        /* 3-arg handle_event calls are platform-source events; admission
         * requires the legacy switch under the binding-decoupling default */
        AiOverlayConfig cfg;
        cfg.legacy_auto_bind = true;
        AiOverlaySubscriber sub(cfg);
        std::map<std::string, std::string> md;
        md["stream_id"] = "main";
        md["session_id"] = "s1";
        sub.handle_event("inference/main", md, kZonePayload);
        md.clear(); /* no session_id at all */
        sub.handle_event("inference/session/end", md, "");
        assert(sub.polygon_count("main") == 1);
    }

    std::printf("ai_overlay_session_test: all assertions passed\n");
    return 0;
}
