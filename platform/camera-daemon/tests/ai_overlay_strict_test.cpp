// Unit tests for the strict frame-lock helpers (P1-6):
//   strict_gate_decision()      — pure sequence comparison → gate action
//   resolve_strict_wait_cap_ms() — configured > fps-derived (2 frame periods)
//                                  > 66ms fallback, all clamped to [1,500]
//   strict_cross_fed()          — display stream fed by a foreign infer stream
//                                  (shared HAL counter ticks once per frontend
//                                  callback, so cross-stream sequences can
//                                  never match — strict lock is not offered)
//   parse_frame_sequence()      — wire payload side (ai-runtime results carry
//                                  "frame_sequence":N on every event)
#include "ai_overlay_subscriber.h"

#include <cassert>
#include <string>

int main() {
    /* ---- strict_gate_decision: sequence comparison only, no timing ---- */
    {
        // no result for the stream at all → nothing to gate on
        assert(strict_gate_decision(0, false, 100) == StrictGateDecision::SKIP);
        // exact frame sequence → the frame locks its own result
        assert(strict_gate_decision(100, true, 100) == StrictGateDecision::LOCK_FRESH);
        // stored result is NEWER than the gating frame (wait overtook) → draw it
        assert(strict_gate_decision(105, true, 100) == StrictGateDecision::LOCK_NEWEST);
        // stored result is OLDER → keep waiting; the caller bounds the wait
        // (cap expiry is the caller's DEGRADE, the pure fn never times out)
        assert(strict_gate_decision(98, true, 100) == StrictGateDecision::WAIT);
        assert(strict_gate_decision(0, true, 1) == StrictGateDecision::WAIT);
    }

    /* ---- resolve_strict_wait_cap_ms: configured > fps-derived > fallback,
     *      always clamped to [1,500] (the wait runs on the streaming thread) */
    {
        AiOverlayConfig cfg;
        cfg.stream_fps["main"] = 30;

        // explicit configuration wins, clamped at both ends
        assert(resolve_strict_wait_cap_ms(80, "main", cfg) == 80);
        assert(resolve_strict_wait_cap_ms(1, "main", cfg) == 1);
        assert(resolve_strict_wait_cap_ms(5000, "main", cfg) == 500);

        // unconfigured → two frame periods of the display stream (30fps → 67ms,
        // same formula as the TTL chain)
        assert(resolve_strict_wait_cap_ms(0, "main", cfg) == 67);
        cfg.stream_fps["slow"] = 15;
        assert(resolve_strict_wait_cap_ms(0, "slow", cfg) == 133);

        // fps high enough to round to 0 must floor at 1ms, not stall-free 0
        cfg.stream_fps["fast"] = 3000;
        assert(resolve_strict_wait_cap_ms(0, "fast", cfg) == 1);

        // unknown stream → fallback (66ms ≈ 2 frame periods @30fps)
        assert(resolve_strict_wait_cap_ms(0, "mystery", cfg) == 66);
        // empty config, same fallback
        assert(resolve_strict_wait_cap_ms(0, "main", AiOverlayConfig{}) == 66);
    }

    /* ---- strict_cross_fed: a foreign infer stream targets the display ---- */
    {
        AiOverlayConfig cfg;
        // auto-generated map shape: every stream → primary encoder
        cfg.stream_map["main"] = "main";
        cfg.stream_map["sub"] = "main";
        cfg.stream_map["third"] = "main";
        // "main" is targeted by sub/third (infer != display) → cross-fed
        assert(strict_cross_fed("main", cfg) == true);
        // nothing foreign targets "sub" → its own results (identity) can gate
        assert(strict_cross_fed("sub", cfg) == false);

        // explicit third → main only
        AiOverlayConfig cfg2;
        cfg2.stream_map["third"] = "main";
        assert(strict_cross_fed("main", cfg2) == true);
        assert(strict_cross_fed("third", cfg2) == false);

        // empty map: no cross-feeding declared
        assert(strict_cross_fed("main", AiOverlayConfig{}) == false);
    }

    /* ---- parse_frame_sequence: wire side ---- */
    {
        uint64_t seq = 0;
        assert(parse_frame_sequence(
                   "{\"stream_id\":\"main\",\"frame_sequence\":42,\"timestamp_ns\":123}", &seq) == true);
        assert(seq == 42);
        // absent key → false, out untouched
        seq = 7;
        assert(parse_frame_sequence("{\"stream_id\":\"main\",\"num_detections\":0}", &seq) == false);
        assert(seq == 7);
        // non-numeric value → false
        assert(parse_frame_sequence("{\"frame_sequence\":\"x\"}", &seq) == false);
        // large values (counter overflow of uint32 frame seq aside, parse is u64)
        assert(parse_frame_sequence("{\"frame_sequence\":4294967295}", &seq) == true);
        assert(seq == 4294967295ull);
    }

    return 0;
}
