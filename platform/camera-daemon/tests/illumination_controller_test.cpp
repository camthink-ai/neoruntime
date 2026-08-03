#include "illumination_controller.h"

#include <cassert>
#include <map>
#include <string>

int main() {
    std::map<uint32_t, uint32_t> outputs;
    IlluminationConfig config;
    config.lut_path.clear();
    config.log_updates = false;
    IlluminationController controller(
        config, [&](uint32_t id, uint32_t duty) {
            outputs[id] = duty;
            return true;
        });

    std::string warning;
    assert(controller.initialize(&warning));
    assert(controller.set_mode(ImagingMode::Day, 1.0));
    assert(outputs[0] == 0 && outputs[1] == 0);

    assert(controller.set_mode(ImagingMode::Infrared, 1.5));
    assert(outputs[0] == 80 && outputs[1] == 0);
    assert(controller.status().source == InfraredOutputSource::Automatic);

    assert(controller.set_manual_pwm(40, 20, 1.5));
    assert(outputs[0] == 40 && outputs[1] == 20);
    assert(controller.status().source == InfraredOutputSource::Manual);

    assert(controller.begin_zoom_follow(2.5));
    assert(outputs[0] == 32 && outputs[1] == 50);
    assert(controller.status().source == InfraredOutputSource::ZoomFollow);

    assert(controller.apply_endpoint_ratio(2.88));
    assert(outputs[0] == 0 && outputs[1] == 100);
    assert(controller.end_zoom_follow(2.88));
    assert(outputs[0] == 40 && outputs[1] == 20);
    assert(controller.status().source == InfraredOutputSource::Manual);

    assert(controller.clear_manual(2.0));
    assert(outputs[0] == 75 && outputs[1] == 10);
    assert(controller.set_mode(ImagingMode::Day, 2.0));
    assert(outputs[0] == 0 && outputs[1] == 0);
    return 0;
}
