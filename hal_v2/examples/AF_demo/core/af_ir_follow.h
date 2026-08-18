#ifndef HAL_AUTO_AF_IR_FOLLOW_H
#define HAL_AUTO_AF_IR_FOLLOW_H

#include <string>
#include <vector>

namespace hal_auto_af
{

struct IrPwm
{
    int near_pwm = 0;
    int far_pwm = 0;
};

struct IrLutPoint
{
    double zoom_ratio = 1.0;
    int near_pwm = 0;
    int far_pwm = 0;
};

struct IrFollowConfig
{
    bool auto_follow = true;
    int deadband = 2;
    int settle_frames = 3;
    bool log_enabled = true;
};

struct IrFollowState
{
    IrFollowConfig config{};
    std::vector<IrLutPoint> lut;
    std::string lut_source = "builtin";
    bool night_mode = false;
    bool follow_active = false;
    bool manual_valid = false;
    IrPwm manual{};
    bool applied_valid = false;
    IrPwm applied{};
    bool degraded = false;
};

std::vector<IrLutPoint> default_ir_zoom_lut();

bool validate_ir_zoom_lut(const std::vector<IrLutPoint> &lut, std::string *error,
                          bool *monotonic_warning = nullptr);

IrPwm interpolate_ir_zoom_lut(const std::vector<IrLutPoint> &lut, double zoom_ratio);

bool upsert_ir_zoom_lut_point(std::vector<IrLutPoint> *lut, const IrLutPoint &point,
                              std::string *error);

bool load_ir_zoom_lut_csv(const std::string &path, std::vector<IrLutPoint> *lut,
                          std::string *error, bool *monotonic_warning = nullptr);

bool save_ir_zoom_lut_csv(const std::string &path, const std::vector<IrLutPoint> &lut,
                          std::string *error);

IrPwm desired_ir_pwm(const IrFollowState &state, double zoom_ratio);

bool ir_pwm_channel_needs_update(int current_pwm, int target_pwm, int deadband);

void set_ir_manual(IrFollowState *state, IrPwm pwm);
void clear_ir_manual(IrFollowState *state);
void begin_ir_follow(IrFollowState *state);
void end_ir_follow(IrFollowState *state);

} // namespace hal_auto_af

#endif
