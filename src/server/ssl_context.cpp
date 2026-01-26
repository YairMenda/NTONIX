/**
 * NTONIX - High-Performance AI Inference Gateway
 * SSL Context implementation - Manages SSL/TLS configuration and certificate loading
 */

#include "server/ssl_context.hpp"

#include <spdlog/spdlog.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ntonix::server {

namespace {

/**
 * Get OpenSSL error string
 */
std::string get_ssl_error_string() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return std::string(buf);
}

/**
 * Check if file exists and is readable
 */
bool file_readable(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream file(path);
    return file.good();
}

} // anonymous namespace

SslContextManager::SslContextManager(const SslConfig& config)
    : default_context_(ssl::context::tlsv12)  // Will be updated in configure_context
    , key_password_(config.key_password)
{
    try {
        configure_context(default_context_, config);
        valid_ = true;
        spdlog::info("SSL context initialized successfully");
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize SSL context: {}", e.what());
        throw;
    }
}

ssl::context& SslContextManager::get_context() noexcept {
    return default_context_;
}

const ssl::context& SslContextManager::get_context() const noexcept {
    return default_context_;
}

void SslContextManager::add_sni_context(const std::string& server_name, const SslConfig& config) {
    auto ctx = std::make_unique<ssl::context>(ssl::context::tlsv12);
    configure_context(*ctx, config);
    sni_contexts_.emplace_back(server_name, std::move(ctx));
    spdlog::info("Added SNI context for hostname: {}", server_name);
}

void SslContextManager::set_sni_callback(SniCallback callback) {
    sni_callback_ = std::move(callback);
}

bool SslContextManager::is_valid() const noexcept {
    return valid_;
}

std::string SslContextManager::get_certificate_subject() {
    // Access the underlying OpenSSL context to get certificate info
    SSL_CTX* ctx = default_context_.native_handle();
    X509* cert = SSL_CTX_get0_certificate(ctx);
    if (!cert) {
        return "(no certificate loaded)";
    }

    char buf[256];
    X509_NAME* subject = X509_get_subject_name(cert);
    X509_NAME_oneline(subject, buf, sizeof(buf));
    return std::string(buf);
}

std::string SslContextManager::get_certificate_expiry() {
    SSL_CTX* ctx = default_context_.native_handle();
    X509* cert = SSL_CTX_get0_certificate(ctx);
    if (!cert) {
        return "(no certificate loaded)";
    }

    const ASN1_TIME* not_after = X509_get0_notAfter(cert);
    if (!not_after) {
        return "(unknown)";
    }

    // Convert ASN1_TIME to string
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return "(error)";
    }

    ASN1_TIME_print(bio, not_after);
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    std::string result(data, static_cast<std::size_t>(len));
    BIO_free(bio);

    return result;
}

void SslContextManager::configure_context(ssl::context& ctx, const SslConfig& config) {
    // Configure TLS versions
    configure_tls_versions(ctx, config);

    // Load certificate and key
    load_certificate(ctx, config);

    // Configure cipher suites
    configure_ciphers(ctx, config);

    // Set default verification mode for server (we don't verify clients by default)
    ctx.set_verify_mode(ssl::verify_none);

    // Enable session caching for performance
    if (config.enable_session_cache) {
        SSL_CTX* ssl_ctx = ctx.native_handle();
        SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ssl_ctx, static_cast<long>(config.session_cache_size));
        spdlog::debug("SSL session cache enabled (size={})", config.session_cache_size);
    }

    // Load DH parameters if specified
    if (!config.dh_file.empty() && file_readable(config.dh_file)) {
        ctx.use_tmp_dh_file(config.dh_file.string());
        spdlog::debug("Loaded DH parameters from: {}", config.dh_file.string());
    }

    // Setup SNI callback
    setup_sni_callback(ctx);
}

void SslContextManager::load_certificate(ssl::context& ctx, const SslConfig& config) {
    // Validate file paths
    if (config.cert_file.empty()) {
        throw std::runtime_error("SSL certificate file path is empty");
    }
    if (config.key_file.empty()) {
        throw std::runtime_error("SSL private key file path is empty");
    }

    if (!file_readable(config.cert_file)) {
        throw std::runtime_error("Cannot read SSL certificate file: " + config.cert_file.string());
    }
    if (!file_readable(config.key_file)) {
        throw std::runtime_error("Cannot read SSL private key file: " + config.key_file.string());
    }

    // Set password callback if password is provided
    if (!config.key_password.empty()) {
        ctx.set_password_callback(
            [password = config.key_password](std::size_t max_length, ssl::context::password_purpose purpose) {
                return password_callback(max_length, purpose, password);
            }
        );
    }

    // Load certificate chain file
    boost::system::error_code ec;
    ctx.use_certificate_chain_file(config.cert_file.string(), ec);
    if (ec) {
        throw std::runtime_error("Failed to load certificate: " + config.cert_file.string() +
                                " - " + ec.message() + " (" + get_ssl_error_string() + ")");
    }
    spdlog::info("Loaded SSL certificate from: {}", config.cert_file.string());

    // Load private key file
    ctx.use_private_key_file(config.key_file.string(), ssl::context::pem, ec);
    if (ec) {
        throw std::runtime_error("Failed to load private key: " + config.key_file.string() +
                                " - " + ec.message() + " (" + get_ssl_error_string() + ")");
    }
    spdlog::info("Loaded SSL private key from: {}", config.key_file.string());

    // Load CA certificate if provided (for certificate chain)
    if (!config.ca_file.empty() && file_readable(config.ca_file)) {
        ctx.load_verify_file(config.ca_file.string(), ec);
        if (ec) {
            spdlog::warn("Failed to load CA certificate: {} - {}",
                        config.ca_file.string(), ec.message());
        } else {
            spdlog::info("Loaded CA certificate from: {}", config.ca_file.string());
        }
    }
}

void SslContextManager::configure_tls_versions(ssl::context& ctx, const SslConfig& config) {
    // Start with all SSLv23 options (includes TLS), then restrict
    ssl::context::options opts = ssl::context::default_workarounds |
                                 ssl::context::no_sslv2 |
                                 ssl::context::no_sslv3;

    // Configure minimum TLS version
    SSL_CTX* ssl_ctx = ctx.native_handle();

    if (config.enable_tls_1_2 && config.enable_tls_1_3) {
        // Both enabled - set minimum to TLS 1.2
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
        spdlog::info("SSL: Enabled TLS 1.2 and TLS 1.3");
    } else if (config.enable_tls_1_3) {
        // TLS 1.3 only
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
        spdlog::info("SSL: Enabled TLS 1.3 only");
    } else if (config.enable_tls_1_2) {
        // TLS 1.2 only
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_2_VERSION);
        spdlog::info("SSL: Enabled TLS 1.2 only");
    } else {
        throw std::runtime_error("At least one TLS version must be enabled");
    }

    // Disable older TLS versions explicitly
    opts |= ssl::context::no_tlsv1;
    opts |= ssl::context::no_tlsv1_1;

    // Single-DH-use for forward secrecy
    opts |= ssl::context::single_dh_use;

    ctx.set_options(opts);
}

void SslContextManager::configure_ciphers(ssl::context& ctx, const SslConfig& config) {
    SSL_CTX* ssl_ctx = ctx.native_handle();

    // Configure TLS 1.2 cipher list
    std::string cipher_list = config.cipher_list;
    if (cipher_list.empty()) {
        // Default secure cipher list for TLS 1.2
        cipher_list = "ECDHE+AESGCM:DHE+AESGCM:ECDHE+CHACHA20:DHE+CHACHA20:"
                      "ECDHE+AES256:DHE+AES256:ECDHE+AES128:DHE+AES128:"
                      "!aNULL:!eNULL:!EXPORT:!DES:!RC4:!3DES:!MD5:!PSK";
    }

    if (SSL_CTX_set_cipher_list(ssl_ctx, cipher_list.c_str()) != 1) {
        spdlog::warn("Failed to set TLS 1.2 cipher list, using defaults");
    } else {
        spdlog::debug("TLS 1.2 cipher list: {}", cipher_list);
    }

    // Configure TLS 1.3 ciphersuites (if supported)
#ifdef TLS1_3_VERSION
    std::string ciphersuites = config.ciphersuites;
    if (ciphersuites.empty()) {
        // Default secure ciphersuites for TLS 1.3
        ciphersuites = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:"
                       "TLS_AES_128_GCM_SHA256";
    }

    if (SSL_CTX_set_ciphersuites(ssl_ctx, ciphersuites.c_str()) != 1) {
        spdlog::warn("Failed to set TLS 1.3 ciphersuites, using defaults");
    } else {
        spdlog::debug("TLS 1.3 ciphersuites: {}", ciphersuites);
    }
#endif
}

void SslContextManager::setup_sni_callback(ssl::context& ctx) {
    SSL_CTX* ssl_ctx = ctx.native_handle();

    // Set callback user data to this object
    SSL_CTX_set_tlsext_servername_arg(ssl_ctx, this);

    // Set SNI callback
    SSL_CTX_set_tlsext_servername_callback(ssl_ctx, ssl_sni_callback);

    spdlog::debug("SNI callback configured");
}

std::string SslContextManager::password_callback(
    std::size_t max_length,
    ssl::context::password_purpose /*purpose*/,
    const std::string& password)
{
    if (password.length() > max_length) {
        return password.substr(0, max_length);
    }
    return password;
}

int SslContextManager::ssl_sni_callback(SSL* ssl, int* alert, void* arg) {
    auto* manager = static_cast<SslContextManager*>(arg);
    if (!manager) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    // Get the server name from the ClientHello
    const char* server_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!server_name) {
        // No SNI provided, use default context
        return SSL_TLSEXT_ERR_OK;
    }

    std::string hostname(server_name);
    spdlog::debug("SNI: Client requested hostname: {}", hostname);

    // First try custom callback
    if (manager->sni_callback_) {
        SniResult result = manager->sni_callback_(hostname);
        if (result.found && result.context) {
            SSL_set_SSL_CTX(ssl, result.context->native_handle());
            spdlog::debug("SNI: Using custom callback context for: {}", hostname);
            return SSL_TLSEXT_ERR_OK;
        }
    }

    // Then try configured SNI contexts
    for (const auto& [name, ctx] : manager->sni_contexts_) {
        if (name == hostname) {
            SSL_set_SSL_CTX(ssl, ctx->native_handle());
            spdlog::debug("SNI: Using configured context for: {}", hostname);
            return SSL_TLSEXT_ERR_OK;
        }
    }

    // No match found, use default context (already set)
    spdlog::debug("SNI: Using default context for: {}", hostname);
    return SSL_TLSEXT_ERR_OK;
}

std::unique_ptr<SslContextManager> create_ssl_context(
    const std::filesystem::path& cert_file,
    const std::filesystem::path& key_file)
{
    SslConfig config;
    config.cert_file = cert_file;
    config.key_file = key_file;
    config.enable_tls_1_2 = true;
    config.enable_tls_1_3 = true;

    return std::make_unique<SslContextManager>(config);
}

} // namespace ntonix::server
