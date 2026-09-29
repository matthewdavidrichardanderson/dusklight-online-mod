#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dusklight_online::game::speedrun {

// Uses the native speedrun mode and card routines from the official host.
bool initialize();
bool mode_active();
bool timer_running();
bool local_finish_started();
enum class FinishReadiness { Ineligible, Busy, Ready };
FinishReadiness finish_readiness();
bool start_finish_sequence();
bool can_start_here();
bool reset_run();
bool start_run();
bool close_menus_after_start();

class SaveProbe {
public:
    enum class Result { Pending, Clear, Occupied, Error };

    void begin();
    Result tick();
    void clear();
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    enum class Phase { Idle, WaitingForCard, Loading, Done } phase_ = Phase::Idle;
    std::vector<uint8_t> bytes_;
    Result result_ = Result::Error;
    std::string error_;
};

} // namespace dusklight_online::game::speedrun
