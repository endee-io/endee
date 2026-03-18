This is a comprehensive list of `curl` commands to interact with your Vector Database API.

Replace `http://localhost:8080` with your actual server address and `<YOUR_TOKEN>` with the token received from the authentication or root endpoints.

---

# Vector Database API Reference

## 1. General Endpoints

These endpoints are primarily for system status and do not require authentication.

### Health Check

Check if the server is running.

```bash
curl -X GET http://localhost:8080/api/v1/health

```

### System Stats

Get version and uptime info.

```bash
curl -X GET http://localhost:8080/api/v1/stats

```

### Initialize Root Token

**One-time operation** to generate the initial root token.

```bash
curl -X POST http://localhost:8080/api/v1/root/token

```

---

## 2. Admin Endpoints (Root Only)

These require the `Authorization` header with a root-level token.

### Create a New User

`user_type` can be: `Starter`, `Pro`, `Scale`, or `Admin`.

```bash
curl -X POST http://localhost:8080/api/v1/admin/users \
     -H "Authorization: <ROOT_TOKEN>" \
     -H "Content-Type: application/json" \
     -d '{
           "username": "<USER>",
           "user_type": "Pro"
         }'

```

### List All Users

```bash
curl -X GET http://localhost:8080/api/v1/admin/users \
     -H "Authorization: <ROOT_TOKEN>"

```

### Delete or Deactivate a User

```bash
# Delete
curl -X DELETE http://localhost:8080/api/v1/admin/users/<USER> \
     -H "Authorization: <ROOT_TOKEN>"

# Deactivate
curl -X POST http://localhost:8080/api/v1/admin/users/<USER>/deactivate \
     -H "Authorization: <ROOT_TOKEN>"

```

### List All Global Indexes

```bash
curl -X GET http://localhost:8080/api/v1/admin/indexes \
     -H "Authorization: <ROOT_TOKEN>"

```

### Manage User Tokens

Admin can generate or delete tokens for specific users.

```bash
# Create token for user
curl -X POST http://localhost:8080/api/v1/admin/users/<USER>/tokens \
     -H "Authorization: <ROOT_TOKEN>" \
     -H "Content-Type: application/json" \
     -d '{"name": "mobile_app_key"}'

# List user tokens
curl -X GET http://localhost:8080/api/v1/admin/users/<USER>/tokens \
     -H "Authorization: <ROOT_TOKEN>"

```

---

## 3. User & Token Management

Endpoints for users to manage their own account and secondary tokens.

### Get My Info

```bash
curl -X GET http://localhost:8080/api/v1/users/<USER>/info \
     -H "Authorization: <USER_TOKEN>"

```

### Create a Personal Token

```bash
curl -X POST http://localhost:8080/api/v1/tokens \
     -H "Authorization: <USER_TOKEN>" \
     -H "Content-Type: application/json" \
     -d '{"name": "new_api_key"}'

```

---

## 4. Index Operations

Endpoints to create and manage vector collections.

### Create Index

* `space_type`: "l2", "ip", or "cosine".
* `precision`: "medium" (INT8), "high" (FP16), "ultra-high" (FP32).

```bash
curl -X POST http://localhost:8080/api/v1/index/create \
     -H "Authorization: <USER_TOKEN>" \
     -H "Content-Type: application/json" \
     -d '{
           "index_name": "<INDEX_NAME>",
           "dim": 128,
           "space_type": "l2",
           "precision": "medium",
           "M": 16,
           "ef_con": 200
         }'

```

### List My Indexes

```bash
curl -X GET http://localhost:8080/api/v1/index/list \
     -H "Authorization: <USER_TOKEN>"

```

### Get Index Info (Metadata)

```bash
curl -X GET http://localhost:8080/api/v1/index/<INDEX_NAME>/info \
     -H "Authorization: <USER_TOKEN>"

```

### Delete Index

```bash
curl -X DELETE http://localhost:8080/api/v1/index/<INDEX_NAME>/delete \
     -H "Authorization: <USER_TOKEN>"

```

---

## 5. Vector Operations (Data Management)

### Insert Vectors (MessagePack)

Note: This endpoint requires **MessagePack** binary data. Using `curl` with a file:

```bash
curl -X POST http://localhost:8080/api/v1/index/<INDEX_NAME>/vector/insert \
     -H "Authorization: <USER_TOKEN>" \
     -H "Content-Type: application/msgpack" \
     --data-binary @vectors.msgpack

```

### Search (k-NN)

* `k`: Number of neighbors.
* `filter`: Array-based logic (e.g., `[{"category": {"$eq": "electronics"}}]`).

```bash
curl -X POST http://localhost:8080/api/v1/index/<INDEX_NAME>/search \
     -H "Authorization: <USER_TOKEN>" \
     -H "Content-Type: application/json" \
     -d '{
           "vector": [0.1, 0.2, 0.3, ...],
           "k": 10,
           "ef": 50,
           "include_vectors": true,
           "filter": "[{\"price\":{\"$lt\":100}}]"
         }'

```

*Note: The response will be in MessagePack format.*

### Get a Single Vector

```bash
curl -X POST http://localhost:8080/api/v1/index/<INDEX_NAME>/vector/get \
     -H "Authorization: <USER_TOKEN>" \
     -H "Content-Type: application/json" \
     -d '{"id": "vec_123"}'

```

### Delete a Single Vector

```bash
curl -X DELETE http://localhost:8080/api/v1/index/<INDEX_NAME>/vector/vec_123/delete \
     -H "Authorization: <USER_TOKEN>"

```

### Delete Vectors by Filter

```bash
curl -X DELETE http://localhost:8080/api/v1/index/<INDEX_NAME>/vectors/delete \
     -H "Authorization: <USER_TOKEN>" \
     -H "Content-Type: application/json" \
     -d '{
           "filter": [{"status": {"$eq": "deprecated"}}]
         }'

```

---

## 6. Recovery Operations (Root Only)

### Reset an Index

Used to re-initialize metadata or clear a corrupted index.

```bash
curl -X POST http://localhost:8080/api/v1/admin/users/<USER>/indices/products/reset \
     -H "Authorization: <ROOT_TOKEN>" \
     -H "Content-Type: application/json" \
     -d '{
           "dim": 128,
           "space_type": "l2",
           "quant_level": 8
         }'

```

### Recover an Index

Triggers a recovery batch process from the persistence layer.

```bash
curl -X POST http://localhost:8080/api/v1/admin/users/<USER>/indices/products/recover \
     -H "Authorization: <ROOT_TOKEN>"

```