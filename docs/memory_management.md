# Memory Management

## Overview

Endee does not keep every index resident all the time. `IndexManager` keeps a bounded set of
live indices in memory in `indices_`, and uses `indices_list_` to choose eviction candidates when
that live set is full.

When an index is created or loaded on demand, `ensureLiveIndexCapacity()` may call
`evictIfNeeded()` before admitting the new live index.

In the current implementation, a live index consists of:

- the in-memory `HierarchicalNSW<float>` graph
- `IDMapper`
- `VectorStorage`, which itself owns separate MDBX-backed stores for vectors, metadata, and filters
- optional `SparseVectorStorage`
- the per-index WAL object

## What Actually Uses DRAM

The dominant in-memory cost of a live dense index is the HNSW structure:

- the base layer, allocated as `maxElements * sizeDataAtBaseLayer_`
- upper-layer node storage in `dataUpperLayer_`
- the vector cache, sized from `VECTOR_CACHE_PERCENTAGE` and `VECTOR_CACHE_MIN_BITS`
- the visited-list pool and other small bookkeeping structures

One important detail: Endee does not load the full dense vector corpus into the HNSW object.
Dense vectors stay in `VectorStorage` and are fetched on demand through the vector fetcher and the
vector cache. So the main DRAM cost is the graph plus cache, not a second full copy of the vector
database.

## Scaling 

### 1. Virtual Address Space

Each live dense index opens multiple MDBX environments, each with a large configured upper bound:

| Component | Max map size |
| --- | --- |
| `IDMapper` | 8 GiB |
| dense vector store | 4 TiB |
| dense metadata store | 512 GiB |
| filter store | 64 GiB |
| sparse storage | 1 TiB |

These are the default configured maxima. In `settings.hpp`, both the initial/current map size and
the maximum map size are runtime-configurable through environment variables such as:
`NDD_INDEX_META_MAP_SIZE_BITS`, `NDD_INDEX_META_MAP_SIZE_MAX_BITS`,
`NDD_ID_MAPPER_MAP_SIZE_BITS`, `NDD_ID_MAPPER_MAP_SIZE_MAX_BITS`,
`NDD_FILTER_MAP_SIZE_BITS`, `NDD_FILTER_MAP_SIZE_MAX_BITS`,
`NDD_METADATA_MAP_SIZE_BITS`, `NDD_METADATA_MAP_SIZE_MAX_BITS`,
`NDD_VECTOR_MAP_SIZE_BITS`, `NDD_VECTOR_MAP_SIZE_MAX_BITS`, and
`NDD_SPARSE_MAP_SIZE_MAX_BITS`.

That is about 5.57 TiB of configured MDBX map capacity for a live index with sparse storage
enabled. For a dense-only index, the total is about 4.57 TiB. There is also one global metadata
environment for index metadata with a 128 MiB upper bound.

Because these environments stay open while the index is live, virtual address space becomes a
scaling constraint, especially when many indices are resident at once.

### 2. Server DRAM

Each live index allocates its HNSW graph structures eagerly when the index is created or loaded.
That memory must fit in RAM for the server to stay healthy.

### 3. Sticky-Thread MDBX Environments and `PTHREAD_KEYS_MAX`

All dense-index MDBX environments are currently opened without `MDBX_NOSTICKYTHREADS`. The only
place that enables `MDBX_NOSTICKYTHREADS` today is sparse storage.

That means a live dense index currently opens four sticky-thread MDBX environments:

- `IDMapper`
- dense vector store
- dense metadata store
- filter store

There is also one global sticky-thread environment in `MetadataManager`.

If libmdbx consumes one pthread TLS key per sticky environment, the current constant
`MAX_LIVE_INDICES = 255` is consistent with the code layout:

- `255 * 4 = 1020` per-index sticky environments
- `+1` global metadata environment
- total `1021`, which stays just below a `PTHREAD_KEYS_MAX` of `1024`

On glibc-based systems, `PTHREAD_KEYS_MAX` is a libc build-time constant, so increasing it would
require rebuilding glibc.

## How Eviction Works Today

`evictIfNeeded()` is currently a live-index-count guard. It runs when:

- `createIndex()` is about to create a new live index
- `getIndexEntry()` needs to load a cold index from disk

When eviction runs, it:

1. picks the candidate at the back of `indices_list_`
2. saves the index first if it is dirty
3. marks the cache entry invalid
4. removes it from `indices_`

One subtle but important detail: this is not yet a true inactivity-based or LRU policy.
`indices_list_` is updated on create/load, but not refreshed on search or mutation, and
`last_access` is currently not used anywhere in the eviction path.

In practice, that means eviction is closer to "oldest loaded/created live index first" than
"least recently used index first".

## TODO

1. There is no DRAM-based admission or eviction policy yet. `MAX_ANON_MEM` is only a commented
   placeholder. In practice, the usable memory ceiling should be computed at startup from the
   effective deployment limit:
   cgroup limits, container limits, and host/server memory limits. The way this is discovered will
   also be OS-specific, with different logic needed for Linux and macOS.
2. `MAX_LIVE_INDICES` is a fixed compile-time constant. A better implementation would derive a
   safe cap from the actual runtime environment, for example by checking the system's
   `PTHREAD_KEYS_MAX` value via `getconf PTHREAD_KEYS_MAX`.
3. The server does not currently refuse startup when the machine or container cannot satisfy the
   minimum memory footprint needed to keep the required live indices healthy.
