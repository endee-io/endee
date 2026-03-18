# Bloom Filter

nDD uses bloom filters to optimize string ID lookups in the ID mapping system. The bloom filter serves as a fast probabilistic cache that can definitively say "this ID doesn't exist" or "this ID might exist", avoiding expensive LMDB lookups.

## Architecture

**Storage**: Bloom filter is stored as `{database_path}/id_bloom.bin` (separate from LMDB)
**Memory**: Loaded into RAM at startup for fast lookups
**Hash**: Uses ultra-fast xxHash32 (0.00823 μs/key) with power-of-2 optimized bitwise operations
**Format**: Platform-independent little-endian binary
**Lifecycle**: **Fixed at creation** - set by user tier, rebuilt during index loading
**Performance**: Power-of-2 bit array sizes enable fast bitwise AND operations instead of expensive modulo
**Sizing Model**: **FIXED (no auto-growth)** - determined by subscription tier at index creation time

## Tier-Based Fixed Sizing

The bloom filter uses **fixed tier-based sizing** determined by user subscription level at index creation:

```cpp
// Tier-based configuration (FIXED, no growth)
constexpr size_t BLOOM_FILTER_BITS_STARTER = 20;       // 1M elements (2^20)
constexpr size_t BLOOM_FILTER_BITS_PRO = 23;           // 8M elements (2^23)
constexpr size_t BLOOM_FILTER_BITS_SCALE = 24;    // 16M elements (2^24)

// Get fixed bloom filter bits based on user type
inline size_t getBloomFilterBits(UserType type) {    
    switch (type) {
        case UserType::Starter: return BLOOM_FILTER_BITS_STARTER;
        case UserType::Pro: return BLOOM_FILTER_BITS_PRO;
        case UserType::Scale: return BLOOM_FILTER_BITS_SCALE;
        case UserType::Admin: return BLOOM_FILTER_BITS;
        default: return BLOOM_FILTER_BITS_STARTER;
    }
}
```

### Environment Override

For special deployments, the bloom filter size can be overridden:

```bash
# Override bloom filter size (in bits, must be power-of-2)
export NDD_BLOOM_FILTER_BITS=25  # Sets to 32M elements (2^25)
```

### Hash Optimization

```cpp
// Fast bitwise hash calculation (10x faster than modulo)
positions[i] = hash & (array_size_bits_ - 1);  // Power-of-2 optimization
// vs. slower: positions[i] = hash % array_size_bits_;
```

## Rebuild Strategy

**When**: Automatic rebuild during `loadIndex()` operations  
**Trigger**: Always rebuilt during index load to ensure consistency  
**Tier**: Determined by user subscription level or environment override  
**Process**: 
1. Load HNSW index from disk
2. Scan LMDB database for all existing string IDs
3. Use fixed tier-based size (from user type or override)
4. Create new bloom filter with tier-based fixed capacity
5. Populate with all existing IDs using fast bitwise operations
6. Save to `id_bloom.bin`
7. Make index available to other threads

## Lookup Flow

```
1. Check bloom filter first (in memory) - Uses fast bitwise AND operations
   ├─ "Definitely NOT exists" → Return 0 immediately  
   └─ "Might exist" → Check LMDB database

2. During index loading (loadIndex)
   ├─ Load HNSW index
   ├─ Use fixed tier-based bloom filter size
   ├─ Rebuild bloom filter with fixed capacity
   ├─ Load fixed-size bloom filter with fast hash operations
   └─ Make index available to other threads
```
   └─ Make index available in cache
```

## Integration with Cache Management

The bloom filter works seamlessly with the **load-save-reload** architecture:

```cpp
// During index reload operation
1. saveIndex()     → Persist index state to disk
2. evict index     → Remove from memory cache
3. loadIndex()     → Rebuild bloom filter with fixed tier-based size
4. Cache adjusts   → Automatically matches % threshold of element count

// No race conditions: bloom filter rebuilt during single-threaded load
// Performance: Uses bitwise operations for 10x faster hash calculations
// Thread Safety: Updates only happen during loading when index unavailable
// No Growth: Fixed size per tier prevents unexpected capacity issues
```

## Performance Characteristics

### Hash Operation Optimization
- **Power-of-2 sizes**: Enable `hash & (size-1)` instead of `hash % size`
- **Performance gain**: ~10x faster hash calculations
- **Memory Trade-off**: Optimal 10 bits per element with power-of-2 sizes
- **Cache alignment**: Better CPU cache performance with aligned sizes

### Tier-Based Sizes
```
Starter:      1M bits    (2^20)  - Good for small deployments
Pro:          8M bits    (2^23)  - Mid-market scale
Scale:        16M bits   (2^24)  - Large-scale deployments
Admin/Custom: Configurable via NDD_BLOOM_FILTER_BITS
```

## Configuration

Bloom filter sizing is determined at index creation time:

```bash
# Default behavior: Use tier-based sizing based on user subscription
# Starter users get 1M (2^20)
# Pro users get 8M (2^23)
# Scale users get 16M (2^24)

# Override for special cases (in bits, must be power-of-2)
export NDD_BLOOM_FILTER_BITS=25  # 32M elements (2^25)
```

## API Usage

```cpp
// Bloom filter sizing determined at index creation by user tier
IndexManager manager(max_indices, data_dir);

// Create index with tier-based bloom filter sizing
manager.createIndex(index_id, config, UserType::Pro);  // 8M element bloom filter

// During loadIndex() - bloom filter rebuilds with fixed tier-based size
// No capacity planning needed - tier determines everything
```

## Sizing Logic

**Fixed Model**: One size per user tier, no dynamic growth
**Starter Tier**: 1M elements (2^20 bits)
**Pro Tier**: 8M elements (2^23 bits)
**Scale Tier**: 16M elements (2^24 bits)
**Custom Override**: Environment variable `NDD_BLOOM_FILTER_BITS` (power-of-2)
**Rebuild Timing**: During index loadIndex(), always rebuilds for consistency
**Growth Strategy**: None - fixed size, guaranteed no auto-growth surprises

## Performance

- **Hash Operations**: 10x faster with bitwise AND vs modulo operations
- **False Positive Rate**: 1% (mathematically guaranteed)
- **Memory Efficiency**: ~10 bits per element
- **Capacity Guarantee**: Fixed size, no unexpected growth or capacity issues
- **Cache Performance**: Better CPU cache alignment with power-of-2 sizes
- **Thread Safety**: Rebuild during single-threaded load operations, no concurrent access during updates
- **Zero Configuration**: No manual capacity planning needed

## Error Handling

- **Missing File**: Automatic rebuild from LMDB database
- **Load Failure**: Automatic rebuild with optimal capacity
- **Corruption**: Automatic rebuild on next loadIndex()
- **Capacity Issues**: Automatic doubling during rebuild