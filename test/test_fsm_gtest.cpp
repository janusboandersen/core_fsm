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
 * Janus, August 2025
 */

#include <gtest/gtest.h>
#include "core_fsm/fsm.hpp"

// --- Scoping ---
using core_fsm::OutcomeT;


// --- Forward declare types, guards, actions for this machine ---
struct MCtx;                                                            // The Machine itself
enum class MStateKey : std::uint8_t;                                    // State names like Green, Yellow, Red
enum class MEvent : std::uint8_t;                                       // Events like Timer, Button
using MStateT = core_fsm::StateT<MCtx, MEvent>;                         // State objects corresponding the names Green, Yellow, Red
using MEventQueueT = core_fsm::StdQueueAdapter<MEvent, 32>;
using MTransitionTable = core_fsm::TransitionTableT<MStateKey, MEvent, MCtx, 3>;
inline constexpr auto g_always = core_fsm::g_always<MCtx>;
inline constexpr auto g_never = core_fsm::g_never<MCtx>;
inline constexpr auto a_nothing = core_fsm::a_nothing<MCtx>;
inline constexpr void a_side_effect(MCtx&);

// --- Enumerate states for this machine ---
enum class MStateKey : std::uint8_t { Green, Yellow, Red };

// --- Implement events for this machine ---
enum class MEvent : std::uint8_t { Timer, Button, Missing };

// --- Table for transitions ---
constexpr MTransitionTable machine_transitions {{
//  From State              Event           Outcome         Guard       Action          Next State
    {MStateKey::Green,      MEvent::Timer,  OutcomeT::OK,   g_always,   a_side_effect,  MStateKey::Yellow  },
    {MStateKey::Yellow,     MEvent::Timer,  OutcomeT::OK,   nullptr,    nullptr,        MStateKey::Red     },
    {MStateKey::Red,        MEvent::Timer,  OutcomeT::OK,   g_never,    a_nothing,      MStateKey::Green   },
}};

// --- Extend the context ---
struct MCtx final : core_fsm::ContextBaseT<MCtx, MStateKey, MEvent, MEventQueueT>{
public:
    // --- Extended state ---
    int enters{0};
    int exits{0};
    int handled{0};

    // --- Side effects ---
    int side_effects{0};

    // --- Translate state key to final state obj ---
    std::unique_ptr<MStateT> make_state(const MStateKey& key);
};

// --- The LedColor states all implement the the same handling, so let's share this ---
struct LedColorState : MStateT {
    void on_enter(MCtx& ctx) noexcept override { ++ctx.enters; }
    void on_exit(MCtx& ctx) noexcept override { ++ctx.exits; }

    OutcomeT on_event(MCtx& ctx, const MEvent& ev) override {
        ++ctx.handled;

        // Here we implement as switch before we build the dispatcher, and without the guard
        switch (ev) {
            case MEvent::Timer:
                return OutcomeT::OK;
            default:
                return OutcomeT::IGNORE;
        }
    }
};

// --- Final states ---
struct GreenLedColor final : LedColorState {};
struct YellowLedColor final : LedColorState {};
struct RedLedColor final : LedColorState {};

// --- Translation between keys and final states ---
std::unique_ptr<MStateT> MCtx::make_state(const MStateKey& key) {
    switch (key) {
        case MStateKey::Green:
            return std::make_unique<GreenLedColor>();
        case MStateKey::Yellow:
            return std::make_unique<YellowLedColor>();
        case MStateKey::Red:
            return std::make_unique<RedLedColor>();
    }
}

// --- Actions ---
inline constexpr void a_side_effect(MCtx& ctx) {
    ctx.side_effects++;
}


// --- Google Test fixture ---
class ColorStateTest : public ::testing::Test {
protected:
    MCtx machine{};

    void SetUp() override {
        machine.set_initial_state(MStateKey::Green);   // default state
        machine.set_table(machine_transitions);
    }

    void TearDown() override {}
};


// --- Tests: Now we focus on the context being able to dispatch correctly (in the prev. example we tested that the State reacts correctly) ---
TEST_F(ColorStateTest, EntryHooksRunInSetup) {
    EXPECT_EQ(machine.enters, 1);
    EXPECT_EQ(machine.exits, 0);    // hook has not run yet
}

TEST_F(ColorStateTest, InitializerCanOnlyRunOnce) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    machine.set_initial_state(MStateKey::Red);
    EXPECT_EQ(machine.get_state_key(), MStateKey::Green);       // Stays in default state
}

TEST_F(ColorStateTest, MissingEventHandledGracefully) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    machine.post(MEvent::Missing);
    machine.step();
    EXPECT_EQ(machine.get_state_key(), MStateKey::Green);       // Stays in default state
}

TEST_F(ColorStateTest, ColorStateAdvancesOnTimer) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    machine.post(MEvent::Timer);
    machine.step();
    EXPECT_EQ(machine.get_state_key(), MStateKey::Yellow);      // Advanced one step to yellow
}

TEST_F(ColorStateTest, NullPtrGuardAndActionPassGracefully) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    machine.post(MEvent::Timer);                                // -> Yellow
    machine.post(MEvent::Timer);                                // -> Red: guard = nullptr, action = nullptr
    machine.step();
    machine.step();
    EXPECT_EQ(machine.get_state_key(), MStateKey::Red);         // Advanced gracefully to Red
}

TEST_F(ColorStateTest, NeverGuardBlocksTransition) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    machine.post(MEvent::Timer);                                // -> Yellow
    machine.post(MEvent::Timer);                                // -> Red
    machine.post(MEvent::Timer);                                // -> Green [blocked by guard]
    machine.step();
    machine.step();
    machine.step();
    EXPECT_EQ(machine.get_state_key(), MStateKey::Red);
}

TEST_F(ColorStateTest, SideEffectIsRun) {
    ASSERT_EQ(machine.get_state_key(), MStateKey::Green);       // Default state as per object setup
    ASSERT_EQ(machine.side_effects, 0);                         // Default state as per object setup
    machine.post(MEvent::Timer);
    machine.step();
    ASSERT_EQ(machine.get_state_key(), MStateKey::Yellow);
    EXPECT_EQ(machine.side_effects, 1);                         // Side effect ran
}