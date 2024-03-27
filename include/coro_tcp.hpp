#pragma once

#include <any>
#include <atomic>
#include <cstddef>
#include <format>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include <spdlog/spdlog.h>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/system/detail/error_code.hpp>
#include "boost/system/detail/error_code.hpp"

constexpr auto use_nothrow_awaitable = boost::asio::as_tuple(boost::asio::use_awaitable);
using CORO_ERROR_CODE = boost::system::error_code;

class IoContextPool final {
public:
    explicit IoContextPool(std::size_t);

    void start();
    void stop();

    boost::asio::io_context& getIoContext();

private:
    std::vector<std::shared_ptr<boost::asio::io_context>> m_io_contexts;
    std::list<boost::asio::any_io_executor> m_work;
    std::atomic_uint64_t m_next_io_context;
    std::vector<std::jthread> m_threads;
};

inline IoContextPool::IoContextPool(std::size_t pool_size)
    : m_next_io_context(0) {
    if (pool_size == 0)
        throw std::runtime_error("IoContextPool size is 0");
    for (std::size_t i = 0; i < pool_size; ++i) {
        auto io_context_ptr = std::make_shared<boost::asio::io_context>();
        m_io_contexts.emplace_back(io_context_ptr);
        m_work.emplace_back(
            boost::asio::require(io_context_ptr->get_executor(), boost::asio::execution::outstanding_work.tracked));
    }
}

inline void IoContextPool::start() {
    for (auto& context : m_io_contexts)
        m_threads.emplace_back(std::jthread([&] { context->run(); }));
}

inline void IoContextPool::stop() {
    for (auto& context_ptr : m_io_contexts)
        context_ptr->stop();
}

inline boost::asio::io_context& IoContextPool::getIoContext() {
    size_t index = m_next_io_context.fetch_add(1, std::memory_order_relaxed);
    boost::asio::io_context& io_context = *m_io_contexts[index % m_io_contexts.size()];
    ++m_next_io_context;
    if (m_next_io_context == m_io_contexts.size())
        m_next_io_context = 0;
    return io_context;
}

inline boost::asio::awaitable<void> timeout(std::chrono::seconds duration) {
    auto now = std::chrono::steady_clock::now() + duration;
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    timer.expires_at(now);
    [[maybe_unused]] auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
    co_return;
}

class ScopeExit {
public:
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    template <typename Callable>
    explicit ScopeExit(Callable&& call)
        : m_call(std::forward<Callable>(call)) {}

    ~ScopeExit() {
        if (m_call)
            m_call();
    }

    void clear() { m_call = decltype(m_call)(); }

private:
    std::function<void()> m_call;
};

struct Session {
    std::any data;
};

struct Config {
    std::string ip;
    short port;
    bool open_keep_alive;
    bool open_tcp_no_delay;
    std::optional<int32_t> send_buffer_size;
    std::optional<int32_t> receive_buffer_size;
};

using Variant = std::variant<std::vector<std::shared_ptr<std::string>>, std::shared_ptr<std::string>>;
using ConcurrentChannel = boost::asio::experimental::concurrent_channel<void(CORO_ERROR_CODE, Variant)>;

inline boost::asio::awaitable<void> sleep(const std::chrono::seconds& duration) {
    auto now = std::chrono::steady_clock::now() + duration;
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    timer.expires_at(now);
    [[maybe_unused]] auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
    co_return;
}

class CoroTcpServer final {
public:
    CoroTcpServer(const Config& config, std::size_t thread_pool_num, std::chrono::seconds timeout)
        : m_config(config)
        , m_pool(thread_pool_num)
        , m_timeout(std::move(timeout))
        , m_channel_pool(2)
        , m_acceptor(m_accept_pool.getIoContext()) {
        m_pool.start();
        m_channel_pool.start();
        m_accept_pool.start();
    }
    ~CoroTcpServer() {
        if (m_stop)
            return;
        stop();
    }

    void start(auto cb) {
        auto& context = m_accept_pool.getIoContext();
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string(m_config.ip), m_config.port);
        m_acceptor.open(endpoint.protocol());
        CORO_ERROR_CODE ec;
        m_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        m_acceptor.bind(endpoint);

        m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
            throw std::runtime_error(ec.message());

        boost::asio::co_spawn(context, start_accept(std::move(cb)), boost::asio::detached);
    }

    void stop() {
        std::promise<void> done;
        boost::asio::post(m_accept_pool.getIoContext(), [&] {
            m_acceptor.close();
            done.set_value();
        });
        done.get_future().wait();
        m_accept_pool.stop();
        m_pool.stop();
        m_channel_pool.stop();
        m_stop = true;
    }

private:
    boost::asio::awaitable<void> send(
        auto start_time_point,
        std::shared_ptr<boost::asio::ip::tcp::socket> socket,
        std::shared_ptr<ConcurrentChannel> channel) {
        for (;;) {
            Variant var;
            if (!channel->try_receive([&](auto, auto&& d) { var = std::forward<decltype(d)>(d); })) {
                auto [ec, ptr] = co_await channel->async_receive(use_nothrow_awaitable);
                if (ec) {
                    break;
                }
                var = std::move(ptr);
            }
            if (std::holds_alternative<std::shared_ptr<std::string>>(var)) [[likely]] {
                auto& str_ptr = std::get<std::shared_ptr<std::string>>(var);
                auto& str = *str_ptr;
                auto [ec, count] = co_await boost::asio::async_write(
                    *socket, boost::asio::buffer(str.c_str(), str.size()), use_nothrow_awaitable);
                if (ec || count != str.size()) {
                    break;
                }
            }
            else {
                auto& vec_str = std::get<std::vector<std::shared_ptr<std::string>>>(var);
                for (auto& str_ptr : vec_str) {
                    auto& str = *str_ptr;
                    auto [ec, count] = co_await boost::asio::async_write(
                        *socket, boost::asio::buffer(str.c_str(), str.size()), use_nothrow_awaitable);
                    if (ec || count != str.size()) {
                        break;
                    }
                }
            }
            *start_time_point = std::chrono::steady_clock::now();
        }
        co_return;
    }

    boost::asio::awaitable<void> recv(auto start_time_point,
        std::shared_ptr<boost::asio::ip::tcp::socket> socket_ptr,
        std::shared_ptr<ConcurrentChannel> channel,
        std::shared_ptr<std::any> any,
        const auto& cb) {
        char data[1024];
        for (;;) {
            auto [ec, length] = co_await socket_ptr->async_read_some(boost::asio::buffer(data, 1024), use_nothrow_awaitable);
            *start_time_point = std::chrono::steady_clock::now();
            if (ec) {
                spdlog::info("{}", ec.message());
                break;
            }
            if (auto ret = co_await cb(std::string_view{data, length}, channel, any); !ret)
                break;
        }
        co_return;
    }

    boost::asio::awaitable<void> check_timeout(auto start_time_point, auto channel) {
        for (;;) {
            if (!channel->is_open())
                break;
            co_await sleep(m_timeout);
            auto now = std::chrono::steady_clock::now();
            if (now - *start_time_point > m_timeout)
                break;
        }
        co_return;
    }

    boost::asio::awaitable<void> proc(auto socket, auto&, const auto& cb) {
        using namespace boost::asio::experimental::awaitable_operators;
        auto channel = std::make_shared<ConcurrentChannel>(m_channel_pool.getIoContext(), 400000);
        ScopeExit auto_exit{[&] {
            CORO_ERROR_CODE ec;
            socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket->close(ec);
            if (channel->is_open())
                channel->close();
        }};
        auto any = std::make_shared<std::any>();
        auto now = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        co_await (recv(now, socket, channel, any, cb) ||
                  send(now, socket, channel) ||
                  check_timeout(now, channel));
        co_return;
    }

    boost::asio::awaitable<void> start_accept(auto cb) {
        for (;;) {
            auto& context = m_pool.getIoContext();
            auto socket = std::make_shared<boost::asio::ip::tcp::socket>(context);
            if (auto [ec] = co_await m_acceptor.async_accept(*socket, use_nothrow_awaitable); ec) {
                if (ec == boost::asio::error::operation_aborted)
                    break;
                continue;
            }
            std::stringstream ss;
            CORO_ERROR_CODE ec{};
            auto endpoint = socket->remote_endpoint(ec);
            if (!ec) {
                ss << endpoint;
                spdlog::info("new connection from: [{}]", ss.str());
            }
            auto set_socket_attribute = [&](auto attribute) {
                CORO_ERROR_CODE ec;
                socket->set_option(attribute, ec);
                if (ec)
                    spdlog::error("[{}]", ec.message());
            };
            if (m_config.open_keep_alive)
                set_socket_attribute(boost::asio::socket_base::keep_alive(true));
            if (m_config.open_tcp_no_delay)
                set_socket_attribute(boost::asio::ip::tcp::no_delay(true));
            if (m_config.receive_buffer_size)
                set_socket_attribute(
                    boost::asio::socket_base::receive_buffer_size{m_config.receive_buffer_size.value()});
            if (m_config.send_buffer_size)
                set_socket_attribute(boost::asio::socket_base::send_buffer_size{m_config.send_buffer_size.value()});
            boost::asio::co_spawn(context, proc(std::move(socket), context, cb), [](std::exception_ptr eptr) {
                try {
                    if (eptr)
                        std::rethrow_exception(eptr);
                } catch (const std::exception& e) {
                    spdlog::error("Caught exception: {}", e.what());
                }
            });
        }
    }

    Config m_config;
    IoContextPool m_pool;
    std::chrono::seconds m_timeout;
    IoContextPool m_accept_pool{1};
    IoContextPool m_channel_pool{2};
    boost::asio::ip::tcp::acceptor m_acceptor;
    bool m_stop{false};
};
