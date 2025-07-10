/**
 * Tests instatiation and guarding of adapters
 */

#include <gtest/gtest.h>
#include "core_fsm/fsm.hpp"

namespace {

// --- Enumerate states and events for this machine ---
enum class MStateKey : std::uint8_t { On, Off };
enum class MEvent : std::uint8_t { Button };

// --- Prototype the machine ---
constexpr std::size_t n_states {2};
constexpr std::size_t q_cap {32};

struct MCtx;
using Factory = core_fsm::StlQueueMachineBuilder<MCtx, MStateKey, MEvent, n_states, q_cap>;

using Outcome = Factory::Outcome;
constexpr Factory::TransitionTable<2> transitions {{
//  From State              On Event        Given Outcome   With Guard              Do Action         To Next State
    {MStateKey::On,         MEvent::Button, Outcome::OK,    Factory::g_always,      nullptr,          MStateKey::Off },
    {MStateKey::Off,        MEvent::Button, Outcome::OK,    Factory::g_always,      nullptr,          MStateKey::On  },
}};

struct On final : Factory::State {
    void on_enter(MCtx&) noexcept override { return; }
    void on_exit (MCtx&) noexcept override { return; }
    Factory::Outcome on_event(MCtx&, const MEvent&) override  { return Factory::Outcome::OK; }
};

struct Off final : Factory::State {
    void on_enter(MCtx&) noexcept override { return; }
    void on_exit (MCtx&) noexcept override { return; }
    Factory::Outcome on_event(MCtx&, const MEvent&) override  { return Factory::Outcome::OK; }
};

// --- Extend the Machine context ---
struct MCtx final : Factory::BaseCtx {
    MCtx() {
        register_state(MStateKey::On,    on_);
        register_state(MStateKey::Off,   off_);
        set_table(transitions);
        const auto ok = initialize(MStateKey::On);
        if (!ok) std::terminate();
    }

    On   on_ {};
    Off  off_ {};
};

}
// --- Google Test fixture ---
class AdapterStateTest : public ::testing::Test {
protected:
    MCtx machine{};
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(AdapterStateTest, AdapterInitialized) {
    EXPECT_TRUE(true);
}
