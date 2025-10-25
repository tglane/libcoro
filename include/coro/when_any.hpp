#pragma once

// EMSCRIPTEN does not currently support std::jthread or std::stop_source|token.
#include <memory>
#ifndef EMSCRIPTEN

    #include "coro/concepts/awaitable.hpp"
    #include "coro/detail/task_self_deleting.hpp"
    #include "coro/event.hpp"
    #include "coro/expected.hpp"
    #include "coro/mutex.hpp"
    #include "coro/task.hpp"
    #include "coro/when_all.hpp"

    #include <atomic>
    #include <cassert>
    #include <coroutine>
    #include <iostream>
    #include <optional>
    #include <stop_token>
    #include <utility>
    #include <vector>

namespace coro
{

namespace detail
{

template<typename T>
struct when_any_variant_traits
{
    using type = T;
};

template<>
struct when_any_variant_traits<void>
{
    using type = std::monostate;
};

template<size_t index, typename return_type, concepts::awaitable awaitable>
auto make_when_any_tuple_task(
    std::atomic<bool>&                        first_completed,
    size_t&                                   first_completed_index,
    std::vector<std::coroutine_handle<void>>& handles,
    coro::event&                              notify,
    std::optional<return_type>&               return_value,
    awaitable                                 a) -> coro::task<void>
{
    auto expected = false;
    if constexpr (concepts::awaitable_void<awaitable>)
    {
        co_await static_cast<awaitable&&>(a);
        if (first_completed.compare_exchange_strong(
                expected, true, std::memory_order::acq_rel, std::memory_order::relaxed))
        {
            first_completed_index = index;
            return_value.emplace(std::in_place_index<index>, std::monostate{});

            // Destroy the remaining handles to stop there coroutines from running indefinetely
            for (size_t i = 0; i < handles.size(); ++i)
            {
                if (i != first_completed_index && handles[i])
                {
                    std::cout << "Resuming handle at " << handles[i].address() << std::endl;
                    handles[i].resume();
                }
            }

            notify.set();
        }
    }
    else
    {
        auto result = co_await static_cast<awaitable&&>(a);
        if (first_completed.compare_exchange_strong(
                expected, true, std::memory_order::acq_rel, std::memory_order::relaxed))
        {
            first_completed_index = index;
            return_value.emplace(std::in_place_index<index>, std::move(result));

            // Destroy the remaining handles to stop there coroutines from running indefinetely
            for (size_t i = 0; i < handles.size(); ++i)
            {
                if (i != first_completed_index && handles[i])
                {
                    std::cout << "Resuming handle at " << handles[i].address() << std::endl;
                    handles[i].resume();
                }
            }

            notify.set();
        }
    }

    co_return;
}

template<typename... Ts>
std::vector<std::coroutine_handle<void>> collect_coroutine_handles(Ts&... tasks)
{
    auto handles = std::vector<std::coroutine_handle<void>>();
    handles.reserve(sizeof...(tasks));
    auto collect = [&handles](auto&... task_refs) { ((handles.push_back(task_refs.handle())), ...); };
    (collect(tasks), ...);
    return handles;
}

template<typename return_type, concepts::awaitable... awaitable_type, size_t... indices>
[[nodiscard]] auto make_when_any_tuple_controller_task(
    std::index_sequence<indices...>,
    coro::event&                notify,
    std::optional<return_type>& return_value,
    awaitable_type... awaitables) -> coro::detail::task_self_deleting
{
    std::atomic<bool> first_completed{false};
    size_t            first_completed_index{};

    auto handles = collect_coroutine_handles(awaitables...);

    // Create all tasks
    auto tasks = std::make_tuple(
        make_when_any_tuple_task<indices>(
            first_completed, first_completed_index, handles, notify, return_value, std::move(awaitables))...);

    // auto handles = collect_coroutine_handles(tasks);

    // Create a cancellation task that will destroy remaining tasks after first completion
    // auto cancellation_task = [&tasks, &notify]() -> coro::task<void>
    // auto cancellation_task = [&handles, &notify, &first_completed_index]() -> coro::task<void>
    // {
    //     co_await notify; // Wait for first completion

    //     // for (auto& handle : handles)
    //     // {
    //     //     handle.destroy();
    //     // }
    //     for (size_t i = 0; i < handles.size(); ++i)
    //     {
    //         if (i != first_completed_index)
    //         {
    //             // handles[i].resume();
    //         }
    //     }

    //     co_return;
    // };

    // Problem: tasks will on dtor also destroy the handles that were previously destroyed by the when_any_tuple_tasks
    //  -> we need to move the handle out of the task after we destroyed it so that the task can not destroy it.

    // Start all tasks and the cancellation task
    // co_await coro::when_all(std::get<indices>(std::move(tasks))..., cancellation_task());
    co_await coro::when_all(std::get<indices>(std::move(tasks))...);

    co_return;
}

template<concepts::awaitable awaitable>
static auto make_when_any_task_return_void(awaitable a, std::atomic<bool>& first_completed, coro::event& notify)
    -> coro::task<void>
{
    co_await static_cast<awaitable&&>(a);
    auto expected = false;
    if (first_completed.compare_exchange_strong(expected, true, std::memory_order::acq_rel, std::memory_order::relaxed))
    {
        notify.set(); // This will trigger the controller task to wake up exactly once.
    }
    co_return;
}

template<concepts::awaitable awaitable, typename return_type>
static auto make_when_any_task(
    awaitable a, std::atomic<bool>& first_completed, coro::event& notify, std::optional<return_type>& return_value)
    -> coro::task<void>
{
    auto expected = false;
    auto result   = co_await static_cast<awaitable&&>(a);
    // Its important to only touch return_value and notify once since their lifetimes will be destroyed
    // after being set and notified the first time.
    if (first_completed.compare_exchange_strong(expected, true, std::memory_order::acq_rel, std::memory_order::relaxed))
    {
        return_value = std::move(result);
        notify.set();
    }

    co_return;
}

template<std::ranges::range range_type, concepts::awaitable awaitable_type = std::ranges::range_value_t<range_type>>
static auto make_when_any_controller_task_return_void(range_type awaitables, coro::event& notify)
    -> coro::detail::task_self_deleting
{
    std::atomic<bool>             first_completed{false};
    std::vector<coro::task<void>> tasks{};

    if constexpr (std::ranges::sized_range<range_type>)
    {
        tasks.reserve(std::size(awaitables));
    }

    for (auto&& a : awaitables)
    {
        tasks.emplace_back(make_when_any_task_return_void<awaitable_type>(std::move(a), first_completed, notify));
    }

    co_await coro::when_all(std::move(tasks));
    // Create a cancellation task that will destroy remaining tasks after first completion
    // auto cancellation_task = [&tasks, &notify]() -> coro::task<void>
    // {
    //     co_await notify; // Wait for first completion

    //     // Destroy all tasks to cancel them
    //     for (auto& task : tasks)
    //     {
    //         task.destroy();
    //     }
    //     co_return;
    // };

    // co_await coro::when_all(std::move(tasks), cancellation_task());
    co_return;
}

template<
    std::ranges::range  range_type,
    concepts::awaitable awaitable_type = std::ranges::range_value_t<range_type>,
    typename return_type               = typename concepts::awaitable_traits<awaitable_type>::awaiter_return_type,
    typename return_type_base          = std::remove_reference_t<return_type>>
static auto make_when_any_controller_task(
    range_type awaitables, coro::event& notify, std::optional<return_type_base>& return_value)
    -> coro::detail::task_self_deleting
{
    // This must live for as long as the longest running when_any task since each task tries to see
    // if it was the first to complete. Only the very first task to complete will set the return_value
    // and notify.
    std::atomic<bool> first_completed{false};

    // This detatched task will maintain the lifetime of all the when_any tasks.
    std::vector<coro::task<void>> tasks{};

    if constexpr (std::ranges::sized_range<range_type>)
    {
        tasks.reserve(std::size(awaitables));
    }

    for (auto&& a : awaitables)
    {
        tasks.emplace_back(
            make_when_any_task<awaitable_type, return_type_base>(std::move(a), first_completed, notify, return_value));
    }

    co_await coro::when_all(std::move(tasks));
    // Create a cancellation task that will destroy remaining tasks after first completion
    // auto cancellation_task = [&tasks, &notify]() -> coro::task<void>
    // {
    //     co_await notify; // Wait for first completion

    //     // Destroy all tasks to cancel them
    //     for (auto& task : tasks)
    //     {
    //         task.destroy();
    //     }
    //     co_return;
    // };

    // co_await coro::when_all(std::move(tasks), cancellation_task());
    co_return;
}

} // namespace detail

template<concepts::awaitable... awaitable_type>
[[nodiscard]] auto when_any(std::stop_source stop_source, awaitable_type... awaitables)
    -> coro::task<std::variant<typename detail::when_any_variant_traits<
        std::remove_reference_t<typename concepts::awaitable_traits<awaitable_type>::awaiter_return_type>>::type...>>
{
    using return_type = std::variant<typename detail::when_any_variant_traits<
        std::remove_reference_t<typename concepts::awaitable_traits<awaitable_type>::awaiter_return_type>>::type...>;

    coro::event                notify{};
    std::optional<return_type> return_value{std::nullopt};

    auto controller_task = detail::make_when_any_tuple_controller_task(
        std::index_sequence_for<awaitable_type...>{},
        notify,
        return_value,
        std::forward<awaitable_type>(awaitables)...);
    controller_task.handle().resume();

    co_await notify;
    stop_source.request_stop();
    co_return std::move(return_value.value());
}

template<concepts::awaitable... awaitable_type>
[[nodiscard]] auto when_any(awaitable_type... awaitables)
    -> coro::task<std::variant<typename detail::when_any_variant_traits<
        std::remove_reference_t<typename concepts::awaitable_traits<awaitable_type>::awaiter_return_type>>::type...>>
{
    using return_type = std::variant<typename detail::when_any_variant_traits<
        std::remove_reference_t<typename concepts::awaitable_traits<awaitable_type>::awaiter_return_type>>::type...>;

    coro::event                notify{};
    std::optional<return_type> return_value{std::nullopt};

    auto controller_task = detail::make_when_any_tuple_controller_task(
        std::index_sequence_for<awaitable_type...>{},
        notify,
        return_value,
        std::forward<awaitable_type>(awaitables)...);
    controller_task.handle().resume();

    co_await notify;
    co_return std::move(return_value.value());
}

template<
    std::ranges::range  range_type,
    concepts::awaitable awaitable_type = std::ranges::range_value_t<range_type>,
    typename return_type               = typename concepts::awaitable_traits<awaitable_type>::awaiter_return_type,
    typename return_type_base          = std::remove_reference_t<return_type>>
[[nodiscard]] auto when_any(std::stop_source stop_source, range_type awaitables) -> coro::task<return_type_base>
{
    coro::event notify{};

    if constexpr (std::is_void_v<return_type_base>)
    {
        auto controller_task =
            detail::make_when_any_controller_task_return_void(std::forward<range_type>(awaitables), notify);
        controller_task.handle().resume();

        co_await notify;
        stop_source.request_stop();
        co_return;
    }
    else
    {
        // Using an std::optional to prevent the need to default construct the type on the stack.
        std::optional<return_type_base> return_value{std::nullopt};

        auto controller_task =
            detail::make_when_any_controller_task(std::forward<range_type>(awaitables), notify, return_value);
        controller_task.handle().resume();

        co_await notify;
        stop_source.request_stop();
        co_return std::move(return_value.value());
    }
}

template<
    std::ranges::range  range_type,
    concepts::awaitable awaitable_type = std::ranges::range_value_t<range_type>,
    typename return_type               = typename concepts::awaitable_traits<awaitable_type>::awaiter_return_type,
    typename return_type_base          = std::remove_reference_t<return_type>>
[[nodiscard]] auto when_any(range_type awaitables) -> coro::task<return_type_base>
{
    coro::event notify{};

    if constexpr (std::is_void_v<return_type_base>)
    {
        auto controller_task =
            detail::make_when_any_controller_task_return_void(std::forward<range_type>(awaitables), notify);
        controller_task.handle().resume();

        co_await notify;
        co_return;
    }
    else
    {
        std::optional<return_type_base> return_value{std::nullopt};

        auto controller_task =
            detail::make_when_any_controller_task(std::forward<range_type>(awaitables), notify, return_value);
        controller_task.handle().resume();

        co_await notify;
        co_return std::move(return_value.value());
    }
}

} // namespace coro

#endif // EMSCRIPTEN
