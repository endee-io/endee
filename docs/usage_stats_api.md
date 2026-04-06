# Usage Stats API — Dashboard Integration

## Overview

The server periodically POSTs usage stats to the configured `USAGE_STATS_URL` endpoint.
The dashboard must accumulate these incremental values to compute monthly totals.

---

## Payload Format

**Method:** `POST`  
**Content-Type:** `application/json`

```json
[
  {
    "server_id": "srv_abc123",
    "index_id":  "my_index",
    "query_count": 42,
    "timestamp":  1744012800
  },
  ...
]
```

| Field | Type | Description |
|-------|------|-------------|
| `server_id` | string | Unique identifier for this server instance |
| `index_id` | string | Index name (scoped to the server) |
| `query_count` | integer | Number of search queries made on this index **since the last send** |
| `timestamp` | integer | Unix epoch seconds (UTC) at the time of sending |

---

## Send Cadence

- Sent every **5 minutes** (autosave cycle)
- Only indices with `query_count > 0` are included
- Counter resets to 0 after each successful send

---

## Dashboard Accumulation

Because `query_count` is **incremental** (not cumulative), the dashboard must sum values over time:

```sql
-- Monthly total queries per index
SELECT
    server_id,
    index_id,
    SUM(query_count) AS monthly_queries,
    DATE_TRUNC('month', TO_TIMESTAMP(timestamp)) AS month
FROM usage_stats
GROUP BY server_id, index_id, month
ORDER BY month DESC, monthly_queries DESC;
```

---

## Configuration

Set the `USAGE_STATS_URL` environment variable on the server:

```bash
NDD_USAGE_STATS_URL=https://your-dashboard.example.com/api/usage
```

If the URL is empty, stats are written to the debug log instead of being sent.

---

## Response

The server expects any `2xx` response. Non-2xx or connection failures are logged as debug warnings and do not affect search functionality.
