/**
 * NTONIX - High-Performance AI Inference Gateway
 *
 * A C++20 reverse proxy designed to optimize local LLM cluster infrastructure.
 */

#include "server/server.hpp"

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <iostream>

namespace asio = boost::asio;

int main(int argc, char* argv[]) {
    // Set log level (DEBUG for development)
    spdlog::set_level(spdlog::level::debug);

    spdlog::info("NTONIX AI Inference Gateway v0.1.0");
    spdlog::info("Starting server...");

    try {
        // Configure server
        ntonix::server::ServerConfig config;
        config.port = 8080;
        config.thread_count = std::max(1u, std::thread::hardware_concurrency());
        config.bind_address = "0.0.0.0";

        spdlog::info("Configuration: port={}, threads={}, bind={}",
                    config.port, config.thread_count, config.bind_address);

        // Create and start server
        ntonix::server::Server server(config);

        // Simple echo handler for testing - logs connection and echoes back
        server.start([](asio::ip::tcp::socket socket) {
            // For now, just log that we got a connection
            // Full HTTP handling will come in US-003
            boost::system::error_code ec;
            auto remote = socket.remote_endpoint(ec);
            if (!ec) {
                spdlog::debug("Handler: Processing connection from {}:{}",
                             remote.address().to_string(), remote.port());
            }

            // Simple test response - write something back to verify socket works
            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 35\r\n"
                "Connection: close\r\n"
                "\r\n"
                "NTONIX Gateway - Connection OK!\r\n";

            asio::write(socket, asio::buffer(response), ec);
            if (ec) {
                spdlog::debug("Handler: Write error: {}", ec.message());
            }

            socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        });

        spdlog::info("Server started successfully");
        spdlog::info("Press Ctrl+C to stop");

        // Wait for shutdown (blocks until signal received)
        server.wait();

        spdlog::info("Server stopped gracefully");
        return 0;

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return 1;
    }
}
