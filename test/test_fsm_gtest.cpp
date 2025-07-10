/***
 * Tests/shows using a transition table to manage transitions.
 * 
    @startuml
    state LedColor {
        [*] --> Green
        Green --> Yellow : ev_timer
        Yellow --> Red: ev_timer
        Red --> Green: ev_timer
    }
    @enduml
 * 
 * Janus, July 2025
 */

#include <gtest/gtest.h>
#include "core_fsm/fsm.hpp"

namespace {
// --- Enumerate states and events for this machine ---
enum class MStateKey : std::uint8_t { Green, Yellow, Red };
enum class MEvent : std::uint8_t { Timer, Button, Missing };
constexpr std::size_t n_states {3};
constexpr std::size_t n_queue_len {32};

// --- Prototype the machine with 3 states ---
struct MCtx;
using Factory = core_fsm::StlQueueMachineBuilder<MCtx, MStateKey, MEvent, n_states, n_queue_len>;

// --- Transitions (table must outlive MCtx and BaseCtx) ---
using Outcome = Factory::Outcome;
inline constexpr void a_side_effect(MCtx&);
constexpr Factory::TransitionTable<3> transitions {{
//  From State              On Event        Given Outcome   With Guard              Do Action               To Next State
    {MStateKey::Green,      MEvent::Timer,  Outcome::OK,    Factory::g_always,      a_side_effect,          MStateKey::Yellow  },
    {MStateKey::Yellow,     MEvent::Timer,  Outcome::OK,    nullptr,                nullptr,                MStateKey::Red     },
    {MStateKey::Red,        MEvent::Timer,  Outcome::OK,    Factory::g_never,       Factory::a_nothing,     MStateKey::Green   },
}};

// --- The LedColor states all implement the the same handling, so let's share this ---
struct LedColorState : Factory::State {
    void on_enter(MCtx& ctx) noexcept override;
    void on_exit (MCtx& ctx) noexcept override;
    Factory::Outcome on_event(MCtx& ctx, const MEvent& event) override;
};

// --- Final 3 LED color state objects ---
struct GreenLedColor final : LedColorState {};
struct YellowLedColor final : LedColorState {};
struct RedLedColor final : LedColorState {};

// --- Extend the Machine context ---
struct MCtx final : Factory::BaseCtx {

    // Set up available states, transitions and default state
    MCtx() {
        register_state(MStateKey::Green,    green_);
        register_state(MStateKey::Yellow,   yellow_);
        register_state(MStateKey::Red,      red_);
        set_table(transitions);
        const auto ok = initialize(MStateKey::Green);
        if (!ok) std::terminate();
    }

    // --- Extended state variables for testing ---
    int enters{0};
    int exits{0};
    int handled{0};
    int side_effects{0};
    MStateKey last_enter_seen_state{MStateKey::Green};
    MStateKey last_exit_seen_state{MStateKey::Green};

    // --- State obj lifetime owned here: Objects must live as long as BaseCtx ---
    GreenLedColor   green_ {};
    YellowLedColor  yellow_ {};
    RedLedColor     red_ {};
};

// --- Action function bodies ---
inline constexpr void a_side_effect(MCtx& ctx) { ctx.side_effects++; }

// --- State method bodies ---
inline void LedColorState::on_enter(MCtx& ctx) noexcept { ++ctx.enters; ctx.last_enter_seen_state = ctx.get_state_key(); }
inline void LedColorState::on_exit (MCtx& ctx) noexcept { ++ctx.exits; ctx.last_exit_seen_state = ctx.get_state_key(); }
inline auto LedColorState::on_event(MCtx& ctx, const MEvent& ev) -> Factory::Outcome {
    ++ctx.handled;
    return ev == MEvent::Timer ? Factory::Outcome::OK : Factory::Outcome::IGNORE;
}


// --- Google Test fixture ---
class ColorStateTest : public ::testing::Test {
protected:
    MCtx machine{};
    void SetUp() override {}
    void TearDown() override {}
};
}

// --- Tests: Now we focus on the context being able to dispatch correctly (in the prev. example we tested that the State reacts correctly) ---
TEST_F(ColorStateTest, EntryHooksRunInSetup) {
    EXPECT_EQ(machine.enters, 1);
    EXPECT_EQ(machine.exits, 0);    // hook has not run yet
}

TEST_F(ColorStateTest, InitializerCanOnlyRunOnce) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    const auto ok = machine.initialize(MStateKey::Red);
    EXPECT_FALSE(ok);
    EXPECT_EQ(machine.get_state_key(), MStateKey::Green);       // Stays in default state
}

TEST_F(ColorStateTest, MissingEventHandledGracefully) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    (void) machine.post(MEvent::Missing);
    machine.step();
    EXPECT_EQ(machine.get_state_key(), MStateKey::Green);       // Stays in default state
}

TEST_F(ColorStateTest, ColorStateAdvancesOnTimer) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    (void) machine.post(MEvent::Timer);
    machine.step();
    EXPECT_EQ(machine.get_state_key(), MStateKey::Yellow);      // Advanced one step to yellow
}

TEST_F(ColorStateTest, NullPtrGuardAndActionPassGracefully) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    (void) machine.post(MEvent::Timer);                         // -> Yellow
    (void) machine.post(MEvent::Timer);                         // -> Red: guard = nullptr, action = nullptr
    machine.step();
    machine.step();
    EXPECT_EQ(machine.get_state_key(), MStateKey::Red);         // Advanced gracefully to Red
}

TEST_F(ColorStateTest, NeverGuardBlocksTransition) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    (void) machine.post(MEvent::Timer);                         // -> Yellow
    (void) machine.post(MEvent::Timer);                         // -> Red
    (void) machine.post(MEvent::Timer);                         // -> Green [blocked by guard]
    machine.step();
    machine.step();
    machine.step();
    EXPECT_EQ(machine.get_state_key(), MStateKey::Red);
}

TEST_F(ColorStateTest, SideEffectIsRun) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    ASSERT_EQ(machine.side_effects, 0);                         // Default state as per object setup
    (void) machine.post(MEvent::Timer);
    machine.step();
    ASSERT_EQ(machine.get_state_key(), MStateKey::Yellow);
    EXPECT_EQ(machine.side_effects, 1);                         // Side effect ran
}

TEST_F(ColorStateTest, EnterHookSeesCommittedState) {
    // --- Enfore that on_enter sees the right context state ---
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);           // Start Green
    (void) machine.post(MEvent::Timer);                             // Timer event
    machine.step();                                                 // React, and go to Yellow
    EXPECT_EQ(machine.last_enter_seen_state, MStateKey::Yellow);    // On enter, Yellow must be committed
}

TEST_F(ColorStateTest, ExitHookSeesCommittedState) {
    // --- Enfore that on_enter sees the right context state ---
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);
    (void) machine.post(MEvent::Timer);
    (void) machine.post(MEvent::Timer);
    machine.step();
    machine.step();
    ASSERT_EQ(machine.get_state_key(), MStateKey::Red);
    EXPECT_EQ(machine.last_exit_seen_state, MStateKey::Yellow);
}