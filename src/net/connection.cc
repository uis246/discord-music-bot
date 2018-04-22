#include <boost/asio/connect.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

#include "connection.h"
#include "errors.h"
#include "net/resource_parser.h"

discord::connection::connection(boost::asio::io_context &io, ssl::context &tls)
    : ctx{io}, resolver{ctx}, websock{ctx, tls}
{
}

void discord::connection::connect(const std::string &url, error_cb c)
{
    connect_cb = c;

    info = resource_parser::parse(url);

    auto query = tcp::resolver::query{info.host, std::to_string(info.port)};
    resolver.async_resolve(
        query, [self = shared_from_this()](auto &ec, auto it) { self->on_resolve(ec, it); });
}

void discord::connection::read(json_cb c)
{
    websock.async_read(buffer, [c, self = shared_from_this()](auto &ec, auto transferred) {
        if (ec) {
            if (!self->websock.is_open()) {
                auto close_reason = self->websock.reason();
                auto gateway_err = make_error_code(static_cast<gateway_errc>(close_reason.code));
                std::cerr << "close code: " << close_reason.code
                          << " reason: " << close_reason.reason << "\n";

                std::cerr << "[connection] gateway error: " << gateway_err.message() << "\n";
            }
            std::cerr << "[connection] error reading message: " << ec.message() << "\n";
            c({});
        } else {
            auto data = boost::beast::buffers_to_string(self->buffer.data());
            auto json = nlohmann::json::parse(data);
            c(json);
        }
        self->buffer.consume(transferred);
    });
}

void discord::connection::send(const std::string &s, transfer_cb c)
{
    // TODO: use strand + message queue + timer for delay
    auto ec = boost::system::error_code{};
    auto wrote = websock.write(boost::asio::buffer(s), ec);
    c(ec, wrote);
}

void discord::connection::on_resolve(const boost::system::error_code &ec,
                                     tcp::resolver::iterator it)
{
    if (ec) {
        connect_cb(ec);
    } else {
        boost::asio::async_connect(
            websock.next_layer().next_layer(), it,
            [self = shared_from_this()](auto &ec, auto it) { self->on_connect(ec, it); });
    }
}

void discord::connection::on_connect(const boost::system::error_code &ec, tcp::resolver::iterator)
{
    if (ec) {
        connect_cb(ec);
    } else {
        websock.next_layer().set_verify_mode(ssl::verify_peer);
        websock.next_layer().set_verify_callback(ssl::rfc2818_verification(info.host));
        websock.next_layer().async_handshake(
            ssl::stream_base::client,
            [self = shared_from_this()](auto &ec) { self->on_tls_handshake(ec); });
    }
}

void discord::connection::on_tls_handshake(const boost::system::error_code &ec)
{
    if (ec) {
        connect_cb(ec);
    } else {
        websock.async_handshake(info.host, info.resource, [self = shared_from_this()](auto &ec) {
            self->on_websocket_handshake(ec);
        });
    }
}

void discord::connection::on_websocket_handshake(const boost::system::error_code &ec)
{
    connect_cb(ec);
}
