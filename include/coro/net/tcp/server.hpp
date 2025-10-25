#pragma once

#include "coro/fd.hpp"
#include "coro/net/ip_address.hpp"
#include "coro/net/socket.hpp"
#include "coro/net/tcp/client.hpp"
#include "coro/task.hpp"
#include "coro/when_any.hpp"

#include <array>

#include <fcntl.h>
#include <sys/socket.h>

namespace coro
{
class io_scheduler;
} // namespace coro

namespace coro::net::tcp
{

class server
{
public:
    struct options
    {
        /// The ip address for the tcp server to bind and listen on.
        net::ip_address address{net::ip_address::from_string("0.0.0.0")};
        /// The port for the tcp server to bind and listen on.
        uint16_t port{8080};
        /// The kernel backlog of connections to buffer.
        int32_t backlog{128};
    };

    explicit server(
        std::shared_ptr<io_scheduler> scheduler,
        options                       opts = options{
                                  .address = net::ip_address::from_string("0.0.0.0"),
                                  .port    = 8080,
                                  .backlog = 128,
        });

    server(const server&) = delete;
    server(server&& other);
    auto operator=(const server&) -> server& = delete;
    auto operator=(server&& other) -> server&;
    ~server() = default;

    /**
     * Polls for new incoming tcp connections.
     * @param timeout How long to wait for a new connection before timing out, zero waits indefinitely.
     * @return The result of the poll, 'event' means the poll was successful and there is at least 1
     *         connection ready to be accepted.
     */
    auto poll(std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) -> coro::task<coro::poll_status>
    {
        // return m_io_scheduler->poll(m_accept_socket, coro::poll_op::read, timeout);

        // struct multi_poll_awaiter
        // {
        //     explicit multi_poll_awaiter(poll_info& pa, poll_info& pb) noexcept : m_pa(pa), m_pb(pb) {}

        //     // ~poll_awaiter() { std::cerr << "~poll_awaiter on " << m_pi.m_fd << std::endl; }

        //     auto await_ready() const noexcept -> bool { return false; }
        //     auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> void
        //     {
        //         m_pi.m_awaiting_coroutine = awaiting_coroutine;
        //         std::atomic_thread_fence(std::memory_order::release);
        //     }
        //     auto await_resume() noexcept -> coro::poll_status { return m_pi.m_poll_status; }

        //     detail::poll_info& m_pa;
        //     detail::poll_info& m_pb;
        // };

        // TODO
        // Remove the reference to io_scheduler.m_size from poll_info
        // Add new struct canceable_poll_info that is stoppable from the outside (how?)
        //  -> Stop_token: How to signal in co_await that the stop token got triggered?
        //  -> io_scheduler::poll_canceable() should take a stop_token?

        auto poll_accept   = m_io_scheduler->poll(m_accept_socket, coro::poll_op::read, timeout);
        auto poll_shutdown = m_io_scheduler->poll(m_shutdown_fd[0], coro::poll_op::read);
        auto result        = co_await coro::when_any(std::move(poll_accept), std::move(poll_shutdown));
        std::cerr << "KEKEKKEK" << m_accept_socket.native_handle() << " -- " << m_shutdown_fd[1] << std::endl;
        if (result.index() == 0)
        {
            std::cerr << "Index 0 with " << static_cast<int>(std::get<0>(result)) << std::endl;
            co_return std::get<0>(result);
        }
        else
        {
            // Got read event from the shutdown control file descriptor so we return closed event
            std::cerr << "Index 1 with " << static_cast<int>(std::get<1>(result)) << std::endl;
            co_return coro::poll_status::closed;
        }
    }

    /**
     * Accepts an incoming tcp client connection.  On failure the tls clients socket will be set to
     * and invalid state, use the socket.is_valid() to verify the client was correctly accepted.
     * @return The newly connected tcp client connection.
     */
    auto accept() -> coro::net::tcp::client;

    /**
     * @return The tcp accept socket this server is using.
     * @{
     **/
    [[nodiscard]] auto accept_socket() -> net::socket& { return m_accept_socket; }
    [[nodiscard]] auto accept_socket() const -> const net::socket& { return m_accept_socket; }
    /** @} */

    auto shutdown() -> void
    {
        // Shutdown the inner socket
        m_accept_socket.shutdown(coro::poll_op::read_write);

        std::cerr << "shutdown on " << m_shutdown_fd[0] << " and " << m_shutdown_fd[1] << " to "
                  << m_accept_socket.native_handle() << std::endl;

        // Signal the event loop to stop asap, triggering the event fd is safe.
        const int value{1};
        ::write(m_shutdown_fd[1], reinterpret_cast<const void*>(&value), sizeof(value));
    }

private:
    friend client;
    /// The io scheduler for awaiting new connections.
    std::shared_ptr<io_scheduler> m_io_scheduler{nullptr};
    /// The bind and listen options for this server.
    options m_options;
    /// The socket for accepting new tcp connections on.
    net::socket m_accept_socket{-1};

    std::array<fd_t, 2> m_shutdown_fd{-1};
};

} // namespace coro::net::tcp
