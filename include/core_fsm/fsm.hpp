/***
 * Implements framework for event-driven finite-state machines for embedded.
 * Uses the following structure and semantics, aligned to UML state diagrams:
 * 
 * Producer:
 * - E.g. ISR, driver.
 * - Any part of the system that needs to communicate events.
 * 
 * Context (Ctx) aka machine:
 * - Is the machine.
 * - Owns state (both a key and the state obj.).
 * - Receives events via an event-queue (enqueued by producers), via post().
 * - step() consumes events from the queue and passes these to the current state for handling.
 * 
 * Event:
 * - E.g. Event::Timer.
 * - Triggers from timers, messages, button presses, etc.
 * 
 * Guard:
 * - Boolean check that runs before the state gets the event.
 * - Separates out logic that cuts across many states.
 * 
 * Action:
 * - Side-effect that is executed before entering the state.
 * 
 * State:
 * - Parses incoming event, and assesses if OK to continue to next state, or not.
 * - Can execute actions for the event lifetime.
 *      - Run hooks on_enter (when state set) and on_exit (right before next state is set) during transitions. Setup and teardown.
 *      - Can execute side-effects. But preferred to keep such in actions, if possible.
 * - A state is not aware of other states, it only emits an OutcomeT signal.
 * 
 * Key:
 * - E.g. State::Green.
 * - Lookup key in the transition table, corresponds to an object.
 * - Translated via Context::make_state(key).
 * 
 * OutcomeT:
 * - E.g. OK, IGNORE, FAIL
 * - Signal from a state, communicates if state transition can occur.
 * 
 * Transition table:
 * - Constexpr specification of the state machine's transitions.
 * - Defines combinations of states, events and outcomes leading to next-states.
 * - Specifies guards and actions.
 * 
 * Queue adapter:
 * - Events are queueed for reentrancy.
 * - Plumbing to use e.g. STL queues, FreeRTOS queues, etc.
 * - Adapters for different queue types, so we can use FreeRTOS queues or STL, etc.
 * 
 * Janus, July 2025
 */
#pragma once
#include <cstdint>
#include <memory>
#include <concepts>
#include <queue>
#include <span>

namespace core_fsm {

    // --- OutcomeT: signal from a state to the context/dispatcher ---
    enum class OutcomeT : uint8_t { 
        OK,         // Transition may proceed (subject to guard)
        IGNORE,     // No transition; event consumed or intentionally ignored
        FAIL        // Error/failed handling; dispatcher may map to Fault state
    };

    // --- Transition table row ---
    template<class State, class Event, class Ctx>
    struct TransitionEntryT {
        // --- Helpers ---
        using GuardFn = bool (*)(const Ctx&);   // guard will check some extended state
        using ActionFn = void (*)(Ctx&);        // action may mutate contex

        // --- Transition table record ---
        State       from;
        Event       event;
        OutcomeT    outcome;
        GuardFn     guard;
        ActionFn    action;
        State       next;
    };

    // --- Construct a transition table with N rows ---
    template<class State, class Event, class Ctx, std::size_t N>
    using TransitionTableT = std::array<TransitionEntryT<State, Event, Ctx>, N>;

    // --- General guard functions ---
    template<class Ctx> bool g_always(const Ctx&) { return true; }
    template<class Ctx> bool g_never (const Ctx&) { return false; }

    // --- General actions ---
    template<class Ctx> void a_nothing(Ctx&) { return; }

    // --- Event Queue requirements - implementable with STL, FreeRTOS, etc ---
    template<class Q, class Event>
    concept EventQueue = requires(Q q, const Event& ev_in, Event& ev_out) {
        { q.try_push(ev_in)     } -> std::same_as<bool>; //
        { q.pop(ev_out)         } -> std::same_as<bool>;
        { q.empty()             } -> std::same_as<bool>;
    };

    // --- Adapter for STL queue, implement similar for FreeRTOS in the hardware component ---
    template<class Event, std::size_t CAP = 128>
    struct StdQueueAdapter {
        bool try_push(const Event& ev_in) {
            if (q_.size() >= CAP) return false;    // drops the newest
            q_.push(ev_in);
            return true;
        }

        bool pop(Event& ev_out) {
            if (q_.empty()) return false;
            ev_out = q_.front();                    // gets the oldest
            q_.pop();
            return true;
        }

        bool empty() const {
            return q_.empty();
        }

    private:
        std::queue<Event> q_;
    };

    // --- State: Interface to design state objects against ---
    template<class Ctx, class Event>
    struct StateT {
        virtual ~StateT() = default;
        virtual OutcomeT on_event(Ctx& ctx, const Event& ev) = 0;
        virtual void on_enter(Ctx& ctx) noexcept { (void)ctx; }
        virtual void on_exit (Ctx& ctx) noexcept { (void)ctx; }
    };

    // --- ContextBaseT: CRTP base to build/inherit contexts from ---
    template <class DerivedCtx,                                     // Actual context (machine)
              class StateKey,                                       // State names (enum class)
              class Event,                                          // Event type (enum class)
              class Queue,                                          // 
              class State = core_fsm::StateT<DerivedCtx, Event>>    // our kind of templated state
    requires core_fsm::EventQueue<Queue, Event>                     // predicate on template arguments
    struct ContextBaseT {
    public:
        using StatePtr = std::unique_ptr<State>;
        using TransitionEntry = TransitionEntryT<StateKey, Event, DerivedCtx>;
        
        ContextBaseT() = default;
        ~ContextBaseT() = default;

        // --- Setup-API for users to attach an N-length transition table ---
        template<std::size_t N>
        void set_table(const TransitionTableT<StateKey, Event, DerivedCtx, N>& table) {
            table_ = std::span{table};      // deduces pointer and size
        }

        // --- Setup-API for users to initialize the machine to a particular state ---
        void set_initial_state(StateKey key) {
            if (is_initialized_) return;
            set_state(key);
            is_initialized_ = true;
        }

        // --- Runtime-API for producers to send events ---
        bool post(const Event& event) { return queue_.try_push(event); }

        // --- Runtime-API for users to step the machine (consume from queue) ---
        void step() {
            if (!state_) return;
            Event event;
            OutcomeT outcome = OutcomeT::IGNORE;

            if (queue_.pop(event)) {
                outcome = state_->on_event(self(), event);     // state obj handles the event
            }

            dispatch(event, outcome);
        }

        // --- Runtime-API for users to Inspect state ---
        StateKey get_state_key(void) const noexcept {
            return state_key_;
        }

    protected:
        // --- Internal-API: Update state, and trigger hooks ---
        void set_state(StateKey key) {
            if (state_) state_->on_exit(self());        // Tear down
            state_key_ = key;
            state_ = std::move(self().make_state(key)); // Next state
            if (state_) state_->on_enter(self());       // Set up
        }

        // --- Internal: Update state via event+transition ---
        void dispatch(const Event& event, OutcomeT& outcome) {
            if (outcome == OutcomeT::IGNORE) return;    // Explicitly ignore

            if (auto transition = lookup_transition(state_key_, event, outcome)) {
                
                // Stop if guard doesn't pass
                if (transition->guard) {
                    if (!transition->guard(self())) return;
                }

                // Execute side-effect
                if (transition->action) transition->action(self());

                // Ask derived (actual) context for a state obj. from the state key, and set that as next
                set_state(transition->next);
            }
        }

    private:
        // --- Lookup transition by (StateKey, Event, OutcomeT) in the table, and return pointer to the relevant row ---
        const TransitionEntry* lookup_transition(StateKey from, const Event& event, const OutcomeT& outcome) const {
            for (const auto& row : table_) {
                if (row.from == from && row.event == event && row.outcome == outcome) {
                    return &row;
                }
            }
            return nullptr;
        }

        // --- Refererence to DerivedCtx by CRTP idiom ---
        DerivedCtx& self() noexcept { return static_cast<DerivedCtx&>(*this); }
        const DerivedCtx& self() const noexcept { return static_cast<const DerivedCtx&>(*this); }

        // --- Owning queue and state ---
        Queue queue_{};
        StateKey state_key_ {};             // state name
        std::unique_ptr<State> state_{};   // state object

        // --- Non-owning a transition table
        std::span<const TransitionEntry> table_ {};

        // --- One-time initialization ---
        bool is_initialized_ {false};
    }; // ContextBaseT

} // core_fsm