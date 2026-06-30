// Headless display for Linux/Jetson builds (no window). The error signal is
// still printed by main() and can be sent over UDP/gimbal by a real output.

#include "display.hpp"

namespace dd {
namespace {
class NullDisplay : public IDisplay {
public:
    bool isOpen() const override { return true; }
    void present(const Frame&, const Overlay&) override {}
};
} // namespace

std::unique_ptr<IDisplay> CreateDisplay(bool /*headless*/) {
    return std::make_unique<NullDisplay>();
}

} // namespace dd
