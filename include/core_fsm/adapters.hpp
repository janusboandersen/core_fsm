/**
 * These adapters provide a mechanism to the FSM to queue up events on different systems.
 * 
 * If FreeRTOS is available, then
 * 
 * Janus, July 2025
 */
#pragma once

#include <cstdint>
#include <type_traits>

// --- Check for FreeRTOS queues ---
#if __has_include("FreeRTOS.h") && __has_include("queue.h")
    // --- Use FreeRTOS queues ---
    #define CORE_FSM_HAS_FREERTOS 1
    #include "FreeRTOS.h"
    #include "queue.h"
#elif __has_include("freertos/FreeRTOS.h") && __has_include("freertos/queue.h")
    // --- Same ---
    #define CORE_FSM_HAS_FREERTOS 1
    #include "freertos/FreeRTOS.h"
    #include "freertos/queue.h"
#else
    // --- Use STL queues ---
    #define CORE_FSM_HAS_FREERTOS 0
    #include <queue>
#endif


namespace core_fsm {

    // --- Adapter for STL queue ---
    template<class Event, std::size_t CAP = 128>
    struct StlQueueAdapter {
    #if CORE_FSM_HAS_FREERTOS == 0
        // --- Implement STL Queue adapter for host usage ---
        [[nodiscard]] bool try_push(const Event& ev_in) noexcept {
            if (q_.size() >= CAP) return false;     // reject
            q_.push(ev_in);
            return true;
        }

        [[nodiscard]] bool pop(Event& ev_out) noexcept {
            if (q_.empty()) return false;
            ev_out = q_.front();                    // gets the oldest
            q_.pop();
            return true;
        }

        [[nodiscard]] bool empty() const noexcept {
            return q_.empty();
        }

    private:
        std::queue<Event> q_;

    #else
        // --- Do not use STL queues on embedded system ---
        StlQueueAdapter() noexcept {
            static_assert(!CORE_FSM_HAS_FREERTOS, 
            "FreeRTOS headers are available. Do not instantiate StlQueueAdapter on embedded system.");
        }

        [[nodiscard]] bool try_push(const Event&) noexcept { return false; };
        [[nodiscard]] bool pop(Event&) noexcept { return false; };
        [[nodiscard]] bool empty() const noexcept { return true; };
    #endif
    };


    // --- Adapter for FreeRTOS queue, to be completed ---
    template<class Event, std::size_t CAP = 128>
    struct FreeRTOSQueueAdapter {
    #if CORE_FSM_HAS_FREERTOS
        // --- Implement statically allocated FreeRTOS Queue adapter for embedded platform ---
        static_assert(CAP > 0, "CAP must be > 0");
        static_assert(std::is_trivially_copyable_v<Event>,
              "Event type must be trivially copyable for FreeRTOS queue storage.");
        
        FreeRTOSQueueAdapter() noexcept {
            q_ = xQueueCreateStatic(
                static_cast<UBaseType_t>(CAP),              // Queue length
                static_cast<UBaseType_t>(sizeof(Event)),    // Item size
                q_storage_,                                 // Ptr to statically allocated memory for queue
                &q_control_                                 // Ptr to data structure
            );
        }

        // --- Enforce single adapter instance, not copyable, not movable (unsafe as queue is owned!) ---
        FreeRTOSQueueAdapter(const FreeRTOSQueueAdapter&) = delete;
        FreeRTOSQueueAdapter& operator=(FreeRTOSQueueAdapter&) = delete;
        FreeRTOSQueueAdapter& operator=(const FreeRTOSQueueAdapter&) = delete;

        FreeRTOSQueueAdapter(FreeRTOSQueueAdapter&&) = delete;
        FreeRTOSQueueAdapter& operator=(FreeRTOSQueueAdapter&&) = delete;
        FreeRTOSQueueAdapter& operator=(const FreeRTOSQueueAdapter&&) = delete;

        [[nodiscard]] bool try_push(const Event& ev_in) noexcept { 
            // --- Non-blocking (0 ticks wait) ---
            return xQueueSendToBack(q_, &ev_in, 0) == pdPASS; 
        };

        [[nodiscard]] bool try_push_from_isr(const Event& ev_in) noexcept { 
            // --- Safe from ISR, with option for immediate hand-off---
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            const BaseType_t is_ok = xQueueSendToBackFromISR(q_, &ev_in, &xHigherPriorityTaskWoken);

            // --- Immediate hand-off to hi any prio task (e.g. emergency stop) ---
            if (xHigherPriorityTaskWoken == pdTRUE) {
                portYIELD_FROM_ISR();
            }

            return is_ok == pdPASS; 
        };

        [[nodiscard]] bool pop(Event& ev_out) noexcept {
            // --- Blocking on embedded, single consumer, RTC ---
            return xQueueReceive(q_, &ev_out, portMAX_DELAY) == pdPASS;
        };

        [[nodiscard]] bool empty() const noexcept { 
            return uxQueueMessagesWaiting(q_) == 0;
        };

    private:
        QueueHandle_t q_{nullptr};                                  // Queue accessor
        alignas(Event) uint8_t q_storage_[CAP * sizeof(Event)]{};   // Queue memory
        StaticQueue_t q_control_{};                                 // Queue metadata

    #else
        // --- Do not use FreeRTOS queues on host system ---
        FreeRTOSQueueAdapter() noexcept {
            static_assert(CORE_FSM_HAS_FREERTOS, 
            "FreeRTOS headers are not available. Do not instantiate FreeRTOSQueueAdapter on host system.");
        }

        [[nodiscard]] bool try_push(const Event&) noexcept { return false; };
        [[nodiscard]] bool try_push_from_isr(const Event&) noexcept { return false; }
        [[nodiscard]] bool pop(Event&) noexcept { return false; };
        [[nodiscard]] bool empty() const noexcept { return true; };
    #endif
    };

} // core_fsm