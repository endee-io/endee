# Serverless Mode - Auth, Tokens & Tier Management


---

## OSS vs Serverless

| | OSS | Serverless (`-DNDD_SERVERLESS=ON`) |
|---|---|---|
| Users | Single user ("default") | Multi-user, per-user isolation |
| Auth | `NDD_AUTH_TOKEN` (mandatory, root admin) |
| Tiers | None (unlimited) | 4 tiers: Starter, Pro, Scale, Admin |
| Tokens | Plain text comparison | SHA-256 hashed, LRU cached |
| Admin routes | None | 18 endpoints for user/token/index management |

---

## How Auth Works

```
Client Request
  |
  |  Authorization: <username>:<random_token>
  v
EnterpriseAuthMiddleware (middleware_enterprise.hpp)
  |
  |-- 1. Is it root token (NDD_AUTH_TOKEN)?  -->  username = "root", type = Admin
  |
  |-- 2. Parse username from "username:random"
  |-- 3. SHA-256 hash the full raw token
  |-- 4. Check LRU cache (10K entries) --> HIT: return username
  |-- 5. Check MDBX tokens_dbi          --> FOUND: cache it, return username
  |-- 6. Not found                       --> 401 Unauthorized
  |
  |-- 7. Lookup user type from users_dbi
  |-- 8. User inactive?                  --> 403 Forbidden
  |
  v
Route handler receives: ctx.username + ctx.user_type
```

**Key point**: Raw tokens are NEVER stored. Only SHA-256 hashes exist in MDBX and cache.

---

## MDBX Database (data_dir/auth/)

Two named databases (DBIs):

| DBI | Key | Value | Purpose |
|-----|-----|-------|---------|
| `users` | `"alice"` | `{"username":"alice", "is_active":true, "user_type":"Starter", "created_at":...}` | User accounts |
| `tokens` | `"alice:<sha256_hash>"` | `{"hashed_token":"<hash>", "name":"default", "username":"alice", "created_at":...}` | Token auth |

**Token operations**:
- **Validate** (hot path): Direct O(1) lookup by `username:hash` key
- **List/Delete by name**: Prefix scan `username:*`, parse JSON to match `name` field
- **Duplicate check**: Same prefix scan to ensure token name uniqueness

---

## Tier System

### Limits per Tier

| Resource | Starter | Pro | Scale | Admin |
|----------|---------|-----|-------|-------|
| Max Vectors | 1M | 10M | 100M | 1B |
| Max Dimensions | 2,000 | 4,000 | 8,000 | Unlimited |
| Max Indices | 3 | 10 | Unlimited | Unlimited |
| Bloom Filter Bits | 20 | 23 | 24 | 24 |

### Allowed Precisions per Tier

| Precision | Starter | Pro | Scale | Admin |
|-----------|---------|-----|-------|-------|
| int8d | Yes | Yes | Yes | Yes |
| int16d | -- | Yes | Yes | Yes |
| float16 | -- | Yes | Yes | Yes |
| float32 | -- | Yes | Yes | Yes |
| binary | -- | Yes | Yes | Yes |

**To change allowed precisions**: Edit the vectors in `enterprise/settings_enterprise.hpp` -- no other code changes needed.

### Where Limits are Enforced

All tier limits are checked in `src/main.cpp` inside `#ifdef NDD_SERVERLESS` block during index creation:

```
POST /api/v1/index/create
  --> Parse precision, dimension, etc.
  --> #ifdef NDD_SERVERLESS
        Check dimension   <= getMaxDimension(user_type)
        Check index count <  getMaxAllowedIndices(user_type)
        Check vector count<= getMaxVectorsPerIndex(user_type)
        Check precision   in isPrecisionAllowed(user_type, level)
      #endif
  --> createIndex(...)
```

---

## Token Lifecycle

### Create User (generates default token)
```
POST /api/v1/admin/users  (admin only)
Body: {"username": "alice", "user_type": "starter"}

  1. Validate username (3-32 chars, [a-zA-Z0-9_], not reserved)
  2. Store user in users_dbi
  3. Create data_dir/alice/ directory
  4. Generate token:
     - random = RAND_bytes(32) -> alphanumeric string
     - raw_token = "alice:<random>"
     - hash = SHA256(raw_token)
     - Store in tokens_dbi: key="alice:<hash>", val=Token JSON
     - Cache: hash -> "alice"
  5. Return raw_token to client (ONLY time it's visible)
```

### Generate Additional Token
```
POST /api/v1/admin/users/:username/tokens  (admin)
POST /api/v1/tokens                         (self-service)
Body: {"name": "my-api-key"}

Same flow as above, with duplicate name check via prefix scan.
```

### Delete Token
```
DELETE /api/v1/admin/users/:username/tokens/:name  (admin)
DELETE /api/v1/tokens/:name                         (self-service)

  1. Prefix scan tokens_dbi for "username:*"
  2. Parse JSON, find entry where name matches
  3. Delete from tokens_dbi
  4. Invalidate from LRU cache
```

### Deactivate User
```
POST /api/v1/admin/users/:username/deactivate  (admin only)

  1. Delete ALL user tokens from tokens_dbi (prefix scan)
  2. Invalidate ALL from cache
  3. Set user.is_active = false in users_dbi
  Result: User can't authenticate. Indexes preserved on disk.
```

---

## API Endpoints

### Admin-Only (require root token or Admin user)

| Method | Endpoint | Action |
|--------|----------|--------|
| POST | `/api/v1/admin/users` | Create user + default token |
| GET | `/api/v1/admin/users` | List all users |
| DELETE | `/api/v1/admin/users/:username` | Delete user + all tokens |
| POST | `/api/v1/admin/users/:username/deactivate` | Deactivate + delete tokens |
| POST | `/api/v1/admin/users/:username/activate` | Re-activate user |
| PUT | `/api/v1/admin/users/:username/type` | Change tier |
| GET | `/api/v1/admin/users/:username/indices` | List user's indices |
| GET | `/api/v1/admin/indexes` | List ALL indexes |
| DELETE | `/api/v1/admin/users/:username/indexes/:name` | Delete user's index |
| POST | `/api/v1/admin/users/:username/tokens` | Create token for user |
| GET | `/api/v1/admin/users/:username/tokens` | List user's tokens |
| DELETE | `/api/v1/admin/users/:username/tokens/:name` | Delete user's token |
| POST | `/api/v1/admin/users/:username/indices/:name/reset` | Reset index |
| POST | `/api/v1/admin/users/:username/indices/:name/recover` | Recover index |

### Self-Service (user manages their own resources)

| Method | Endpoint | Action |
|--------|----------|--------|
| GET | `/api/v1/users/:username/info` | Get own info |
| GET | `/api/v1/users/:username/type` | Get own tier |
| POST | `/api/v1/tokens` | Create own token |
| GET | `/api/v1/tokens` | List own tokens |
| DELETE | `/api/v1/tokens/:name` | Delete own token |

---

## File Reference

```
enterprise/
  settings_enterprise.hpp   - All tier limits + allowed precisions (configurable)
  auth_enterprise.hpp       - AuthManager: MDBX, SHA-256, user/token CRUD, tier helpers
  auth_token_cache.hpp      - LRU cache (hashed_token -> username, thread-safe)
  middleware_enterprise.hpp  - Crow middleware: validates token, sets ctx.username + ctx.user_type
  admin_routes_enterprise.hpp - 18 HTTP endpoints (admin + self-service)
```

### Conditional Compilation (`#ifdef NDD_SERVERLESS`)

Used in `src/main.cpp` for:
1. Including serverless headers
2. Using `EnterpriseAuthMiddleware` instead of OSS `AuthMiddleware`
3. Mandatory `NDD_AUTH_TOKEN` check
4. Tier-based limits on index creation (dimension, count, vectors, precision)
5. Passing `ctx.user_type` to `createIndex()`
6. Registering admin routes

---

## Data Layout on Disk

```
data_dir/
+-- auth/                 MDBX database (users + tokens)
+-- meta/                 MDBX database (index metadata)
+-- backups/              System-wide backups
+-- alice/                Per-user directory
|   +-- my-index/         Index directory
|       +-- main.idx      HNSW index
|       +-- wal.bin       Write-ahead log
|       +-- ids/          ID mapper (LMDB)
|       +-- vectors/      Quantized vector storage
+-- bob/
    +-- ...
```

Indexes are scoped as `username/index_name` -- users can only access their own.
