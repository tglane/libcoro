#include "catch_amalgamated.hpp"
#include "catch_extensions.hpp"

#include <chrono>
#include <coro/coro.hpp>
#include <iostream>
#include <stop_token>
#include <variant>

TEST_CASE("when_any", "[when_any]")
{
    std::cerr << "[when_any]\n\n";
}

TEST_CASE("when_any two tasks", "[when_any]")
{
    std::cerr << "BEGIN when_any two tasks\n";
    auto make_task = [](uint64_t amount) -> coro::task<uint64_t> { co_return amount; };

    std::vector<coro::task<uint64_t>> tasks{};
    tasks.emplace_back(make_task(1));
    tasks.emplace_back(make_task(2));

    auto result = coro::sync_wait(coro::when_any(std::move(tasks)));
    REQUIRE(result == 1);
}

TEST_CASE("when_any return void", "[when_any]")
{
    std::cerr << "BEGIN when_any return void\n";
    auto tp = coro::thread_pool::make_shared();
    std::atomic<uint64_t> counter{0};

    auto make_task = [](std::shared_ptr<coro::thread_pool> tp, std::atomic<uint64_t>& counter, uint64_t i) -> coro::task<void>
    {
        co_await tp->schedule();
        // One thread will win.
        uint64_t expected = 0;
        counter.compare_exchange_strong(expected, i);
        co_return;
    };

    std::vector<coro::task<void>> tasks;
    for (auto i = 1; i <= 4; ++i)
    {
        tasks.emplace_back(make_task(tp, counter, i));
    }

    coro::sync_wait(coro::when_any(std::move(tasks)));
    REQUIRE(counter.load() > 0);
}

TEST_CASE("when_any tuple return void (monostate)", "[when_any]")
{
    std::cerr << "BEGIN when_any tuple return void (monostate)\n";

    // This test needs to use a mutex to guarantee that the task that sets the counter
    // is the first task to complete, otherwise there is a race condition if counter is atomic
    // as the other task could complete first (unlikely but happens) and cause the REQUIRE statements
    // between what is returned to mismatch from what is executed.
    coro::mutex       m{};
    auto tp = coro::thread_pool::make_shared();
    std::atomic<uint64_t>          counter{0};

    auto make_task_return_void =
        [](std::shared_ptr<coro::thread_pool> tp, coro::mutex& m, std::atomic<uint64_t>& counter, uint64_t i) -> coro::task<std::monostate>
    {
        co_await tp->schedule();
        co_await m.lock();
        if (counter == 0)
        {
            counter = i;
        }
        else
        {
            REQUIRE_THREAD_SAFE(counter == 2);
        }
        co_return std::monostate{};
    };

    auto make_task = [](std::shared_ptr<coro::thread_pool> tp, coro::mutex& m, std::atomic<uint64_t>& counter, uint64_t i) -> coro::task<uint64_t>
    {
        co_await tp->schedule();
        co_await m.lock();
        if (counter == 0)
        {
            counter = i;
        }
        else
        {
            REQUIRE_THREAD_SAFE(counter == 1);
        }
        co_return i;
    };

    auto result =
        coro::sync_wait(coro::when_any(make_task_return_void(tp, m, counter, 1), make_task(tp, m, counter, 2)));
    // Because of how coro::mutex works.. we need to release it after when_any returns since it symetrically transfers to the other coroutine task
    // and can cause a race condition where the result does not equal the counter. This guarantees the task has fully completed before issuing REQUIREs.
    m.unlock();
    std::atomic_thread_fence(std::memory_order::acq_rel);

    if (std::holds_alternative<std::monostate>(result))
    {
        REQUIRE(counter == 1);
    }
    else
    {
        REQUIRE(std::get<uint64_t>(result) == 2);
        REQUIRE(counter == 2);
    }
}

#ifdef LIBCORO_FEATURE_NETWORKING

TEST_CASE("when_any two tasks one long running", "[when_any]")
{
    std::cerr << "BEGIN when_any two tasks one long running\n";
    auto s = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto make_task = [](std::shared_ptr<coro::io_scheduler> s, uint64_t amount) -> coro::task<uint64_t>
    {
        co_await s->schedule();
        // Make sure both tasks are scheduled.
        co_await s->yield_for(std::chrono::milliseconds{10});
        if (amount == 1)
        {
            co_await s->yield_for(std::chrono::milliseconds{100});
        }
        co_return amount;
    };

    std::vector<coro::task<uint64_t>> tasks{};
    tasks.emplace_back(make_task(s, 1));
    tasks.emplace_back(make_task(s, 2));

    auto result = coro::sync_wait(coro::when_any(std::move(tasks)));
    REQUIRE(result == 2);

    // not using shutdown since it prevents yield_for/scheduling to be rejected.
    while (!s->empty())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

TEST_CASE("when_any two tasks one long running with cancellation", "[when_any]")
{
    std::cerr << "BEGIN when_any two tasks one long running with cancellation\n";
    std::stop_source stop_source{};
    auto             s = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    std::atomic<bool> thrown{false};

    auto make_task =
        [](std::shared_ptr<coro::io_scheduler> s, std::stop_token stop_token, uint64_t amount, std::atomic<bool>& thrown) -> coro::task<uint64_t>
    {
        co_await s->schedule();
        try
        {
            if (amount == 1)
            {
                std::cerr << "yielding with amount=" << amount << "\n";
                co_await s->yield_for(std::chrono::milliseconds{100});
                if (stop_token.stop_requested())
                {
                    std::cerr << "throwing\n";
                    throw std::runtime_error{"task was cancelled"};
                }
                else
                {
                    std::cerr << "not throwing\n";
                }
            }
        }
        catch (const std::exception& e)
        {
            REQUIRE(amount == 1);
            REQUIRE(e.what() == std::string{"task was cancelled"});
            thrown = true;
        }
        co_return amount;
    };

    std::vector<coro::task<uint64_t>> tasks{};
    tasks.emplace_back(make_task(s, stop_source.get_token(), 1, thrown));
    tasks.emplace_back(make_task(s, stop_source.get_token(), 2, thrown));

    auto result = coro::sync_wait(coro::when_any(std::move(stop_source), std::move(tasks)));
    REQUIRE(result == 2);

    while (!thrown)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
}

TEST_CASE("when_any timeout", "[when_any]")
{
    std::cerr << "BEGIN when_any timeout\n";
    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.pool = coro::thread_pool::options{.thread_count = 2}});

    auto make_long_running_task = [](std::shared_ptr<coro::io_scheduler> scheduler,
                                     std::chrono::milliseconds           execution_time) -> coro::task<int64_t>
    {
        co_await scheduler->schedule();
        co_await scheduler->yield_for(execution_time);
        co_return 1;
    };

    auto make_timeout_task = [](std::shared_ptr<coro::io_scheduler> scheduler,
                                std::chrono::milliseconds timeout) -> coro::task<int64_t>
    {
        co_await scheduler->schedule_after(timeout);
        co_return -1;
    };

    {
        std::vector<coro::task<int64_t>> tasks{};
        tasks.emplace_back(make_long_running_task(scheduler, std::chrono::milliseconds{50}));
        tasks.emplace_back(make_timeout_task(scheduler, std::chrono::milliseconds{500}));

        auto result = coro::sync_wait(coro::when_any(std::move(tasks)));
        REQUIRE(result == 1);
    }

    {
        std::vector<coro::task<int64_t>> tasks{};
        tasks.emplace_back(make_long_running_task(scheduler, std::chrono::milliseconds{500}));
        tasks.emplace_back(make_timeout_task(scheduler, std::chrono::milliseconds{50}));

        auto result = coro::sync_wait(coro::when_any(std::move(tasks)));
        REQUIRE(result == -1);
    }
}

TEST_CASE("when_any io_scheduler::schedule(task, timeout)", "[when_any]")
{
    std::cerr << "BEGIN when_any io_scheduler::schedule(task, timeout)\n";
    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.pool = coro::thread_pool::options{.thread_count = 2}});

    auto make_task = [](std::shared_ptr<coro::io_scheduler> scheduler,
                        std::chrono::milliseconds           execution_time) -> coro::task<int64_t>
    {
        co_await scheduler->yield_for(execution_time);
        co_return 1;
    };

    {
        auto result = coro::sync_wait(
            scheduler->schedule(make_task(scheduler, std::chrono::milliseconds{10}), std::chrono::milliseconds{50}));
        REQUIRE(result.has_value());
        REQUIRE(result.value() == 1);
    }

    {
        auto result = coro::sync_wait(
            scheduler->schedule(make_task(scheduler, std::chrono::milliseconds{50}), std::chrono::milliseconds{10}));
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error() == coro::timeout_status::timeout);
    }
}

    #ifndef EMSCRIPTEN
TEST_CASE("when_any io_scheduler::schedule(task, timeout stop_token)", "[when_any]")
{
    std::cerr << "BEGIN when_any io_scheduler::schedule(task, timeout stop_token)\n";
    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.pool = coro::thread_pool::options{.thread_count = 2}});

    auto make_task = [](std::shared_ptr<coro::io_scheduler> scheduler,
                        std::chrono::milliseconds           execution_time,
                        std::stop_token                     stop_token) -> coro::task<int64_t>
    {
        co_await scheduler->yield_for(execution_time);
        if (stop_token.stop_requested())
        {
            co_return -1;
        }
        co_return 1;
    };

    {
        std::stop_source stop_source{};
        auto             result = coro::sync_wait(scheduler->schedule(
            std::move(stop_source),
            make_task(scheduler, std::chrono::milliseconds{10}, stop_source.get_token()),
            std::chrono::milliseconds{50}));
        REQUIRE(result.has_value());
        REQUIRE(result.value() == 1);
    }

    {
        std::stop_source stop_source{};
        auto             result = coro::sync_wait(scheduler->schedule(
            std::move(stop_source),
            make_task(scheduler, std::chrono::milliseconds{50}, stop_source.get_token()),
            std::chrono::milliseconds{10}));
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error() == coro::timeout_status::timeout);
    }
}
    #endif

TEST_CASE("when_any tuple multiple", "[when_any]")
{
    std::cerr << "BEGIN when_any tuple multiple\n";
    using namespace std::chrono_literals;

    auto scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.pool = coro::thread_pool::options{.thread_count = 4}});

    auto make_task1 = [](std::shared_ptr<coro::io_scheduler> scheduler,
                         std::chrono::milliseconds           execution_time) -> coro::task<int>
    {
        co_await scheduler->schedule_after(execution_time);
        co_return 1;
    };

    auto make_task2 = [](std::shared_ptr<coro::io_scheduler> scheduler,
                         std::chrono::milliseconds           execution_time) -> coro::task<double>
    {
        co_await scheduler->schedule_after(execution_time);
        co_return 3.14;
    };

    auto make_task3 = [](std::shared_ptr<coro::io_scheduler> scheduler,
                         std::chrono::milliseconds           execution_time) -> coro::task<std::string>
    {
        co_await scheduler->schedule_after(execution_time);
        co_return std::string{"hello world"};
    };

    {
        auto result = coro::sync_wait(
            coro::when_any(make_task1(scheduler, 10ms), make_task2(scheduler, 150ms), make_task3(scheduler, 150ms)));
        REQUIRE(result.index() == 0);
        REQUIRE(std::get<0>(result) == 1);
    }

    {
        auto result = coro::sync_wait(
            coro::when_any(make_task1(scheduler, 150ms), make_task2(scheduler, 10ms), make_task3(scheduler, 150ms)));
        REQUIRE(result.index() == 1);
        REQUIRE(std::get<1>(result) == 3.14);
    }

    {
        auto result = coro::sync_wait(
            coro::when_any(make_task1(scheduler, 150ms), make_task2(scheduler, 150ms), make_task3(scheduler, 10ms)));
        REQUIRE(result.index() == 2);
        REQUIRE(std::get<2>(result) == "hello world");
    }
}

#endif

TEST_CASE("~when_any", "[when_any]")
{
    std::cerr << "[~when_any]\n\n";
}
