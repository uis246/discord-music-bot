#ifndef ASIO_HTTP_REQUEST_H
#define ASIO_HTTP_REQUEST_H

#include <net/http_response.h>
#include <boost/asio.hpp>
#include <string>

class http_request
{
public:
    http_request(boost::asio::io_context &io, const std::string &host, int port,
                 const std::string &resource);

private:
    boost::asio::ip::tcp::socket sock;
    std::string resource;
    std::string host;

    size_t bytes_to_write;

    std::unique_ptr<http_response> response;

    void on_connect(const boost::system::error_code &e);
    void on_write(const boost::system::error_code &e, size_t wrote);
};

#endif
