#pragma once

#include <atomic>
#include <future>
#include <list>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

constexpr auto use_nothrow_awaitable = boost::asio::as_tuple(boost::asio::use_awaitable);

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

inline IoContextPool::IoContextPool(std::size_t pool_size) : m_next_io_context(0) {
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
    explicit ScopeExit(Callable&& call) : m_call(std::forward<Callable>(call)) {}

    ~ScopeExit() {
        if (m_call)
            m_call();
    }

    void clear() { m_call = decltype(m_call)(); }

private:
    std::function<void()> m_call;
};

class CoroTcpServer final {
public:
    CoroTcpServer(const std::string& host, const std::string& port, std::size_t thread_pool_num)
        : m_host(host), m_port(port), m_pool(thread_pool_num), m_acceptor(m_accept_pool.getIoContext()) {
        m_pool.start();
        m_accept_pool.start();
    }
    ~CoroTcpServer() {
        m_accept_pool.stop();
        m_pool.stop();
    }

    void start(std::function<boost::asio::awaitable<void>(std::shared_ptr<boost::asio::ip::tcp::socket>,
                                                          boost::asio::io_context&)>
                   cb) {
        auto& context = m_accept_pool.getIoContext();
        boost::asio::ip::tcp::resolver resolver(context);
        boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(m_host, m_port).begin();
        m_acceptor.open(endpoint.protocol());
        boost::system::error_code ec;
        m_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        m_acceptor.bind(endpoint);

        m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
            throw std::runtime_error(ec.message());

        boost::asio::co_spawn(context, startAccept(std::move(cb)), boost::asio::detached);
    }

    void stop() {
        std::promise<void> done;
        boost::asio::post(m_accept_pool.getIoContext(), [&] {
            m_acceptor.close();
            done.set_value();
        });
        done.get_future().wait();
    }

private:
    boost::asio::awaitable<void> startAccept(auto cb) {
        for (;;) {
            auto& context = m_pool.getIoContext();
            auto socket_ptr = std::make_shared<boost::asio::ip::tcp::socket>(context);
            auto [ec] = co_await m_acceptor.async_accept(*socket_ptr, use_nothrow_awaitable);
            if (ec) {
                if (ec == boost::asio::error::operation_aborted)
                    break;
                continue;
            }
            boost::asio::co_spawn(context, cb(std::move(socket_ptr), context), boost::asio::detached);
        }
    }

    std::string m_host;
    std::string m_port;
    IoContextPool m_pool;
    IoContextPool m_accept_pool{1};
    boost::asio::ip::tcp::acceptor m_acceptor;
};
