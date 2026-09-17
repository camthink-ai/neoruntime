// Unit tests for the AI overlay v2 helpers: parse_overlay_polygons()
// (wire sidecar channel), apply_track_id_label() (detection label suffix),
// resolve_result_ttl_ms() (per-result/stream/global/fps-derived chain), and
// compose_draw_config() (HAL draw config seeding + per-frame overrides).

#include "ai_overlay_subscriber.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

int main() {
    /* ---- parse_overlay_polygons: absent key leaves the sidecar untouched ---- */
    {
        std::vector<OverlayPolygon> out;
        out.push_back({{{0.1f, 0.1f}, {0.9f, 0.1f}}, "old", true, 0, 0, false, false});
        assert(parse_overlay_polygons("{\"num_detections\":0,\"detections\":[]}", &out) == false);
        assert(out.size() == 1 && out[0].label == "old");
        assert(parse_overlay_polygons("{\"polygons\":notanarray}", &out) == false);
        assert(out.size() == 1 && out[0].label == "old");
    }

    /* ---- full polygon: points, label, closed, color, thickness ---- */
    {
        std::vector<OverlayPolygon> out;
        const char* payload =
            "{\"polygons\":[{\"points\":[[0.1,0.2],[0.3,0.4],[0.5,0.6]],"
            "\"label\":\"yard\",\"closed\":false,\"color\":4294901760,\"thickness\":3}]}";
        assert(parse_overlay_polygons(payload, &out) == true);
        assert(out.size() == 1);
        const auto& p = out[0];
        assert(p.points.size() == 3);
        assert(p.points[0].first == 0.1f && p.points[0].second == 0.2f);
        assert(p.points[2].first == 0.5f && p.points[2].second == 0.6f);
        assert(p.label == "yard");
        assert(p.closed == false);
        assert(p.has_color && p.color == 4294901760u);       // 0xFFFF0000 ARGB red
        assert(p.has_thickness && p.thickness == 3);
    }

    /* ---- defaults: absent label/closed/color/thickness ---- */
    {
        std::vector<OverlayPolygon> out;
        assert(parse_overlay_polygons("{\"polygons\":[{\"points\":[[0,0],[1,1]]}]}", &out) == true);
        assert(out.size() == 1);
        assert(out[0].label.empty());
        assert(out[0].closed == true);   // absent "closed" keeps the default
        assert(!out[0].has_color && !out[0].has_thickness);
        assert(out[0].points.size() == 2);
        assert(out[0].points[1].first == 1.0f && out[0].points[1].second == 1.0f);
    }

    /* ---- empty array clears the sidecar (returns true) ---- */
    {
        std::vector<OverlayPolygon> out;
        out.push_back({{{0.f, 0.f}, {1.f, 1.f}}, "old", true, 0, 0, false, false});
        assert(parse_overlay_polygons("{\"polygons\":[]}", &out) == true);
        assert(out.empty());
    }

    /* ---- malformed entries are skipped, valid ones survive ---- */
    {
        std::vector<OverlayPolygon> out;
        const char* payload =
            "{\"polygons\":["
            "{\"points\":[[0.5]]},"                       // 1 point  -> dropped
            "{\"label\":\"nopts\"},"                       // no points -> dropped
            "{\"points\":[[0,0],[0,1],[1,1]],\"label\":\"ok\","
            "\"closed\":true},"                            // valid
            "{\"points\":[[0,0]]},"                       // another 1-pointer
            "]}";
        assert(parse_overlay_polygons(payload, &out) == true);
        assert(out.size() == 1);
        assert(out[0].label == "ok" && out[0].closed == true);
    }

    /* ---- the array walk stops at ']' — later payload keys are not consumed ---- */
    {
        std::vector<OverlayPolygon> out;
        const char* payload =
            "{\"polygons\":[{\"points\":[[0,0],[1,1]]}],"
            "\"detections\":[{\"bbox\":[0,0,1,1],\"label\":\"person\"}],"
            "\"num_detections\":1}";
        assert(parse_overlay_polygons(payload, &out) == true);
        assert(out.size() == 1);
    }

    /* ---- cap: at most 16 polygons per payload ---- */
    {
        std::vector<OverlayPolygon> out;
        std::string payload = "{\"polygons\":[";
        for (int i = 0; i < 20; i++) payload += "{\"points\":[[0,0],[1,1]]},";
        payload += "]}";
        assert(parse_overlay_polygons(payload, &out) == true);
        assert(out.size() == 16);
    }

    /* ---- cap: at most HAL_MAX_POLYGON_POINTS points per polygon ---- */
    {
        std::vector<OverlayPolygon> out;
        std::string payload = "{\"polygons\":[{\"points\":[";
        for (int i = 0; i < HAL_MAX_POLYGON_POINTS + 10; i++) payload += "[0.5,0.5],";
        payload += "]}]}";
        assert(parse_overlay_polygons(payload, &out) == true);
        assert(out.size() == 1);
        assert(out[0].points.size() == (size_t)HAL_MAX_POLYGON_POINTS);
    }

    /* ---- compact contract: a space before the points '[' doesn't match the
       scanner, so such a polygon parses zero points and is dropped. The SDK
       always publishes compact JSON (compact=True), which is the contract. ---- */
    {
        std::vector<OverlayPolygon> out;
        assert(parse_overlay_polygons(
            "{\"polygons\":[{\"points\": [ [0.1 , 0.2] , [0.3, 0.4] ] }]}", &out) == true);
        assert(out.empty());   // points-less polygon dropped, parse still "ok"
    }

    /* ---- apply_track_id_label ---- */
    {
        char label[64];
        // appends " #<id>"
        snprintf(label, sizeof(label), "person");
        apply_track_id_label(label, sizeof(label), 7);
        assert(strcmp(label, "person #7") == 0);

        // negative id (untracked) is a no-op
        snprintf(label, sizeof(label), "car");
        apply_track_id_label(label, sizeof(label), -1);
        assert(strcmp(label, "car") == 0);

        // id 0 is still a track
        snprintf(label, sizeof(label), "dog");
        apply_track_id_label(label, sizeof(label), 0);
        assert(strcmp(label, "dog #0") == 0);

        // long label truncates from the right; the suffix always fits:
        // cap 12 -> body 12 - 1 NUL - 8 suffix chars = 3 label chars
        snprintf(label, sizeof(label), "traffic_light_westbound_avenue");
        apply_track_id_label(label, 12, 123456);
        assert(strcmp(label, "tra #123456") == 0);
        assert(strlen(label) == 11);

        // cap too small for the suffix at all -> untouched
        snprintf(label, sizeof(label), "person");
        apply_track_id_label(label, 3, 7);        // " #7" needs 4 bytes incl NUL
        assert(strcmp(label, "person") == 0);

        // guards
        apply_track_id_label(nullptr, 8, 1);
        snprintf(label, sizeof(label), "x");
        apply_track_id_label(label, 0, 1);
        assert(strcmp(label, "x") == 0);
    }

    /* ---- resolve_result_ttl_ms: the full chain ---- */
    {
        AiOverlayConfig cfg;
        cfg.result_ttl_ms = 250;
        cfg.stream_result_ttls["main"] = 120;
        cfg.stream_fps["main"] = 30;         // -> 67ms derived
        cfg.stream_fps["sub"] = 15;          // -> 133ms derived

        // 1. per-result ttl wins over everything
        assert(resolve_result_ttl_ms(80, "main", cfg) == 80);
        // 2. stream override beats global and fps
        assert(resolve_result_ttl_ms(0, "main", cfg) == 120);
        // 3. global beats fps-derived (no stream override for "sub")
        assert(resolve_result_ttl_ms(0, "sub", cfg) == 250);
        // 4. fps-derived: two frame periods, rounded to nearest ms
        //    (clear the "main" override first — stream beats fps in the chain)
        cfg.result_ttl_ms = 0;
        cfg.stream_result_ttls["main"] = 0;
        assert(resolve_result_ttl_ms(0, "main", cfg) == 67);   // 30fps
        assert(resolve_result_ttl_ms(0, "sub", cfg) == 133);   // 15fps
        // 5. unknown stream, nothing configured -> fallback
        assert(resolve_result_ttl_ms(0, "other", cfg) == 500);
        // zero-valued entries are skipped, not honored
        cfg.stream_result_ttls["main"] = 0;
        cfg.stream_fps["main"] = 0;
        assert(resolve_result_ttl_ms(0, "main", cfg) == 500);
        // very high fps floors at 1ms
        cfg.stream_fps["fast"] = 4000;
        assert(resolve_result_ttl_ms(0, "fast", cfg) == 1);
    }

    /* ---- parse_result_ttl_ms: wire metadata is never trusted ---- */
    {
        assert(parse_result_ttl_ms("50") == 50);
        assert(parse_result_ttl_ms("600000") == 600000);   // the cap itself
        assert(parse_result_ttl_ms("600001") == 0);        // above the cap
        assert(parse_result_ttl_ms("0") == 0);             // zero = unset
        assert(parse_result_ttl_ms("-5") == 0);            // pre-fix: wrapped to 4294967291
        assert(parse_result_ttl_ms("") == 0);
        assert(parse_result_ttl_ms("junk") == 0);
        assert(parse_result_ttl_ms("50x") == 0);           // trailing garbage
        assert(parse_result_ttl_ms("99999999999999999999") == 0);  // ERANGE-sized
    }

    /* ---- compose_draw_config: HAL defaults seeded, overrides applied ----
       Regression guard for the silent no-render bug: per-stream draw configs
       used to be zero-initialized, so cfg->draw_detections was false and the
       HAL unified draw skipped the whole detection branch. The composer must
       start from hal_draw_config_init_default values and only override the
       per-frame knobs. */
    {
        auto same_color = [](HalColor a, HalColor b) {
            return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
        };

        HalDrawConfig base{};
        hal_draw_config_init_default(&base);
        assert(base.draw_detections);          // the default that was being lost

        // overrides land, everything else rides through from base
        HalDrawConfig cfg = compose_draw_config(base, false, true, 4);
        assert(cfg.draw_detections);
        assert(!cfg.draw_detection_labels);
        assert(cfg.draw_detection_confidence);
        assert(cfg.default_box_thickness == 4);
        // base fields the composer must not touch
        assert(same_color(cfg.default_box_color, base.default_box_color));
        assert(same_color(cfg.default_text_color, base.default_text_color));
        assert(cfg.draw_keypoints == base.draw_keypoints);
        assert(cfg.draw_classifications == base.draw_classifications);
        assert(cfg.draw_ocr == base.draw_ocr);
        assert(cfg.segmentation_alpha == base.segmentation_alpha);
        assert(cfg.default_font_scale == base.default_font_scale);

        // all-on overrides keep the defaults on
        cfg = compose_draw_config(base, true, true, 2);
        assert(cfg.draw_detections && cfg.draw_detection_labels &&
               cfg.draw_detection_confidence && cfg.default_box_thickness == 2);

        // a zeroed base (degenerate caller) still yields a drawable config —
        // draw_detections must be forced on regardless of the base
        HalDrawConfig zeroed{};
        cfg = compose_draw_config(zeroed, true, true, 2);
        assert(cfg.draw_detections);
        assert(cfg.draw_detection_labels && cfg.draw_detection_confidence);
        assert(cfg.default_box_thickness == 2);

        // base is never mutated
        assert(base.draw_detection_labels && base.default_box_thickness != 4);
    }

    return 0;
}
