# Open-Source Authentication Mode

This document describes the simplified authentication system for the open-source version of Endee.

## Overview

The open-source version uses a simplified authentication model:
- **Single default user** (`default`) with Admin privileges (no limits)
- **Optional token-based authentication** via `NDD_AUTH_TOKEN` environment variable
- **No multi-user management** - all admin endpoints removed

## Authentication Modes

### Mode 1: Open Mode (No Token)

When `NDD_AUTH_TOKEN` is **NOT** set:
- All APIs work without any authentication
- No `Authorization` header required
- All operations use the `default` user

```bash
# Start server without token
./ndd-avx512

# All APIs work without auth
curl http://localhost:8080/api/v1/index/list
curl -X POST http://localhost:8080/api/v1/index/create \
  -H "Content-Type: application/json" \
  -d '{"index_name": "myindex", "dim": 128, "space_type": "cosine"}'
```

### Mode 2: Token Mode

When `NDD_AUTH_TOKEN` is set:
- All protected APIs require the token in `Authorization` header
- Requests without token or with wrong token get `401 Unauthorized`

```bash
# Start server with token
export NDD_AUTH_TOKEN="my-secret-token-12345"
./ndd-avx512

# Requests require valid token
curl -H "Authorization: my-secret-token-12345" \
  http://localhost:8080/api/v1/index/list

# Without token - 401 Unauthorized
curl http://localhost:8080/api/v1/index/list
# Response: "Authorization header required"

# Wrong token - 401 Unauthorized
curl -H "Authorization: wrong-token" \
  http://localhost:8080/api/v1/index/list
# Response: "Invalid token"
```

## Configuration

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `NDD_AUTH_TOKEN` | Authentication token (empty = open mode) | `""` (empty) |
| `NDD_DATA_DIR` | Data storage directory | `/mnt/data` |
| `NDD_SERVER_PORT` | HTTP server port | `8080` |

### Token Guidelines

When setting `NDD_AUTH_TOKEN`:
- Use a strong, random token (32+ characters recommended)
- Keep the token secret - it provides full access to all operations
- Token is visible in process environment variables

Example token generation:
```bash
# Generate a random token
export NDD_AUTH_TOKEN=$(openssl rand -hex 32)
```

## API Changes

### Removed Endpoints

The following admin/multi-user endpoints were removed:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/root/token` | POST | Generate root token |
| `/api/v1/admin/users` | POST | Create user |
| `/api/v1/admin/users` | GET | List all users |
| `/api/v1/admin/users/<string>` | DELETE | Delete user |
| `/api/v1/admin/users/<string>/deactivate` | POST | Deactivate user |
| `/api/v1/admin/users/<string>/type` | PUT | Change user type |
| `/api/v1/admin/users/<string>/indices` | GET | List user indexes |
| `/api/v1/admin/indexes` | GET | List all indexes |
| `/api/v1/admin/users/<string>/indexes/<string>` | DELETE | Delete user index |
| `/api/v1/admin/users/<string>/tokens` | POST/GET | Manage user tokens |
| `/api/v1/admin/users/<string>/tokens/<string>` | DELETE | Delete user token |
| `/api/v1/admin/users/<string>/indices/<string>/reset` | POST | Reset index |
| `/api/v1/admin/users/<string>/indices/<string>/recover` | POST | Recover index |
| `/api/v1/users/<string>/type` | PUT | Set user type |
| `/api/v1/tokens` | POST/GET/DELETE | Token management |

### Available Endpoints

| Endpoint | Method | Auth Required | Description |
|----------|--------|---------------|-------------|
| `/api/v1/health` | GET | No | Health check |
| `/api/v1/stats` | GET | No | Server stats |
| `/api/v1/users/<string>/info` | GET | Yes* | Get user info (returns default user) |
| `/api/v1/users/<string>/type` | GET | Yes* | Get user type (always Admin) |
| `/api/v1/index/create` | POST | Yes* | Create index |
| `/api/v1/index/list` | GET | Yes* | List indexes |
| `/api/v1/index/<string>/delete` | DELETE | Yes* | Delete index |
| `/api/v1/index/<string>/info` | GET | Yes* | Get index info |
| `/api/v1/index/<string>/search` | POST | Yes* | Search vectors |
| `/api/v1/index/<string>/vector/insert` | POST | Yes* | Insert vectors |
| `/api/v1/index/<string>/vector/get` | POST | Yes* | Get vector |
| `/api/v1/index/<string>/vector/<string>/delete` | DELETE | Yes* | Delete vector |
| `/api/v1/index/<string>/vectors/delete` | DELETE | Yes* | Delete vectors by filter |
| `/api/v1/index/<string>/filters/update` | POST | Yes* | Update filters |
| `/api/v1/index/<string>/backup` | POST | Yes* | Create backup |
| `/api/v1/backups` | GET | Yes* | List backups |
| `/api/v1/backups/<string>/restore` | POST | Yes* | Restore backup |
| `/api/v1/backups/<string>` | DELETE | Yes* | Delete backup |

*Auth required only when `NDD_AUTH_TOKEN` is set

## Data Storage

All data is stored under the `default` user directory:

```
$NDD_DATA_DIR/
├── default/           # Default user indexes
│   ├── index1/
│   │   ├── main.idx
│   │   ├── ids/
│   │   ├── vectors/
│   │   └── sparse/
│   └── index2/
├── meta/              # Metadata manager
└── backups/
    └── default/       # Default user backups
```

## No Limits

In open-source mode, there are **no restrictions**:
- Unlimited indexes (up to 10,000)
- Unlimited vectors per index (up to 1 billion)
- All dimensions supported (up to 16,384)
- All quantization levels (float16, float32)
- Checksum/queryable encryption supported
- Bloom filter size: 2^24 (16M elements, configurable via `NDD_BLOOM_FILTER_BITS`)

## Implementation Details

### Simplified User Type System

The open-source version has been simplified to remove all multi-user tier management:

**UserType Enum (auth.hpp)**
```cpp
enum class UserType {
    Admin  // Only one type exists
};
```

All helper functions return Admin values:
- `getMaxAllowedIndices()` → 10,000 indices
- `getMaxVectorsPerIndex()` → 1 billion vectors (MAX_VECTORS_ADMIN)
- `getBloomFilterBits()` → 24 bits (configurable)

### Removed Constants

The following tier-specific constants were removed from `settings.hpp`:

**User Tier Dimension Limits** (removed):
- `STARTER_MAX_DIMENSION` (was 2,000)
- `PRO_MAX_DIMENSION` (was 4,000)

**Vector Limits** (removed, only MAX_VECTORS_ADMIN kept):
- `MAX_VECTORS_DEFAULT` (was 100,000)
- `MAX_VECTORS_STARTER` (was 1 million)
- `MAX_VECTORS_PRO` (was 10 million)
- `MAX_VECTORS_ENTERPRISE` (was 100 million)

**Bloom Filter Limits** (removed):
- `BLOOM_FILTER_BITS_STARTER` (was 20 = 1M elements)
- `BLOOM_FILTER_BITS_PRO` (was 23 = 8M elements)
- `BLOOM_FILTER_BITS_ENTERPRISE` (was 24 = 16M elements)

**Auth Database Sizes** (removed):
- `AUTH_USERS_TOKENS_MAP_SIZE_BITS`
- `AUTH_USERS_TOKENS_MAP_SIZE_MAX_BITS`

**Token Cache** (removed):
- `MAX_TOKENS_IN_CACHE`

### Current Limits (All Users)

| Setting | Value | Source |
|---------|-------|--------|
| Max indices | 10,000 | `getMaxAllowedIndices()` |
| Max vectors per index | 1 billion | `MAX_VECTORS_ADMIN` |
| Max dimension | 16,384 | `MAX_DIMENSION` |
| Bloom filter bits | 24 (16M elements) | `BLOOM_FILTER_BITS` (env configurable) |


## Token Change on Restart

**Changing `NDD_AUTH_TOKEN` and restarting works correctly:**

1. Token is read from environment at startup
2. Indexes are stored at `data_dir/default/index_name/` - path doesn't include token
3. No token is persisted to disk
4. Changing token only affects authentication, not data access

```bash
# Day 1: Create index with token A
export NDD_AUTH_TOKEN="tokenA"
./ndd-avx512 &
curl -H "Authorization: tokenA" -X POST .../index/create ...

# Day 2: Restart with different token
export NDD_AUTH_TOKEN="tokenB"
./ndd-avx512 &
# All indexes still accessible with new token
curl -H "Authorization: tokenB" .../index/list
```

## Migration from Multi-User Version

If migrating from the multi-user version:

1. **Data**: Index data can be kept - move user indexes to `default/` directory
2. **Auth**: Replace root token setup with `NDD_AUTH_TOKEN` environment variable
3. **Admin operations**: No longer available - all operations through single user

## Security Considerations

1. **Token is single point of access** - anyone with the token has full access
2. **Token visible in environment** - use secure methods to set it
3. **No audit logging** - all operations appear from `default` user
4. **Open mode is fully open** - no protection without token
