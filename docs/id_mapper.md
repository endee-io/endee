# ID Mapper Documentation

## Overview

The `IDMapper` class is a critical component of the NDD vector database that manages the mapping between external string identifiers (user-provided IDs) and internal numeric identifiers used by the HNSW algorithm. It provides atomic ID generation, efficient lookups, and ID recycling capabilities.

## Architecture

### Storage Backend
- **Database**: LMDB (Lightning Memory-Mapped Database)
- **Map Size**: 1GB (configurable via `settings::ID_MAPPER_SIZE_BITS`)
- **File Location**: `{index_path}/id_mapper/`
- **Bloom Filter**: Separate file `id_bloom.bin` for fast negative lookups

### Key Components

#### 1. Core Mapping Storage
- **Format**: `string_id → labelInt (numeric_id)`
- **Type**: Direct key-value pairs in LMDB
- **Thread Safety**: LMDB provides ACID transactions

#### 2. Special Keys
The database contains three special internal keys with random suffixes to avoid collisions:

```cpp
NEXT_ID_KEY = "__next_id_px7b39lw__"       // Stores next available numeric ID
DELETED_IDS_KEY = "__deleted_ids_px7b39lw__" // Stores array of reusable IDs
BLOOM_FILTER_FILENAME = "id_bloom.bin"     // Bloom filter file
```

#### 3. ID Generation Strategy
- **Sequential Generation**: IDs start from 1 and increment
- **Atomic Counter**: `NEXT_ID_KEY` stores the next available ID
- **ID Recycling**: Deleted IDs are stored and reused before generating new ones
- **Batch Generation**: Multiple IDs generated atomically in single transaction

#### 4. Bloom Filter Integration
- **Purpose**: Fast negative lookups to avoid unnecessary LMDB queries
- **False Positive Rate**: 1% (0.01)
- **Auto-sizing**: Grows based on element count using bit-doubling
- **Persistence**: Saved to disk and loaded on startup

## Core Operations

### 1. ID Creation - `create_ids_batch()`

The primary method for mapping string IDs to numeric IDs:

```cpp
template <bool use_deleted_ids>
std::vector<std::pair<labelInt, bool>> create_ids_batch(
    const std::vector<std::string>& str_ids, 
    void* wal_ptr = nullptr
);
```

**Process Flow:**

1. **Bloom Filter Check**
   - Quick filter to identify definitely-new IDs
   - IDs that "might exist" proceed to LMDB lookup
   - IDs that "definitely don't exist" are marked for creation

2. **LMDB Lookup**
   - Read-only transaction to check existing mappings
   - Returns existing numeric IDs for found strings
   - Marks non-existent strings for ID creation

3. **Deleted ID Recycling** (if `use_deleted_ids = true`)
   - Retrieves deleted IDs from `DELETED_IDS_KEY`
   - Assigns recycled IDs to new string IDs
   - Reduces new ID generation count

4. **New ID Generation**
   - Calls `get_next_ids()` for remaining needed IDs
   - **WAL Logging**: Immediately logs `VECTOR_ADD` operations to WAL
   - Atomic increment of `NEXT_ID_KEY` counter

5. **Database Write**
   - Write transaction to store new string→numeric mappings
   - Updates bloom filter with new strings
   - Handles LMDB map resizing if needed
   - Commits all changes atomically

**Return Value:**
- Vector of `(numeric_id, is_new)` pairs
- `is_new = true`: New mapping created
- `is_new = false`: Existing mapping returned

### 2. ID Lookup - `get_id()`

Fast lookup of existing string→numeric mappings:

```cpp
labelInt get_id(const std::string& str_id) const;
```

**Process:**
1. **Bloom Filter Check**: Quick negative filter
2. **LMDB Lookup**: Direct key lookup if bloom filter says "might exist"
3. **Return**: Numeric ID or 0 if not found

### 3. ID Deletion - `deletePoints()`

Removes mappings and adds numeric IDs to recycle pool:

```cpp
std::vector<labelInt> deletePoints(const std::vector<std::string>& external_ids);
```

**Process:**
1. Look up numeric IDs for each string ID
2. Delete string→numeric mappings from LMDB
3. Add numeric IDs to `DELETED_IDS_KEY` array for recycling
4. Return vector of deleted numeric IDs (0 for not found)

### 4. Deleted ID Retrieval - `getDeletedIds()`

Retrieves and removes deleted IDs for recycling:

```cpp
std::vector<labelInt> getDeletedIds(size_t max_count);
```

**Process:**
1. Read `DELETED_IDS_KEY` array from LMDB
2. Extract up to `max_count` IDs
3. Update remaining IDs back to database (or delete key if empty)
4. Return extracted IDs for reuse

## Atomicity and Crash Recovery

### Write-Ahead Logging (WAL) Integration

The ID mapper integrates with NDD's WAL system to ensure atomicity:

1. **ID Generation Logging**: New IDs are logged to WAL immediately after generation
2. **Operation Type**: Uses `WALOperationType::VECTOR_ADD`
3. **Recovery**: Failed operations are detected and IDs are reclaimed

### Crash Recovery Process

When the system recovers from a crash:

1. **WAL Replay**: Processes all WAL entries in order
2. **Orphaned ID Detection**: `VECTOR_ADD` entries without corresponding vectors
3. **ID Reclamation**: Orphaned IDs are added back to deleted_ids pool
4. **Consistency Restoration**: Ensures no IDs are permanently lost

### Atomicity Guarantees

- **ID Generation**: Atomic increment of next_id counter
- **Batch Operations**: All mappings in a batch succeed or fail together
- **WAL Coordination**: IDs logged before storage operations
- **Recovery Safety**: Lost IDs are automatically reclaimed

## Bloom Filter Management

### Automatic Sizing Strategy

The bloom filter uses intelligent auto-sizing:

```cpp
size_t calculateOptimalBloomSize(size_t current_elements) const {
    return settings::calculateOptimalBloomSize(current_elements);
}
```

**Sizing Logic:**
- **Bit-based doubling**: Powers of 2 sizing for efficient memory usage
- **Growth triggers**: Rebuilds when capacity < optimal size
- **Custom sizing**: Supports user-specified minimum sizes
- **Performance optimization**: Balances memory usage vs. false positive rate

### Rebuild Process

Bloom filter rebuilding happens:
1. **On startup**: If file missing or undersized
2. **During operation**: When capacity insufficient
3. **After recovery**: To ensure consistency

**Rebuild Steps:**
1. Calculate optimal size based on current element count
2. Create new bloom filter with optimal capacity
3. Iterate through all LMDB keys (excluding special keys)
4. Add all string IDs to new bloom filter
5. Replace old filter and mark as modified
6. Save to disk

## Performance Characteristics

### Lookup Performance
- **Bloom Filter**: O(k) where k = number of hash functions (~3-4)
- **LMDB Lookup**: O(log n) B-tree lookup
- **Cache Locality**: LMDB uses memory mapping for efficiency

### Batch Operations
- **Amortized Cost**: Single transaction for multiple operations
- **Bloom Filter Batching**: Multiple additions before save
- **Reduced Syscalls**: Minimize transaction overhead

### Memory Usage
- **LMDB Map**: 1GB virtual memory (sparse allocation)
- **Bloom Filter**: Size based on element count (typically KB-MB range)
- **Working Set**: Minimal resident memory due to memory mapping

## Thread Safety

### Concurrency Model
- **LMDB Transactions**: Provide ACID properties
- **Read Concurrency**: Multiple concurrent readers supported
- **Write Serialization**: Single writer at a time (per LMDB design)
- **Mutex Protection**: `next_id` operations protected by mutex

### Lock-Free Reads
- Read operations (lookups) don't require exclusive locks
- Bloom filter reads are atomic at the data structure level
- LMDB handles read isolation automatically

## Error Handling

### LMDB Error Recovery
- **Map Full**: Automatic map size doubling and retry
- **Transaction Conflicts**: Automatic abort and cleanup
- **Corruption Detection**: LMDB integrity checks

### Bloom Filter Recovery
- **Load Failure**: Automatic rebuild from database
- **Size Mismatch**: Automatic resize and rebuild
- **File Corruption**: Fallback to database-only operation

### WAL Integration Errors
- **WAL Failure**: Operations continue (IDs may be lost but recovered)
- **Recovery Errors**: Failed IDs added to recycle pool
- **Consistency Checks**: Orphaned ID detection and cleanup

## Configuration

### Constructor Parameters
```cpp
IDMapper(
    const std::string& path,           // Storage directory
    bool is_new = false,              // New index flag
    UserType user_type = UserType::Starter,  // User tier (affects sizing)
    size_t custom_bloom_size = 0      // Custom bloom filter size
);
```

### Sizing Configuration
- **ID_MAPPER_SIZE_BITS**: LMDB map size (default: 30 = 1GB)
- **BLOOM_FILTER_BITS**: Bloom filter size (default: 20 = 1M capacity)
- **Custom sizing**: Override defaults via constructor parameter

## Usage Patterns

### Vector Addition
```cpp
// During vector addition
std::vector<std::string> str_ids = {"vec1", "vec2", "vec3"};
WriteAheadLog* wal = getWAL();

// Create mappings with deleted ID reuse
auto mappings = id_mapper->create_ids_batch<true>(str_ids, wal);

for (auto [numeric_id, is_new] : mappings) {
    if (is_new) {
        // Handle new vector
    } else {
        // Handle existing vector (update case)
    }
}
```

### Vector Lookup
```cpp
// Fast ID lookup
labelInt numeric_id = id_mapper->get_id("vec1");
if (numeric_id == 0) {
    // Vector doesn't exist
} else {
    // Use numeric_id for HNSW operations
}
```

### Vector Deletion
```cpp
// Delete vectors and recycle IDs
std::vector<std::string> to_delete = {"vec1", "vec2"};
auto deleted_ids = id_mapper->deletePoints(to_delete);

// deleted_ids can be reused for new vectors
```

## Best Practices

### Performance Optimization
1. **Batch Operations**: Use `create_ids_batch()` instead of individual calls
2. **Bloom Filter Maintenance**: Allow automatic rebuilding for optimal performance
3. **ID Recycling**: Enable deleted ID reuse to minimize ID space growth

### Reliability
1. **WAL Integration**: Always pass WAL pointer for crash recovery
2. **Error Handling**: Check return values and handle exceptions
3. **Regular Maintenance**: Monitor bloom filter hit rates

### Monitoring
1. **ID Space Usage**: Monitor `get_count()` for growth patterns
2. **Bloom Filter Efficiency**: Track false positive rates
3. **Deleted ID Pool**: Monitor recycling effectiveness

## Implementation Notes

### Key Design Decisions

1. **LMDB Choice**: Provides ACID transactions, memory mapping, and excellent performance
2. **Bloom Filter Integration**: Reduces unnecessary LMDB lookups significantly
3. **ID Recycling**: Prevents ID space exhaustion in high-churn scenarios
4. **WAL Coordination**: Ensures no IDs are lost during crashes
5. **Batch Processing**: Amortizes transaction costs for better performance

### Future Considerations

1. **Sharding**: For very large ID spaces, consider database sharding
2. **Compression**: Large string IDs could benefit from compression
3. **Analytics**: Add metrics for monitoring and optimization
4. **Backup/Restore**: Integration with database backup systems

This documentation reflects the current implementation as of the analysis date and should be updated as the codebase evolves.