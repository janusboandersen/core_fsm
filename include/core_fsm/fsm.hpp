/***
 * Implements framework for event-driven finite-state machines for embedded.
 * Uses the following structure and semantics, aligned to UML state diagrams:
 * 
 * To get Run-to-completion semantics: 
 *  - Implement with one machine per task calling step().
 *  - Regular producers send events via post(). On embedded, any ISRs send events via post_from_isr().
 *  - FreeRTOSQueueAdapter is used for embedded (static queue), and the FSM will block on the queue, waiting for events, i.e. no busy-polling.
 *  - StlQueueAdapter is used on host machines (for tests, etc.), and the FSM does not block.
 * 
 * State changge semantics are like typical UML statecharts: OK-action-exit-switch-enter:
 *    Event -> state(Event) -> OK -> transition guard -> OK -> exit(state)|pre-transition-context -> transition action -> enter(next)|post-transition-context -> commit.
 * 
 * Producer:
 * - E.g. ISR, driver.
 * - Any part of the system that needs to communicate events.
 * 
 * Context (Ctx) aka machine:
 * - Is the machine.
 * - Receives events via an event-queue (enqueued by producers), via post().
 * - step() consumes event from the queue and passes this to the current state for handling.
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
 * Outcome:
 * - E.g. OK, IGNORE, FAIL
 * - Signal from a state, communicates if state transition can occur.
 * 
 * Transition table:
 * - Constexpr specification of the state machine's transitions.
 * - Defines combinations of states, events and outcomes leading to next-states.
 * - Specifies guards and actions.
 * 
 * Queue adapter:
 * - Adapters for different queue types, so we can use FreeRTOS queues or STL, etc.
 * - E.g. on FreeRTOS, queues allow for reentrancy.
 * 
 * TODO:
 * 
 * .	API/ergonomics

	•	Add template<size_t CAP> to StlQueueMachineBuilder/FreeRTOSQueueMachineBuilder.
	•	Add a policy template (self-transition reentry, FAIL mapping, lookup strategy).
	•	Introduce a light “policy layer” (traits) for: self-transition behavior, FAIL behavior, and possibly lookup strategy.
    •	Add optional trace callbacks (on_transition(from,event,outcome,to)), compiled out when not used.
	•	Consider a static, table-driven variant for MCUs that forbids virtuals.
	•	Add template<size_t CAP> parameter to StlQueueMachineBuilder/FreeRTOSQueueMachineBuilder.
	•	Add a configurable policy for self-transition hook behavior (reenter vs not).
	•	Provide a default_transition(StateKey) or on_fail(Event) handler at the context level.

	2.	Correctness

	•	consteval validator ensuring unique (from,event,outcome) keys.
    •	Provide an opt-in compile-time validator for the transition table (e.g., consteval uniqueness check).
	•	Offer unique_ptr<State> registration overloads or at least assert that states_[idx] is not re-registered.
	•	Optional fault_state or on_fail(Event) callback.
    •	Add an optional on_fail(Event) or fault_state mapping.
Lifetime hazards by design. register_state stores raw State*; set_table stores a std::span. You document the lifetime requirement but there’s no compile-time guard. A user can still dangle. Source: fsm.hpp (register_state, set_table).
Outcome::FAIL is a sink. Returning false from dispatch() provides no recovery path or fault transition; users must bolt on their own error path. Source: fsm.hpp.
No duplicate transition detection. Two rows with the same (from,event,outcome) silently pick the first. Easy to misconfigure. Source: fsm.hpp (lookup_transition).
	•	Remove duplicated deleted assignment operator overloads.
	•	Offer an optional precomputed transition index (e.g., constexpr 2D table [State][Event] → row index) when StateKey and Event are small enums.

	3.	Performance

	•	Optional pre-indexed transition table ([State][Event] → row*), generated at compile time for enum keys.

	4.	Robustness

	•	Remove redundant operator= deletions; keep only the canonical four special members.
	•	Guard register_state against double registration; optionally accept ownership.

	5.	Testing

	•	Add negative tests (duplicates, invalid keys), explicit self-transition tests, and FAIL path tests.
	•	Mock FreeRTOS ISR yield path; verify blocking pop() behavior by design (document when step() is expected to block).
	•	Add a compile-time table validator and test it (ensures uniqueness).

 * 
 * 
 * 
 * Janus, July 2025
 */
#pragma once
#include <cstdint>
#include <concepts>
#include <array>
#include <span>
#include "adapters.hpp"

namespace core_fsm {

    // --- OutcomeT: signal from a state to the context/dispatcher ---
    enum class Outcome : uint8_t { 
        OK,         // Transition may proceed (subject to guard)
        IGNORE,     // No transition; event consumed or intentionally ignored
        FAIL        // Error/failed handling; dispatcher may map to Fault state
    };

    // --- General guard functions ---
    template<class DCtx> bool g_always(const DCtx&) { return true; }
    template<class DCtx> bool g_never (const DCtx&) { return false; }

    // --- General actions ---
    template<class DCtx> void a_nothing(DCtx&) { return; }

    // --- Transition table row ---
    template<class StateKey, class Event, class DCtx>
    struct TransitionEntryT {
        // --- Helpers ---
        using Outcome = core_fsm::Outcome;
        using GuardFn = bool (*)(const DCtx&);  // Guard could check some extended state variables
        using ActionFn = void (*)(DCtx&);       // Action may mutate contex

        // --- Transition table record ---
        StateKey    from;                       // Move from state
        Event       event;                      // on this event
        Outcome     outcome = Outcome::OK;      // given outcome from current state is
        GuardFn     guard = nullptr;            // and given guard is OK
        ActionFn    action = nullptr;           // then do transition action
        StateKey    next;                       // and move to next state.
    };

    // --- Construct a transition table with N rows ---
    template<class State, class Event, class DCtx, std::size_t N>
    using TransitionTableT = std::array<core_fsm::TransitionEntryT<State, Event, DCtx>, N>;

    // --- Event Queue base requirements - implementable with STL, FreeRTOS, etc ---
    template<class Q, class Event>
    concept EventQueue = requires(Q& q, const Event& ev_in, Event& ev_out) {
        { q.try_push(ev_in)     } -> std::same_as<bool>; 
        { q.pop(ev_out)         } -> std::same_as<bool>;
        { q.empty()             } -> std::same_as<bool>;
    };

    // --- Event Queue embedded requirement - implementable with FreeRTOS ---
    template<class Q, class Event>
    concept HasIsr = requires(Q& q, const Event& ev_in) {
        { q.try_push_from_isr(ev_in) } -> std::same_as<bool>;
    };

    // --- State: Interface to design state objects against ---
    template<class DCtx, class Event, class Outcome>
    struct StateT {
        virtual ~StateT() = default;
        virtual Outcome on_event(DCtx& ctx, const Event& ev) = 0;
        virtual void on_enter(DCtx& ctx) noexcept { (void)ctx; }
        virtual void on_exit (DCtx& ctx) noexcept { (void)ctx; }
    };

    // --- ContextBaseT: CRTP base to build/inherit contexts from ---
    template <class DerivedCtx,                                     // Actual context (machine), derived from ContextBaseT (this class)
              class StateKey,                                       // State names (enum class)
              class Event,                                          // Event type (enum class)
              class Queue,                                          
              std::size_t NumStates>
    requires core_fsm::EventQueue<Queue, Event>                     // predicate on template arguments
    struct ContextBaseT {
    public:
        using Outcome           = core_fsm::Outcome;
        using State             = core_fsm::StateT<DerivedCtx, Event, Outcome>;
        using Transition        = core_fsm::TransitionEntryT<StateKey, Event, DerivedCtx>;
        
        ContextBaseT() = default;
        ~ContextBaseT() = default;

        // --- Enforce single machine instance, not copyable, not movable (unsafe as queue is owned!) ---
        ContextBaseT(const ContextBaseT&) = delete;
        ContextBaseT& operator=(ContextBaseT&) = delete;
        ContextBaseT& operator=(const ContextBaseT&) = delete;
        
        ContextBaseT(ContextBaseT&&) = delete;
        ContextBaseT& operator=(ContextBaseT&&) = delete;
        ContextBaseT& operator=(const ContextBaseT&&) = delete;

        // --- Setup-API to register StateKey -> State* ---
        void register_state(StateKey key, /*const*/ State& state) {
            const auto idx = static_cast<std::size_t>(key);
            if (idx < NumStates) states_[idx] = &state;
        }

        // --- Setup-API for users to attach an N-length transition table ---
        // --- LIFETIME OF table MUST BE LONGER THAN table_=std::span
        template<std::size_t N>
        void set_table(const TransitionTableT<StateKey, Event, DerivedCtx, N>& table) {
            table_ = std::span{table};      // deduces pointer and size
        }

        // --- Setup-API for users to initialize the machine to a particular state ---
        // --- Do not let the machine silently be in an uninitialized state ---
        [[nodiscard]] bool initialize(StateKey start_state_key) noexcept {
            if (is_initialized_) return false;                          // guard reinit. of already init'ed machine
            if (!is_valid_statekey(start_state_key)) return false;      // not registered
            commit_state(start_state_key);                              // save state
            run_next_state_enter_hook(start_state_key);
            is_initialized_ = true;                                     // activate guard
            return is_initialized_;                                     // OK
        }

        // --- Runtime-API for producers to send events ---
        [[nodiscard]] bool post(const Event& event) noexcept { return queue_.try_push(event); }
        [[nodiscard]] bool post_from_isr(const Event& event) noexcept requires HasIsr<Queue, Event> { return queue_.try_push_from_isr(event); }

        // --- Runtime-API for users to step the machine (consume from queue) ---
        bool step() noexcept {
            if (!is_initialized_ || !state_) return false;              // Do not try to run an uninitialized machine
            
            Event event;
            if (queue_.pop(event)) {                                    // If using FreeRTOS, this will block
                Outcome outcome = state_->on_event(self(), event);      // state obj handles the event
                return dispatch(event, outcome);
            }
            return false;                                               // No step taken
        }

        // --- Runtime-API for users to Inspect state ---
        StateKey get_state_key(void) const noexcept {
            return state_key_;
        }

    protected:

        inline std::size_t key_to_idx(const StateKey& key) const noexcept {
            return static_cast<std::size_t>(key);
        }

        inline bool is_valid_statekey(const StateKey& key) const noexcept {
            // --- a valid state key has a state object registered ---
            const auto idx = key_to_idx(key);
            return (idx < states_.size()) && (states_[idx] != nullptr);
        }

        inline State* key_to_obj(const StateKey& key) const noexcept {
            return is_valid_statekey(key) ? states_[key_to_idx(key)] : nullptr;
        }

        inline bool is_self_transition(const StateKey& next_state) const noexcept {
            return (next_state == state_key_);
        }

        inline void run_current_state_exit_hook(void) noexcept {
            if (state_) state_->on_exit(self());
        }

        inline void run_next_state_enter_hook(const StateKey& next_state_key) noexcept {
            if (is_valid_statekey(next_state_key)) {
                key_to_obj(next_state_key)->on_enter(self());
            }
        }

        inline void commit_state(const StateKey& next_state_key) noexcept {
            if (is_valid_statekey(next_state_key)) {
                state_key_ = next_state_key;
                state_ = key_to_obj(next_state_key);
            }
        }

        // --- Internal: Update state via event+transition ---
        bool dispatch(const Event& event, const Outcome& outcome) noexcept {
            // --- State has evaluated the event, and given us the outcome ---
            if (outcome == Outcome::IGNORE) return false;               // IGNORE -> no dispatch
            if (outcome == Outcome::FAIL)   return false;               // FAIL -> needs a handler

            // --- Dispatch must ensure the transition order ---
            // 1) Check if transition is described in table
            auto transition = lookup_transition(state_key_, event, outcome);
            if (transition == nullptr)      return false;               // Not in table

            // 2) Check if next state is valid
            if (!is_valid_statekey(transition->next)) 
                                            return false;               // Not registered

            // 3) Check guard
            if (transition->guard && 
                !transition->guard(self())) return false;               // Guard failed

            // Must store flag as state is mutating during the process
            auto next_statekey = transition->next;
            bool is_self_flag = is_self_transition(next_statekey);

            // 4) Exit hook with state seeing pre-transition context. No hook on self-transitions.
            if (!is_self_flag) run_current_state_exit_hook();

            // 5) Run transition action, might mutate context.
            if (transition->action) transition->action(self());

            // 6) Commit
            commit_state(next_statekey);

            // 7) Enter hook with state seeing post-transition context. No hook on self-transitions.
            if (!is_self_flag) run_next_state_enter_hook(next_statekey);

            // Dispatch success
            return true;
        }

    private:
        // --- Lookup transition by (StateKey, Event, OutcomeT) in the table, and return pointer to the relevant row ---
        const Transition* lookup_transition(StateKey from, const Event& event, const Outcome& outcome) const noexcept {
            for (const auto& row : table_) {
                if (row.from == from && row.event == event && row.outcome == outcome) {
                    return &row;
                }
            }
            return nullptr;
        }

        // --- Refererence to DerivedCtx by CRTP idiom ---
        DerivedCtx&       self() noexcept       { return static_cast<DerivedCtx&>(*this); }
        const DerivedCtx& self() const noexcept { return static_cast<const DerivedCtx&>(*this); }

        // --- Owning queue ---
        Queue queue_{};

        // --- Manage current FSM state, State lifetime is owned by user ---
        std::array<State*, NumStates> states_ {};   // all registered states
        State* state_ {nullptr};                    // current state obj
        StateKey state_key_ {};                     // current state name

        // --- Non-owning a transition table ---
        std::span<const Transition> table_ {};

        // --- One-time initialization of state ---
        bool is_initialized_ {false};
    }; // ContextBaseT
    
    // --- Generic builder. Queue policy differentiates machines purely on the queue family and capacity ---
    template<
        template<class, std::size_t> class QueueAdapter,    // signature constraint QueueAdapter<Event, Capacity>
        std::size_t Capacity,
        class DCtx, 
        class StateKey,
        class Event,
        std::size_t NumStates>
    struct MachineBuilder {
        using EventQueue    = QueueAdapter<Event, Capacity>;
        using BaseCtx       = ContextBaseT<DCtx, StateKey, Event, EventQueue, NumStates>;
        using DerivedCtx    = DCtx;
        using MachineBaseT  = BaseCtx;
        using Outcome       = typename BaseCtx::Outcome;
        using State         = typename BaseCtx::State;
        using Transition    = typename BaseCtx::Transition;
        template<std::size_t N> using TransitionTable = std::array<Transition, N>;
        static constexpr auto g_always  = core_fsm::g_always<DCtx>;
        static constexpr auto g_never   = core_fsm::g_never<DCtx>;
        static constexpr auto a_nothing = core_fsm::a_nothing<DCtx>;
    };

    // --- This builder is a Factory on non-embedded targets using STL Queues---
    template <class DCtx, class StateKey, class Event, std::size_t NumStates, std::size_t QEventCapacity>
    using StlQueueMachineBuilder =
        MachineBuilder<core_fsm::StlQueueAdapter, QEventCapacity, DCtx, StateKey, Event, NumStates>;

    // --- This builder is a on embedded targets using FreeRTOS Queues---
    template <class DCtx, class StateKey, class Event, std::size_t NumStates, std::size_t QEventCapacity>
    using FreeRTOSQueueMachineBuilder =
        MachineBuilder<core_fsm::FreeRTOSQueueAdapter, QEventCapacity, DCtx, StateKey, Event, NumStates>;

} // core_fsm