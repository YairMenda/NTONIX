# ADR-005: Configuration Hierarchy

## Status
Accepted

## Context
NTONIX needs flexible configuration to support different deployment scenarios:
- Development: Quick iteration with command-line overrides
- Docker: Environment variable configuration
- Production: Configuration files with hot-reload support

The configuration system must handle:
- Multiple configuration sources with clear precedence
- Validation and sensible defaults
- Hot-reload for operational changes without restart
- Clear error messages for misconfiguration

## Decision
We implement a **layered configuration system** with the following precedence (highest to lowest):

1. Command-line arguments
2. Environment variables (NTONIX_*)
3. Configuration file (JSON)
4. Default values

### Alternatives Considered

1. **Single Configuration Source**
   - Pros: Simple, no precedence confusion
   - Cons: Inflexible for different deployment scenarios

2. **YAML Configuration**
   - Pros: More readable for complex configs
   - Cons: Additional dependency, JSON sufficient for our needs

3. **Environment-only (12-Factor)**
   - Pros: Container-native
   - Cons: Complex nested structures difficult to express

### Why This Hierarchy

This follows established patterns from NGINX, Redis, and PostgreSQL:

- **CLI overrides everything**: Useful for testing and debugging
- **Environment for containers**: Docker/K8s native configuration
- **File for complex configs**: Structured backend lists, nested settings
- **Defaults for safety**: Always starts with sane configuration

## Implementation

### Configuration Loading Flow

```cpp
bool ConfigManager::load(int argc, char* argv[]) {
    // 1. Start with defaults
    config_ = Config{};  // Default-initialized

    // 2. Load from file (if specified via CLI or env)
    if (auto path = find_config_path(argc, argv)) {
        load_from_file(*path);
    }

    // 3. Apply environment overrides
    apply_environment_overrides();

    // 4. Apply CLI overrides (highest precedence)
    apply_cli_overrides(argc, argv);

    // 5. Validate final configuration
    config_.validate();

    return true;
}
```

### Environment Variable Mapping

| Environment Variable | Config Field |
|---------------------|--------------|
| `NTONIX_PORT` | `server.port` |
| `NTONIX_SSL_PORT` | `server.ssl_port` |
| `NTONIX_THREADS` | `server.threads` |
| `NTONIX_BACKENDS` | `backends` (comma-separated) |
| `NTONIX_CACHE_ENABLED` | `cache.enabled` |
| `NTONIX_CACHE_SIZE_MB` | `cache.max_size_mb` |
| `NTONIX_CACHE_TTL` | `cache.ttl_seconds` |
| `NTONIX_LOG_LEVEL` | `logging.level` |

### Hot-Reload Support

Only the backend list supports hot-reload via SIGHUP:

```cpp
void ConfigManager::reload() {
    if (config_path_.empty()) return;

    // Reload file
    auto new_config = load_from_file(config_path_);

    // Apply stored CLI overrides (preserve precedence)
    if (cli_port_) new_config.server.port = *cli_port_;
    // ... other CLI overrides

    // Only update backends (other settings require restart)
    std::lock_guard lock(mutex_);
    config_.backends = std::move(new_config.backends);

    // Notify listeners
    for (auto& callback : reload_callbacks_) {
        callback(config_.backends);
    }
}
```

Why only backends? Other settings (ports, threads, SSL certs) require restart to take effect safely.

## Consequences

### Positive
- Flexible deployment across environments
- Clear precedence rules
- Hot-reload reduces downtime
- Familiar pattern for operators

### Negative
- Multiple sources can cause confusion
- Debug requires checking all sources

### Error Handling
- Invalid JSON: Clear error with file path and parse error
- Missing required fields: Validation error listing missing fields
- Invalid values: Range/type validation with descriptive messages

## Usage Examples

### Development
```bash
./ntonix --port 9000 --backends localhost:8001,localhost:8002
```

### Docker
```yaml
services:
  ntonix:
    environment:
      - NTONIX_PORT=8080
      - NTONIX_BACKENDS=backend-1:8001,backend-2:8001
      - NTONIX_CACHE_ENABLED=true
```

### Production
```json
// /etc/ntonix/config.json
{
  "server": {"port": 8080, "threads": 8},
  "backends": [
    {"host": "llm-1.internal", "port": 8001, "weight": 2},
    {"host": "llm-2.internal", "port": 8001, "weight": 1}
  ],
  "cache": {"enabled": true, "max_size_mb": 2048}
}
```

```bash
./ntonix --config /etc/ntonix/config.json
```

### Hot-Reload
```bash
# Update backends in config file
vim /etc/ntonix/config.json

# Trigger reload without restart
kill -HUP $(pgrep ntonix)
```
