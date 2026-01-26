/**
 * NTONIX - High-Performance AI Inference Gateway
 * SSL Context - Manages SSL/TLS configuration and certificate loading
 */

#ifndef NTONIX_SERVER_SSL_CONTEXT_HPP
#define NTONIX_SERVER_SSL_CONTEXT_HPP

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ntonix::server {

namespace asio = boost::asio;
namespace ssl = asio::ssl;

/**
 * SSL/TLS configuration
 */
struct SslConfig {
    std::filesystem::path cert_file;        // Server certificate file (PEM format)
    std::filesystem::path key_file;         // Private key file (PEM format)
    std::filesystem::path ca_file;          // Optional: CA certificate for chain (PEM format)
    std::string key_password;               // Optional: Password for encrypted private key

    // TLS version settings
    bool enable_tls_1_2{true};              // Enable TLS 1.2
    bool enable_tls_1_3{true};              // Enable TLS 1.3

    // Optional: Cipher suite configuration (OpenSSL cipher string format)
    std::string cipher_list;                // For TLS 1.2 (e.g., "HIGH:!aNULL:!MD5")
    std::string ciphersuites;               // For TLS 1.3 (e.g., "TLS_AES_256_GCM_SHA384")

    // Session caching
    bool enable_session_cache{true};
    std::size_t session_cache_size{20480};  // Number of cached sessions

    // Optional: DH parameters file for DHE ciphers
    std::filesystem::path dh_file;
};

/**
 * SNI (Server Name Indication) callback result
 */
struct SniResult {
    bool found{false};
    ssl::context* context{nullptr};  // Context to use for this server name
};

/**
 * SNI callback type - returns context for a given server name
 * Return empty optional to use default context
 */
using SniCallback = std::function<SniResult(const std::string& server_name)>;

/**
 * SSL Context Manager - handles SSL certificate loading and TLS configuration
 *
 * Supports:
 * - TLS 1.2 and TLS 1.3
 * - Certificate and private key loading from files
 * - SNI (Server Name Indication) for multiple hostnames
 * - Session caching for performance
 */
class SslContextManager {
public:
    /**
     * Create SSL context manager with default context
     * @param config SSL configuration for the default context
     * @throws std::runtime_error if certificate/key loading fails
     */
    explicit SslContextManager(const SslConfig& config);

    ~SslContextManager() = default;

    // Non-copyable but movable
    SslContextManager(const SslContextManager&) = delete;
    SslContextManager& operator=(const SslContextManager&) = delete;
    SslContextManager(SslContextManager&&) = default;
    SslContextManager& operator=(SslContextManager&&) = default;

    /**
     * Get the default SSL context for accepting connections
     */
    ssl::context& get_context() noexcept;

    /**
     * Get const reference to default SSL context
     */
    const ssl::context& get_context() const noexcept;

    /**
     * Add an additional context for a specific server name (SNI support)
     * @param server_name The SNI hostname to match
     * @param config SSL configuration for this hostname
     * @throws std::runtime_error if certificate/key loading fails
     */
    void add_sni_context(const std::string& server_name, const SslConfig& config);

    /**
     * Set custom SNI callback for dynamic hostname resolution
     */
    void set_sni_callback(SniCallback callback);

    /**
     * Check if SSL context is properly initialized
     */
    bool is_valid() const noexcept;

    /**
     * Get the loaded certificate's subject name
     */
    std::string get_certificate_subject();

    /**
     * Get certificate expiry information
     */
    std::string get_certificate_expiry();

private:
    /**
     * Configure SSL context with given config
     */
    void configure_context(ssl::context& ctx, const SslConfig& config);

    /**
     * Load certificate and key files into context
     */
    void load_certificate(ssl::context& ctx, const SslConfig& config);

    /**
     * Configure TLS versions
     */
    void configure_tls_versions(ssl::context& ctx, const SslConfig& config);

    /**
     * Configure cipher suites
     */
    void configure_ciphers(ssl::context& ctx, const SslConfig& config);

    /**
     * Setup SNI callback on context
     */
    void setup_sni_callback(ssl::context& ctx);

    /**
     * Password callback for encrypted private keys
     */
    static std::string password_callback(
        std::size_t max_length,
        ssl::context::password_purpose purpose,
        const std::string& password);

    /**
     * OpenSSL SNI callback (static, uses user data to call member function)
     */
    static int ssl_sni_callback(SSL* ssl, int* alert, void* arg);

    ssl::context default_context_;
    std::vector<std::pair<std::string, std::unique_ptr<ssl::context>>> sni_contexts_;
    SniCallback sni_callback_;
    std::string key_password_;
    bool valid_{false};
};

/**
 * Create SSL context from configuration
 * Helper function for simple use cases
 *
 * @param cert_file Path to certificate file (PEM)
 * @param key_file Path to private key file (PEM)
 * @return Configured SSL context
 * @throws std::runtime_error on errors
 */
std::unique_ptr<SslContextManager> create_ssl_context(
    const std::filesystem::path& cert_file,
    const std::filesystem::path& key_file);

} // namespace ntonix::server

#endif // NTONIX_SERVER_SSL_CONTEXT_HPP
