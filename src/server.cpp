#include <functional>
#include <iostream>
#include <memory>

#include <coro_tcp.hpp>

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <thread>

struct App {
    using ConcurrentChannel =
        boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::string)>;

    App() { m_pool.start(); }
    ~App() { m_pool.stop(); }

    boost::asio::awaitable<void> recv(std::shared_ptr<boost::asio::ip::tcp::socket> socket_ptr,
                                      std::shared_ptr<ConcurrentChannel> ch_ptr) {
        char data[1024];
        for (;;) {
            auto [ec, length] =
                co_await socket_ptr->async_read_some(boost::asio::buffer(data, 1024), use_nothrow_awaitable);
            if (ec) {
                std::cout << "read: " << ec.message() << std::endl;
                break;
            }
            std::cout << std::string{data, length} << std::endl;
            std::thread([=] {
                for (int i = 0; i < 5; i++)
                    ch_ptr->try_send(boost::system::error_code{}, "test");
            }).detach();
        }
    }
    boost::asio::awaitable<void> send(std::shared_ptr<boost::asio::ip::tcp::socket> socket_ptr,
                                      std::shared_ptr<ConcurrentChannel> ch_ptr) {
        for (;;) {
            std::cout << "---------sendCoro--------" << std::endl;
            std::string str;
            if (!ch_ptr->try_receive([&](boost::system::error_code, std::string msg) { str = std::move(msg); })) {
                boost::system::error_code ec;
                std::tie(ec, str) = co_await ch_ptr->async_receive(use_nothrow_awaitable);
                if (ec) {
                    std::cout << "sendCoro: " << ec.message() << std::endl;
                    break;
                }
            }
            std::cout << "---------recv --------  sendCoro--------" << std::endl;
            auto [ec, nwritten] = co_await boost::asio::async_write(
                *socket_ptr, boost::asio::buffer(str.c_str(), str.size()), use_nothrow_awaitable);
            if (ec || nwritten != str.size()) {
                std::cout << ec.message() << std::endl;
                break;
            }
        }
        co_return;
    }

    boost::asio::awaitable<void> proc(std::shared_ptr<boost::asio::ip::tcp::socket> socket_ptr,
                                      boost::asio::io_context&) {
        using namespace boost::asio::experimental::awaitable_operators;
        auto channel_ptr = std::make_shared<ConcurrentChannel>(m_pool.getIoContext(), 50000);
        ScopeExit auto_exit{[=] {
            boost::system::error_code ec;
            socket_ptr->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            if (channel_ptr->is_open())
                channel_ptr->close();
        }};
        co_await (recv(socket_ptr, channel_ptr) || send(socket_ptr, channel_ptr));
        co_return;
    }

    IoContextPool m_pool{10};
};

int main() {
    App app;
    CoroTcpServer coro_tcp_server{"0.0.0.0", "8858", 8};
    coro_tcp_server.start(std::bind_front(&App::proc, &app));
    sleep(100);
    coro_tcp_server.stop();
    return 0;
}
