# Filter Design & Strategy

This document outlines the architectural design for Endee's filtering system, covering component designs for Numeric, Category, and Boolean types, and the overarching execution strategy.

## 1. Global Filtering Strategy

The system prioritizes **Pre-Filtering** followed by an adaptive search execution path.

### 1.1. Execution Flow
1.  **Filter Analysis:**
    *   Incoming queries (e.g., `Age: [18-25] AND City: "NY"`) are broken into atomic filter operations.
    *   **Cardinality Estimation:** Each filter estimates its result set size (e.g., "NY" has 500 users, "Age" has 10k).
2.  **Optimization (Cheapest First):**
    *   Filters are executed in order of increasing cardinality (smallest first).
    *   Results are intersected (`AND`) incrementally. If the intermediate result becomes empty, execution stops early.
3.  **Adaptive Search Path:**
    *   Final `RoaringBitmap` of valid IDs is passed to the Vector Search engine.
    *   **Small Result (< 1,000 IDs):** **Bypass HNSW.** Fetch vectors for valid IDs directly and perform Brute Force distance calculation. This avoids graph overhead for sparse results.
    *   **Large Result:** **Filtered HNSW.** Pass the Bitmap to HNSW's `searchKnn` via `BitMapFilterFunctor`.

---

## 2. Numeric Filter Design

*Optimized for range queries, high compression, and sequential access.*

### 2.1. Storage Architecture (Hybrid Bucket)
The database (LMDB) acts as a coarse-grained B+ Tree.
*   **Key:** `[FieldID] + [Base_Value_32bit]`.
    *   Floats are mapped to lexicographically ordered integers to preserve sort order.
    *   Keys are stored in Big-Endian to support native cursor iteration.
*   **Value (Bucket):** Fixed-size block (Max 1024 unique values).
    *   **Summary Bitmap (Roaring):** Pre-computed union of all IDs in the bucket. Used for $O(1)$ block retrieval during full overlaps.
    *   **Data Arrays (Structure of Arrays - SoA):**
        *   **Values:** Compressed as `uint16_t` deltas relative to the Key's `Base_Value`.
        *   **IDs:** Raw `idInt` array, index-aligned with values.

### 2.2. Query Execution
*   **Buckets Fully Inside Selection (Middle):** Use **Summary Bitmap**. Zero array access.
*   **Buckets Partially Overlapping (Edges):** Scan `Values` array (SIMD), use indices to fetch specific `IDs`.

### 2.3. Constraints & Splitting
*   **Split Triggers:** Count > 1024 OR Delta > 65,535.
*   **Sliding Split:** To ensure Key Uniqueness in LMDB, splits do not strictly occur at the median. The split point "slides" right to find the first value divergence, ensuring `Key(RightBucket) != Key(LeftBucket)`.

### 2.4. Comparison Operators

The system supports four comparison operators for numeric fields, enabling flexible single-boundary range queries:

#### `$gt` - Greater Than (Exclusive)
Returns documents where the field value is **strictly greater than** the specified value.

**Syntax:**
```json
[{"field_name": {"$gt": value}}]
```

**Supported Types:** Numeric fields only (integers and floats)

**Examples:**
```json
// Find users older than 25
[{"age": {"$gt": 25}}]

// Find products with price greater than 99.99
[{"price": {"$gt": 99.99}}]

// Combine with other filters: users in NY older than 30
[
  {"city": {"$eq": "NY"}},
  {"age": {"$gt": 30}}
]
```

**Implementation:** Uses `range(value+1, UINT32_MAX)` after sortable conversion. Edge case: returns empty bitmap if value equals maximum sortable value.

#### `$gte` - Greater Than or Equal To (Inclusive)
Returns documents where the field value is **greater than or equal to** the specified value.

**Syntax:**
```json
[{"field_name": {"$gte": value}}]
```

**Supported Types:** Numeric fields only (integers and floats)

**Examples:**
```json
// Find users 25 or older
[{"age": {"$gte": 25}}]

// Find products with price at least 100
[{"price": {"$gte": 100.0}}]
```

**Implementation:** Uses `range(value, UINT32_MAX)` after sortable conversion.

#### `$lt` - Less Than (Exclusive)
Returns documents where the field value is **strictly less than** the specified value.

**Syntax:**
```json
[{"field_name": {"$lt": value}}]
```

**Supported Types:** Numeric fields only (integers and floats)

**Examples:**
```json
// Find users younger than 30
[{"age": {"$lt": 30}}]

// Find products with price less than 100
[{"price": {"$lt": 100.0}}]
```

**Implementation:** Uses `range(0, value-1)` after sortable conversion. Edge case: returns empty bitmap if value equals minimum sortable value (0).

#### `$lte` - Less Than or Equal To (Inclusive)
Returns documents where the field value is **less than or equal to** the specified value.

**Syntax:**
```json
[{"field_name": {"$lte": value}}]
```

**Supported Types:** Numeric fields only (integers and floats)

**Examples:**
```json
// Find users 30 or younger
[{"age": {"$lte": 30}}]

// Find products with price up to 100 (inclusive)
[{"price": {"$lte": 100.0}}]

// Combine: find products between 10 and 100 (inclusive)
[
  {"price": {"$gte": 10.0}},
  {"price": {"$lte": 100.0}}
]
```

**Implementation:** Uses `range(0, value)` after sortable conversion.

**Note:** All comparison operators work with both positive and negative numbers, including floats. You can combine operators to create precise ranges (e.g., `$gte` + `$lte` for inclusive range, `$gt` + `$lt` for exclusive range).

---

## 3. Category Filter Design

*Optimized for exact match lookups and faceting.*

### 3.1. Interface (MongoDB-Style)
*   **Single Value:** `{"City": "NY"}`
*   **List Membership ($in):** `{"City": {"$in": ["NY", "London", "Tokyo"]}}`

### 3.2. Storage Architecture
Utilizes Inverted Indices with **Text-Based Keys** to enable prefix scanning and faceting.
*   **Key:** `[FieldName] + ":" + [Value]`.
    *   **Parsing Logic:** The system strictly splits on the **first** occurrence of `:`.
    *   **Format:** `City:New:York` is parsed as Field=`City`, Value=`New:York`.
    *   **Constraints:** `FieldName` must **not** contain the `:` character (alphanumeric + underscore recommended). `Value` can contain any character including `:`.
*   **Value:** `RoaringBitmap` (Serialized). Contains all IDs that have this attribute value.

### 3.3. Query Execution
*   **Exact Match:** Direct Key lookup.
*   **$in Query:**
    1.  Parse the list `["NY", "London"]`.
    2.  Perform multiple Key lookups.
    3.  Compute the **Union** of the resulting Bitmaps efficiently.

---

## 4. Boolean Filter Design

*Optimized for extreme density ops.*

### 4.1. Storage Architecture
Treated as a specialized Category filter with strictly two possible keys per field.
*   **Keys:** `[FieldName]:0` (False) and `[FieldName]:1` (True).
    *   Consistent with the text-based key design (uses `:` separator).
*   **Value:** `RoaringBitmap`.

### 4.2. Strategy
Boolean filters are typically low-selectivity (often matching ~50% of the DB). They are processed **Last** in the intersection chain unless statistics indicate high skew (e.g., `Is_Active` is true for 99% of data, so filtering for `False` is fast).

---

## 5. Schema & Type Enforcement

To ensure index integrity without a strict schema registry, the system adheres to **First-Write Wins** typing.

*   **Immutable Types:** Once a `FieldName` is indexed with a specific type (Numeric, Category, or Boolean), that type is bound to the field.
*   **Validation Logic:**
    *   If `is_active` is first seen as **Boolean**, subsequent attempts to insert `is_active: "yes"` (Category) or `is_active: 1` (Numeric bucket) must be rejected.
    *   This prevents storage corruption and ambiguous query parsing.
