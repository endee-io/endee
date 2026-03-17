/**
 * Inverted index for sparse vector similarity search.
 *
 * Blocked on-disk layout in MDBX:
 *   key   = pack(term_id, block_nr) as uint64_t integer-key
 *   value = BlockHeader | doc_offsets[n] (uint16) | values[n] (uint8 or float)
 *
 * Metadata rows in the same DBI:
 *   pack(term_id, UINT32_MAX) -> PostingListHeader
 *
 * The key packing keeps all rows for a term contiguous, so scans can seek once
 * to pack(term_id, 0) and walk until term_id changes.
 */

#include "inverted_index.hpp"

#include <atomic>
#include <chrono>
#include <cmath>

namespace ndd {

    namespace {
        template <bool StoreFloats>
        struct PostingValueAccessor;

        template <>
        struct PostingValueAccessor<true> {
            using ValueType = float;

            static inline bool isLive(ValueType value) {
                return value > 0.0f;
            }
        };

        template <>
        struct PostingValueAccessor<false> {
            using ValueType = uint8_t;

            static inline bool isLive(ValueType value) {
                return value > 0;
            }
        };

#ifdef ND_SPARSE_INSTRUMENT
        using SteadyClock = std::chrono::steady_clock;

        inline uint64_t elapsedNsSince(const SteadyClock::time_point& start) {
            return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - start)
                            .count());
        }

        struct SparseSearchDebugStats {
            std::atomic<uint64_t> phase2_iterators_visited{0};
            std::atomic<uint64_t> phase2_iterators_contributed{0};
            std::atomic<uint64_t> parse_current_kv_calls{0};
            std::atomic<uint64_t> parse_current_kv_total_ns{0};
        };

        struct SparseUpdateDebugStats {
            std::atomic<uint64_t> add_batch_calls{0};
            std::atomic<uint64_t> add_batch_docs{0};
            std::atomic<uint64_t> add_batch_terms{0};
            std::atomic<uint64_t> add_batch_raw_updates{0};
            std::atomic<uint64_t> add_batch_deduped_updates{0};
            std::atomic<uint64_t> add_batch_blocks{0};
            std::atomic<uint64_t> build_term_updates_total_ns{0};
            std::atomic<uint64_t> sort_dedup_total_ns{0};
            std::atomic<uint64_t> load_block_calls{0};
            std::atomic<uint64_t> load_block_total_ns{0};
            std::atomic<uint64_t> load_block_entries_total{0};
            std::atomic<uint64_t> merge_block_calls{0};
            std::atomic<uint64_t> merge_block_total_ns{0};
            std::atomic<uint64_t> merge_existing_entries_total{0};
            std::atomic<uint64_t> merge_update_entries_total{0};
            std::atomic<uint64_t> merge_output_entries_total{0};
            std::atomic<uint64_t> save_block_calls{0};
            std::atomic<uint64_t> save_block_total_ns{0};
            std::atomic<uint64_t> save_block_entries_total{0};
            std::atomic<uint64_t> recompute_max_calls{0};
            std::atomic<uint64_t> recompute_max_total_ns{0};
        };

        SparseSearchDebugStats& sparseSearchDebugStats() {
            static SparseSearchDebugStats stats;
            return stats;
        }

        SparseUpdateDebugStats& sparseUpdateDebugStats() {
            static SparseUpdateDebugStats stats;
            return stats;
        }

        class ParseCurrentKVTimer {
        public:
            ParseCurrentKVTimer() :
                start_(SteadyClock::now()) {}

            ~ParseCurrentKVTimer() {
                SparseSearchDebugStats& stats = sparseSearchDebugStats();
                stats.parse_current_kv_calls.fetch_add(1, std::memory_order_relaxed);
                stats.parse_current_kv_total_ns.fetch_add(elapsedNsSince(start_),
                                                          std::memory_order_relaxed);
            }

        private:
            SteadyClock::time_point start_;
        };
#endif // ND_SPARSE_INSTRUMENT
    }  // namespace

#ifdef ND_SPARSE_INSTRUMENT
    void printSparseSearchDebugStats() {
        SparseSearchDebugStats& stats = sparseSearchDebugStats();
        const uint64_t visited = stats.phase2_iterators_visited.exchange(0, std::memory_order_relaxed);
        const uint64_t contributed =
                stats.phase2_iterators_contributed.exchange(0, std::memory_order_relaxed);
        const uint64_t parse_calls = stats.parse_current_kv_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t parse_total_ns =
                stats.parse_current_kv_total_ns.exchange(0, std::memory_order_relaxed);

        LOG_INFO("Sparse search debug stats");
        LOG_INFO("phase3 iterators visited: " << visited);
        LOG_INFO("phase3 iterators contributed: " << contributed);
        LOG_INFO("phase3 contribution rate(%): "
                 << std::fixed << std::setprecision(3)
                 << (visited ? (100.0 * static_cast<double>(contributed) / static_cast<double>(visited))
                             : 0.0));
        LOG_INFO("parseCurrentKV count: " << parse_calls);
        LOG_INFO("parseCurrentKV total(ms): "
                 << std::fixed << std::setprecision(3)
                 << (static_cast<double>(parse_total_ns) / 1'000'000.0));
        LOG_INFO("parseCurrentKV avg(us): "
                 << std::fixed << std::setprecision(3)
                 << (parse_calls ? (static_cast<double>(parse_total_ns) / 1000.0)
                                           / static_cast<double>(parse_calls)
                                 : 0.0));
        std::cout << "=================================\n";
    }

    void printSparseUpdateDebugStats() {
        SparseUpdateDebugStats& stats = sparseUpdateDebugStats();
        const uint64_t add_batch_calls = stats.add_batch_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t add_batch_docs = stats.add_batch_docs.exchange(0, std::memory_order_relaxed);
        const uint64_t add_batch_terms = stats.add_batch_terms.exchange(0, std::memory_order_relaxed);
        const uint64_t add_batch_raw_updates =
                stats.add_batch_raw_updates.exchange(0, std::memory_order_relaxed);
        const uint64_t add_batch_deduped_updates =
                stats.add_batch_deduped_updates.exchange(0, std::memory_order_relaxed);
        const uint64_t add_batch_blocks = stats.add_batch_blocks.exchange(0, std::memory_order_relaxed);
        const uint64_t build_term_updates_total_ns =
                stats.build_term_updates_total_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t sort_dedup_total_ns =
                stats.sort_dedup_total_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t load_block_calls = stats.load_block_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t load_block_total_ns =
                stats.load_block_total_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t load_block_entries_total =
                stats.load_block_entries_total.exchange(0, std::memory_order_relaxed);
        const uint64_t merge_block_calls = stats.merge_block_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t merge_block_total_ns =
                stats.merge_block_total_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t merge_existing_entries_total =
                stats.merge_existing_entries_total.exchange(0, std::memory_order_relaxed);
        const uint64_t merge_update_entries_total =
                stats.merge_update_entries_total.exchange(0, std::memory_order_relaxed);
        const uint64_t merge_output_entries_total =
                stats.merge_output_entries_total.exchange(0, std::memory_order_relaxed);
        const uint64_t save_block_calls = stats.save_block_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t save_block_total_ns =
                stats.save_block_total_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t save_block_entries_total =
                stats.save_block_entries_total.exchange(0, std::memory_order_relaxed);
        const uint64_t recompute_max_calls =
                stats.recompute_max_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t recompute_max_total_ns =
                stats.recompute_max_total_ns.exchange(0, std::memory_order_relaxed);

        LOG_INFO("Sparse update debug stats");
        LOG_INFO("addDocumentsBatchInternal count: " << add_batch_calls);
        LOG_INFO("addDocumentsBatchInternal docs: " << add_batch_docs);
        LOG_INFO("addDocumentsBatchInternal terms: " << add_batch_terms);
        LOG_INFO("addDocumentsBatchInternal raw updates: " << add_batch_raw_updates);
        LOG_INFO("addDocumentsBatchInternal deduped updates: " << add_batch_deduped_updates);
        LOG_INFO("addDocumentsBatchInternal touched blocks: " << add_batch_blocks);
        LOG_INFO("term_updates build total(ms): "
                 << std::fixed << std::setprecision(3)
                 << (static_cast<double>(build_term_updates_total_ns) / 1'000'000.0));
        LOG_INFO("sort+dedup total(ms): "
                 << std::fixed << std::setprecision(3)
                 << (static_cast<double>(sort_dedup_total_ns) / 1'000'000.0));
        LOG_INFO("loadBlockEntries count: " << load_block_calls);
        LOG_INFO("loadBlockEntries total(ms): "
                 << std::fixed << std::setprecision(3)
                 << (static_cast<double>(load_block_total_ns) / 1'000'000.0));
        LOG_INFO("loadBlockEntries avg(us): "
                 << std::fixed << std::setprecision(3)
                 << (load_block_calls
                             ? (static_cast<double>(load_block_total_ns) / 1000.0)
                                       / static_cast<double>(load_block_calls)
                             : 0.0));
        LOG_INFO("loadBlockEntries avg existing entries: "
                 << std::fixed << std::setprecision(3)
                 << (load_block_calls
                             ? static_cast<double>(load_block_entries_total)
                                       / static_cast<double>(load_block_calls)
                             : 0.0));
        LOG_INFO("merge blocks count: " << merge_block_calls);
        LOG_INFO("merge blocks total(ms): "
                 << std::fixed << std::setprecision(3)
                 << (static_cast<double>(merge_block_total_ns) / 1'000'000.0));
        LOG_INFO("merge blocks avg(us): "
                 << std::fixed << std::setprecision(3)
                 << (merge_block_calls
                             ? (static_cast<double>(merge_block_total_ns) / 1000.0)
                                       / static_cast<double>(merge_block_calls)
                             : 0.0));
        LOG_INFO("merge avg existing entries: "
                 << std::fixed << std::setprecision(3)
                 << (merge_block_calls
                             ? static_cast<double>(merge_existing_entries_total)
                                       / static_cast<double>(merge_block_calls)
                             : 0.0));
        LOG_INFO("merge avg update entries: "
                 << std::fixed << std::setprecision(3)
                 << (merge_block_calls
                             ? static_cast<double>(merge_update_entries_total)
                                       / static_cast<double>(merge_block_calls)
                             : 0.0));
        LOG_INFO("merge avg output entries: "
                 << std::fixed << std::setprecision(3)
                 << (merge_block_calls
                             ? static_cast<double>(merge_output_entries_total)
                                       / static_cast<double>(merge_block_calls)
                             : 0.0));
        LOG_INFO("saveBlockEntries count: " << save_block_calls);
        LOG_INFO("saveBlockEntries total(ms): "
                 << std::fixed << std::setprecision(3)
                 << (static_cast<double>(save_block_total_ns) / 1'000'000.0));
        LOG_INFO("saveBlockEntries avg(us): "
                 << std::fixed << std::setprecision(3)
                 << (save_block_calls
                             ? (static_cast<double>(save_block_total_ns) / 1000.0)
                                       / static_cast<double>(save_block_calls)
                             : 0.0));
        LOG_INFO("saveBlockEntries avg entries: "
                 << std::fixed << std::setprecision(3)
                 << (save_block_calls
                             ? static_cast<double>(save_block_entries_total)
                                       / static_cast<double>(save_block_calls)
                             : 0.0));
        LOG_INFO("recomputeGlobalMax count: " << recompute_max_calls);
        LOG_INFO("recomputeGlobalMax total(ms): "
                 << std::fixed << std::setprecision(3)
                 << (static_cast<double>(recompute_max_total_ns) / 1'000'000.0));
        LOG_INFO("recomputeGlobalMax avg(us): "
                 << std::fixed << std::setprecision(3)
                 << (recompute_max_calls
                             ? (static_cast<double>(recompute_max_total_ns) / 1000.0)
                                       / static_cast<double>(recompute_max_calls)
                             : 0.0));
        std::cout << "=================================\n";
    }
#else
    void printSparseSearchDebugStats() {}
    void printSparseUpdateDebugStats() {}
#endif // ND_SPARSE_INSTRUMENT

    InvertedIndex::InvertedIndex(MDBX_env* env,
                                 size_t vocab_size,
                                 const std::string& index_id,
                                 ndd::SparseScoringModel sparse_model)
        : env_(env),
          blocked_term_postings_dbi_(0),
          vocab_size_(vocab_size),
          index_id_(index_id),
          sparse_model_(sparse_model) {}

    void InvertedIndex::applyHeaderDelta(PostingListHeader& header,
                                        int64_t total_delta,
                                        int64_t live_delta) {
        int64_t new_total = static_cast<int64_t>(header.nr_entries) + total_delta;
        int64_t new_live = static_cast<int64_t>(header.nr_live_entries) + live_delta;

        if (new_total < 0) new_total = 0;
        if (new_live < 0) new_live = 0;
        if (new_live > new_total) new_live = new_total;

        header.nr_entries = static_cast<uint32_t>(new_total);
        header.nr_live_entries = static_cast<uint32_t>(new_live);
    }

    bool InvertedIndex::validateSuperBlock(MDBX_txn* txn) {
        SuperBlock sb;
        bool sb_found = false;
        if (!readSuperBlock(txn, &sb, &sb_found)) {
            return false;
        }

        if (!sb_found) {
            // Check whether the DBI already has data (legacy DB without superblock).
            MDBX_stat stat;
            int rc = mdbx_dbi_stat(txn, blocked_term_postings_dbi_, &stat, sizeof(stat));
            if (rc == MDBX_SUCCESS && stat.ms_entries > 0) {
                LOG_ERROR(2201,
                          index_id_,
                          "Sparse index database exists without a superblock; it was created by an older incompatible version");
                throw std::runtime_error(
                    "Incompatible sparse index: database has no superblock (legacy format)");
            }

            // Fresh database — write the superblock.
            sb.format_version = settings::SPARSE_ONDISK_VERSION;
            LOG_INFO(2202,
                     index_id_,
                     "Writing fresh sparse superblock (version="
                             << static_cast<int>(settings::SPARSE_ONDISK_VERSION) << ")");
            if (!writeSuperBlock(txn, sb)) {
                return false;
            }
            return true;
        }

        if (sb.format_version != settings::SPARSE_ONDISK_VERSION) {
            LOG_ERROR(2203,
                      index_id_,
                      "Sparse index format version mismatch: on-disk="
                              << static_cast<int>(sb.format_version)
                              << " compiled=" << static_cast<int>(settings::SPARSE_ONDISK_VERSION));
            throw std::runtime_error(
                "Incompatible sparse index: format version "
                + std::to_string(sb.format_version)
                + " does not match compiled version "
                + std::to_string(settings::SPARSE_ONDISK_VERSION));
        }

        return true;
    }

    bool InvertedIndex::initialize() {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2204, index_id_, "Failed to begin sparse index init transaction: " << mdbx_strerror(rc));
            return false;
        }

        rc = mdbx_dbi_open(txn,
                            "blocked_term_postings",
                            MDBX_CREATE | MDBX_INTEGERKEY,
                            &blocked_term_postings_dbi_);
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2205, index_id_, "Failed to open blocked_term_postings DBI: " << mdbx_strerror(rc));
            mdbx_txn_abort(txn);
            return false;
        }

        if (!validateSuperBlock(txn)) {
            mdbx_txn_abort(txn);
            return false;
        }

        rc = mdbx_txn_commit(txn);
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2206, index_id_, "Failed to commit sparse index init transaction: " << mdbx_strerror(rc));
            return false;
        }

        if (!loadTermInfo()) {
            return false;
        }

        LOG_INFO(2207, index_id_, "Sparse index initialized with " << term_info_.size() << " loaded terms");
        return true;
    }

    bool InvertedIndex::addDocumentsBatch(
        MDBX_txn* txn,
        const std::vector<std::pair<ndd::idInt, SparseVector>>& docs)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return addDocumentsBatchInternal(txn, docs);
    }

    bool InvertedIndex::removeDocument(MDBX_txn* txn,
                                    ndd::idInt doc_id,
                                    const SparseVector& vec)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return removeDocumentInternal(txn, doc_id, vec);
    }

    size_t InvertedIndex::getTermCount() const {
        return term_info_.size();
    }

    size_t InvertedIndex::getVocabSize() const {
        return vocab_size_;
    }

    std::vector<std::pair<ndd::idInt, float>>
    InvertedIndex::search(const SparseVector& query,
                        size_t k,
                        const ndd::RoaringBitmap* filter)
    {
        return search(query, k, 0, filter);
    }

    //log(1 + (N - df + 0.5)/(df + 0.5))
    float InvertedIndex::get_IDF(size_t total_nr_docs, size_t nr_live_docs_with_term) {
        if (total_nr_docs == 0) {
            return 0.0f;
        }

        const size_t clamped_df = std::min(total_nr_docs, nr_live_docs_with_term);
        const double total_docs = static_cast<double>(total_nr_docs);
        const double doc_freq = static_cast<double>(clamped_df);
        const double ratio = (total_docs - doc_freq + 0.5) / (doc_freq + 0.5);
        return static_cast<float>(std::log(1.0 + ratio));
    }

#if 0
    /**
     * There are many implementations of IDF.
     * We can make a library of implementations later.
     */
    float InvertedIndex::get_IDF(size_t total_nr_docs, size_t nr_live_docs_with_term) {
        return 1;
        if (total_nr_docs == 0) {
            return 0.0f;
        }

        const size_t clamped_df = std::min(total_nr_docs, nr_live_docs_with_term);
        const double total_docs = static_cast<double>(total_nr_docs);

        return std::log(total_docs + 1) - std::log(clamped_df + 0.5);
    }
#endif //if 0

    template <bool StoreFloats>
    bool InvertedIndex::accumulateBatchScores(PostingListIterator* it,
                                                ndd::idInt batch_start,
                                                uint32_t batch_end_block_nr,
                                                BlockOffset batch_end_block_offset,
                                                float* scores_buf,
                                                float term_weight)
    {
        using Accessor = PostingValueAccessor<StoreFloats>;
        using ValueType = typename Accessor::ValueType;

        const BlockOffset* offsets = it->doc_offsets;
        const ValueType* vals = static_cast<const ValueType*>(it->values_ptr);
        uint32_t idx = it->current_entry_idx;
        uint32_t sz = it->data_size;
        float block_max_value = it->max_value;
        bool contributed = false;

        while (true) {
            if (it->current_block_nr > batch_end_block_nr) {
                break;
            }

            const bool consume_full_block = it->current_block_nr < batch_end_block_nr;
            const int64_t local_base =
                    static_cast<int64_t>(it->currentBlockBaseDocId()) - static_cast<int64_t>(batch_start);
            const uint32_t before = idx;
            while (idx < sz && (consume_full_block || offsets[idx] <= batch_end_block_offset)) {
                const ValueType value = vals[idx];
                if (Accessor::isLive(value)) {
                    const size_t local = static_cast<size_t>(local_base + offsets[idx]);
                    if constexpr (StoreFloats) {
                        scores_buf[local] += value * term_weight;
                    } else {
                        scores_buf[local] += InvertedIndex::dequantize(value, block_max_value) * term_weight;
                    }
                    contributed = true;
                }
                idx++;
            }
            it->consumeEntries(idx - before);

            if (idx < sz) {
                break;
            }

            it->current_entry_idx = idx;
            if (!it->loadNextBlock()) {
                break;
            }

            offsets = it->doc_offsets;
            vals = static_cast<const ValueType*>(it->values_ptr);
            block_max_value = it->max_value;
            idx = 0;
            sz = it->data_size;

            if (it->current_block_nr > batch_end_block_nr
                || (it->current_block_nr == batch_end_block_nr
                && sz > 0
                && offsets[0] > batch_end_block_offset))
            {
                break;
            }
        }

        it->current_entry_idx = idx;
        it->advanceToNextLive();
        return contributed;
    }

    std::vector<std::pair<ndd::idInt, float>>
    InvertedIndex::search(const SparseVector& query,
                        size_t k,
                        size_t total_nr_docs,
                        const ndd::RoaringBitmap* filter)
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2208, index_id_, "Failed to begin sparse search transaction: " << mdbx_strerror(rc));
            return {};
        }

        if (query.empty() || k == 0) {
            mdbx_txn_abort(txn);
            return {};
        }

        std::vector<PostingListIterator> iters_storage;
        std::vector<PostingListIterator*> iters;
        std::vector<MDBX_cursor*> cursors;

        iters_storage.reserve(query.indices.size());
        iters.reserve(query.indices.size());
        cursors.reserve(query.indices.size());

        {
            LOG_TIME("search phase 1");

        // Build one iterator per live query term. Each iterator owns a cursor and lazily
        // streams the term's block rows instead of pulling the whole posting list in memory.
        for (size_t qi = 0; qi < query.indices.size(); qi++) {
            uint32_t term_id = query.indices[qi];
            if (term_id == kMetadataTermId) continue;

            float qw = query.values[qi];
            if (qw <= 0.0f) continue;


            auto info_it = term_info_.find(term_id);
            if (info_it == term_info_.end()) {
                LOG_WARN(2209, index_id_, "Search skipped unknown query term_id=" << term_id);
                continue;
            }

            bool header_found = false;
            PostingListHeader header = readPostingListHeader(txn, term_id, &header_found);
            if (!header_found || header.nr_entries == 0 || header.nr_live_entries == 0) {
                continue;
            }

            float term_weight = qw;
            if (sparse_model_ == ndd::SparseScoringModel::ENDEE_BM25) {
                term_weight *= get_IDF(total_nr_docs, header.nr_live_entries);
            }

            MDBX_cursor* cursor = nullptr;
            rc = mdbx_cursor_open(txn, blocked_term_postings_dbi_, &cursor);
            if (rc != MDBX_SUCCESS) {
                LOG_ERROR(2210,
                          index_id_,
                          "Failed to open sparse search cursor for term "
                                  << term_id << ": " << mdbx_strerror(rc));
                continue;
            }

            PostingListIterator it;
            it.init(cursor,
                    term_id,
                    term_weight,
                    info_it->second,
                    header.nr_entries,
                    this);

            if (it.current_doc_id != EXHAUSTED_DOC_ID) {
                iters_storage.push_back(it);
                cursors.push_back(cursor);
            } else {
                mdbx_cursor_close(cursor);
            }
        }

        for (size_t i = 0; i < iters_storage.size(); i++) {
            iters.push_back(&iters_storage[i]);
        }

        if (iters.empty()) {
            mdbx_txn_abort(txn);
            return {};
        }

        //END OF PHASE 1
        }
        

        bool use_pruning = (iters.size() > 1);
        float best_min_score = 0.0f;

        std::vector<float> scores_buf(settings::INV_IDX_SEARCH_BATCH_SZ, 0.0f);
        std::priority_queue<ScoredDoc> top_results;
        float threshold = 0.0f;

        auto minIterDocId = [&iters]() -> ndd::idInt {
            ndd::idInt min_id = EXHAUSTED_DOC_ID;
            for (size_t i = 0; i < iters.size(); i++) {
                if (iters[i]->current_doc_id < min_id) {
                    min_id = iters[i]->current_doc_id;
                }
            }
            return min_id;
        };

        ndd::idInt min_id = minIterDocId();

        // Process the index in doc-id windows. The accumulator is dense within the current
        // window even though the posting lists themselves stay sparse and block-based.
        while (min_id != EXHAUSTED_DOC_ID) {
            ndd::idInt batch_start = min_id;
            ndd::idInt batch_end = batch_start
                                + (ndd::idInt)settings::INV_IDX_SEARCH_BATCH_SZ - 1;
            if (batch_end < batch_start) {
                batch_end = EXHAUSTED_DOC_ID - 1;
            }
            const uint32_t batch_end_block_nr = docToBlockNr(batch_end);
            const BlockOffset batch_end_block_offset = docToBlockOffset(batch_end);

            size_t batch_len = (size_t)(batch_end - batch_start) + 1;
            if (batch_len > scores_buf.size()) {
                scores_buf.resize(batch_len);
            }
            std::memset(scores_buf.data(), 0, batch_len * sizeof(float));

            {
            LOG_TIME("search phase 2");
            // Consume all postings that fall into this batch. The iterator keeps absolute doc_ids
            // implicit as (current_block_nr, doc_offsets[idx]) to avoid rebuilding them eagerly.
            for (size_t i = 0; i < iters.size(); i++) {
                PostingListIterator* it = iters[i];
#ifdef ND_SPARSE_INSTRUMENT
                sparseSearchDebugStats().phase2_iterators_visited.fetch_add(1, std::memory_order_relaxed);
#endif // ND_SPARSE_INSTRUMENT
                if (it->current_doc_id > batch_end) {
                    continue;
                }
                [[maybe_unused]] const bool phase3_contributed =
#if defined(NDD_INV_IDX_STORE_FLOATS)
                        accumulateBatchScores<true>(
                                it,
                                batch_start,
                                batch_end_block_nr,
                                batch_end_block_offset,
                                scores_buf.data(),
                                it->term_weight);
#else
                        accumulateBatchScores<false>(
                                it,
                                batch_start,
                                batch_end_block_nr,
                                batch_end_block_offset,
                                scores_buf.data(),
                                it->term_weight);
#endif // NDD_INV_IDX_STORE_FLOATS

#ifdef ND_SPARSE_INSTRUMENT
                if (phase3_contributed) {
                    sparseSearchDebugStats().phase2_iterators_contributed.fetch_add(
                            1, std::memory_order_relaxed);
                }
#endif // ND_SPARSE_INSTRUMENT
            }
            //END OF SEARCH PHASE 2
            }

            {
                LOG_TIME("search phase 3");
            // Only scores inside the current batch can be non-zero, so convert that temporary
            // dense buffer into top-k candidates before moving to the next window.
            for (size_t local = 0; local < batch_len; local++) {
                float s = scores_buf[local];
                if (s == 0.0f || s <= threshold) continue;

                ndd::idInt doc_id = batch_start + (ndd::idInt)local;
                if (filter && !filter->contains(doc_id)) continue;

                if (top_results.size() < k) {
                    top_results.emplace(doc_id, s);
                    if (top_results.size() == k) {
                        threshold = top_results.top().score;
                    }
                } else if (s > threshold) {
                    top_results.pop();
                    top_results.emplace(doc_id, s);
                    threshold = top_results.top().score;
                }
            }
            //END OF SEARCH PHASE 3
            }

            {
                LOG_TIME("search phase 4");
            // Compact away exhausted iterators, then optionally prune the longest remaining list
            // when its best possible future contribution cannot beat the current threshold.
            size_t write_idx = 0;
            for (size_t i = 0; i < iters.size(); i++) {
                if (iters[i]->current_doc_id != EXHAUSTED_DOC_ID) {
                    iters[write_idx++] = iters[i];
                }
            }
            iters.resize(write_idx);
            if (iters.empty()) break;

            min_id = minIterDocId();

            if (use_pruning && top_results.size() >= k) {
                float new_min_score = threshold;
                if (!nearEqual(new_min_score, best_min_score)) {
                    best_min_score = new_min_score;
                    pruneLongest(iters, new_min_score);
                    min_id = minIterDocId();
                }
            }
            //END OF SEARCH PHASE 4
            }
        }

#ifdef NDD_INV_IDX_PRUNE_DEBUG
        for (const PostingListIterator& it : iters_storage) {
            LOG_INFO(2229,
                     index_id_,
                     "Sparse prune stats: term_id=" << it.term_id
                                                   << " posting_list_len=" << it.initial_entries
                                                   << " pruned_len=" << it.pruned_entries);
        }
#endif // NDD_INV_IDX_PRUNE_DEBUG

        for (MDBX_cursor* cursor : cursors) {
            mdbx_cursor_close(cursor);
        }
        mdbx_txn_abort(txn);

        std::vector<std::pair<ndd::idInt, float>> results;
        results.reserve(top_results.size());
        while (!top_results.empty()) {
            results.push_back(
                std::make_pair(top_results.top().doc_id, top_results.top().score));
            top_results.pop();
        }
        std::reverse(results.begin(), results.end());
        return results;
    }

    inline uint8_t InvertedIndex::quantize(float val, float max_val) {
        if (max_val <= settings::NEAR_ZERO)
            return 0;

        float scaled = (val / max_val) * UINT8_MAX;
        if (scaled >= UINT8_MAX)
            return UINT8_MAX;
        if (scaled <= 0.0f)
            return 0;

        uint8_t result = (uint8_t)(scaled + 0.5f);

        /**
         * Since a 0 weight is considered deleted,
         * we change it to 1
        */
        return result == 0 ? 1 : result;
    }

    inline float InvertedIndex::dequantize(uint8_t val, float max_val) {
        if (max_val <= settings::NEAR_ZERO)
            return 0.0f;
        return (float)val * (max_val / UINT8_MAX);
    }

    // =========================================================================
    // SIMD helpers
    // =========================================================================

    size_t InvertedIndex::findDocIdSIMD(const uint32_t* doc_ids,
                                    size_t size,
                                    size_t start_idx,
                                    uint32_t target) const
    {
        size_t idx = start_idx;

#if defined(USE_AVX512)
        const size_t simd_width = 16;
        __m512i target_vec = _mm512_set1_epi32((int)target);

        while (idx + simd_width <= size) {
            __m512i data_vec = _mm512_loadu_si512(doc_ids + idx);
            __mmask16 mask = _mm512_cmpge_epu32_mask(data_vec, target_vec);

            if (mask != 0) {
                return idx + __builtin_ctz(mask);
            }
            idx += simd_width;
        }
#elif defined(USE_AVX2)
        const size_t simd_width = 8;
        __m256i target_vec = _mm256_set1_epi32((int)target);

        while (idx + simd_width <= size) {
            __builtin_prefetch(doc_ids + idx + 32);
            if (doc_ids[idx + simd_width - 1] < target) {
                idx += simd_width;
                continue;
            }

            __m256i data_vec =
                _mm256_loadu_si256((const __m256i*)(doc_ids + idx));
            __m256i max_vec = _mm256_max_epu32(data_vec, target_vec);
            __m256i cmp = _mm256_cmpeq_epi32(max_vec, data_vec);

            int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp));
            if (mask != 0) {
                return idx + __builtin_ctz(mask);
            }
            idx += simd_width;
        }
#elif defined(USE_SVE2)
        svbool_t pg = svwhilelt_b32(idx, size);
        svuint32_t target_vec = svdup_u32(target);

        while (svptest_any(svptrue_b32(), pg)) {
            svuint32_t data_vec = svld1_u32(pg, doc_ids + idx);
            svbool_t cmp = svcmpge_u32(pg, data_vec, target_vec);

            if (svptest_any(pg, cmp)) {
                svbool_t before_match = svbrkb_z(pg, cmp);
                uint64_t count = svcntp_b32(pg, before_match);
                return idx + count;
            }
            idx += svcntw();
            pg = svwhilelt_b32(idx, size);
        }
        return idx;
#elif defined(USE_NEON)
        const size_t simd_width = 4;
        uint32x4_t target_vec = vdupq_n_u32(target);

        while (idx + simd_width <= size) {
            uint32x4_t data_vec = vld1q_u32(doc_ids + idx);
            uint32x4_t cmp = vcgeq_u32(data_vec, target_vec);

            if (vmaxvq_u32(cmp) != 0) {
                for (size_t i = 0; i < simd_width; i++) {
                    if (doc_ids[idx + i] >= target) {
                        return idx + i;
                    }
                }
            }
            idx += simd_width;
        }
#endif // USE_AVX512

        while (idx < size && doc_ids[idx] < target) {
            idx++;
        }
        return idx;
    }

    size_t InvertedIndex::findNextLiveSIMD(const uint8_t* values,
                                        size_t size,
                                        size_t start_idx) const
    {
        size_t idx = start_idx;

#if defined(USE_AVX512)
        const size_t simd_width = 64;
        __m512i zero_vec = _mm512_setzero_si512();

        while (idx + simd_width <= size) {
            __m512i data_vec = _mm512_loadu_si512(values + idx);
            __mmask64 mask = _mm512_cmpneq_epu8_mask(data_vec, zero_vec);

            if (mask != 0) {
                return idx + __builtin_ctzll(mask);
            }
            idx += simd_width;
        }
#elif defined(USE_AVX2)
        const size_t simd_width = 32;
        __m256i zero_vec = _mm256_setzero_si256();

        while (idx + simd_width <= size) {
            __m256i data_vec =
                _mm256_loadu_si256((const __m256i*)(values + idx));
            __m256i cmp = _mm256_cmpeq_epi8(data_vec, zero_vec);
            int mask = _mm256_movemask_epi8(cmp);

            if ((uint32_t)mask != 0xFFFFFFFF) {
                return idx + __builtin_ctz(~mask);
            }
            idx += simd_width;
        }
#elif defined(USE_NEON)
        const size_t simd_width = 16;
        uint8x16_t zero_vec = vdupq_n_u8(0);

        while (idx + simd_width <= size) {
            uint8x16_t data_vec = vld1q_u8(values + idx);
            uint8x16_t cmp = vceqq_u8(data_vec, zero_vec);

            if (vminvq_u8(cmp) == 0) {
                for (size_t i = 0; i < simd_width; i++) {
                    if (values[idx + i] != 0) {
                        return idx + i;
                    }
                }
            }
            idx += simd_width;
        }
#elif defined(USE_SVE2)
        svbool_t pg = svwhilelt_b8(idx, size);
        while (svptest_any(svptrue_b8(), pg)) {
            svuint8_t data_vec = svld1_u8(pg, values + idx);
            svbool_t cmp = svcmpne_n_u8(pg, data_vec, 0);

            if (svptest_any(pg, cmp)) {
                svbool_t before_match = svbrkb_z(pg, cmp);
                return idx + svcntp_b8(pg, before_match);
            }
            idx += svcntb();
            pg = svwhilelt_b8(idx, size);
        }
        return idx;
#endif // USE_AVX512

        while (idx < size) {
            if (values[idx] != 0) return idx;
            idx++;
        }
        return idx;
    }

    // =========================================================================
    // Superblock helpers
    // =========================================================================

    bool InvertedIndex::readSuperBlock(MDBX_txn* txn,
                                       SuperBlock* out,
                                       bool* out_found) const {
        if (out_found) *out_found = false;

        uint64_t packed = packPostingKey(kMetadataTermId, kSuperBlockBlockNr);
        MDBX_val key{&packed, sizeof(packed)};
        MDBX_val data;

        int rc = mdbx_get(txn, blocked_term_postings_dbi_, &key, &data);
        if (rc == MDBX_NOTFOUND) {
            return true;
        }
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2211, index_id_, "readSuperBlock MDBX lookup failed: " << mdbx_strerror(rc));
            return false;
        }
        if (data.iov_len < sizeof(SuperBlock)) {
            LOG_ERROR(2212, index_id_, "Corrupt sparse superblock: payload too small");
            return false;
        }

        std::memcpy(out, data.iov_base, sizeof(SuperBlock));
        if (out_found) *out_found = true;
        return true;
    }

    bool InvertedIndex::writeSuperBlock(MDBX_txn* txn, const SuperBlock& sb) {
        uint64_t packed = packPostingKey(kMetadataTermId, kSuperBlockBlockNr);
        MDBX_val key{&packed, sizeof(packed)};
        MDBX_val data{const_cast<SuperBlock*>(&sb), sizeof(SuperBlock)};

        int rc = mdbx_put(txn, blocked_term_postings_dbi_, &key, &data, MDBX_UPSERT);
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2213, index_id_, "writeSuperBlock MDBX put failed: " << mdbx_strerror(rc));
            return false;
        }
        return true;
    }

    // =========================================================================
    // Metadata and block helpers
    // =========================================================================

    PostingListHeader InvertedIndex::readPostingListHeader(MDBX_txn* txn,
                                                            uint32_t term_id,
                                                            bool* out_found) const {
        PostingListHeader header;
        if (out_found) *out_found = false;

        if (term_id == kMetadataTermId) {
            return header;
        }

        uint64_t packed = packPostingKey(term_id, kMetadataBlockNr);
        MDBX_val key{&packed, sizeof(packed)};
        MDBX_val data;

        int rc = mdbx_get(txn, blocked_term_postings_dbi_, &key, &data);
        if (rc == MDBX_SUCCESS && data.iov_len >= sizeof(PostingListHeader)) {
            std::memcpy(&header, data.iov_base, sizeof(PostingListHeader));
            if (out_found) *out_found = true;
        }

        return header;
    }

    bool InvertedIndex::writePostingListHeader(MDBX_txn* txn,
                                            uint32_t term_id,
                                            const PostingListHeader& header) {
        if (term_id == kMetadataTermId) {
            LOG_ERROR(2214, index_id_, "Refusing to write a posting-list header for the reserved metadata term");
            return false;
        }

        uint64_t packed = packPostingKey(term_id, kMetadataBlockNr);
        MDBX_val key{&packed, sizeof(packed)};
        MDBX_val data{const_cast<PostingListHeader*>(&header), sizeof(PostingListHeader)};

        int rc = mdbx_put(txn, blocked_term_postings_dbi_, &key, &data, MDBX_UPSERT);
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2215,
                      index_id_,
                      "Failed to write posting-list header for term "
                              << term_id << ": " << mdbx_strerror(rc));
            return false;
        }

        return true;
    }

    bool InvertedIndex::deletePostingListHeader(MDBX_txn* txn, uint32_t term_id) {
        uint64_t packed = packPostingKey(term_id, kMetadataBlockNr);
        MDBX_val key{&packed, sizeof(packed)};

        int rc = mdbx_del(txn, blocked_term_postings_dbi_, &key, nullptr);
        return rc == MDBX_SUCCESS || rc == MDBX_NOTFOUND;
    }

    bool InvertedIndex::parseBlockViewFromValue(const MDBX_val& data,
                                                uint32_t block_nr,
                                                BlockView* out_view) const {
        if (!out_view) return false;
        if (data.iov_len < sizeof(BlockHeader)) return false;

        const BlockHeader* header = (const BlockHeader*)data.iov_base;
        uint32_t n = header->nr_entries;

        const uint8_t* ptr = (const uint8_t*)data.iov_base + sizeof(BlockHeader);
        const BlockOffset* doc_offsets = reinterpret_cast<const BlockOffset*>(ptr);
        ptr += n * sizeof(BlockOffset);

#if defined(NDD_INV_IDX_STORE_FLOATS)
        uint8_t vbits = 32;
        const void* values = ptr;
        size_t required = sizeof(BlockHeader)
                        + n * sizeof(BlockOffset)
                        + n * sizeof(float);
#else
        uint8_t vbits = 8;
        const void* values = ptr;
        size_t required = sizeof(BlockHeader)
                        + n * sizeof(BlockOffset)
                        + n * sizeof(uint8_t);
#endif // NDD_INV_IDX_STORE_FLOATS

        if (data.iov_len < required) {
            LOG_ERROR(2216, index_id_, "Corrupt sparse block payload: fewer bytes than expected");
            return false;
        }

        out_view->doc_offsets = doc_offsets;
        out_view->values = values;
        out_view->count = n;
        out_view->value_bits = vbits;
        out_view->max_value = header->max_value;
        return true;
    }

    bool InvertedIndex::loadBlockEntries(MDBX_txn* txn,
                                        uint32_t term_id,
                                        uint32_t block_nr,
                                        std::vector<PostingListEntry>* entries,
                                        uint32_t* out_live_in_block,
                                        float* out_max_value,
                                        bool* out_found) const
    {
        if (entries) entries->clear();
        if (out_live_in_block) *out_live_in_block = 0;
        if (out_max_value) *out_max_value = 0.0f;
        if (out_found) *out_found = false;

        if (term_id == kMetadataTermId || block_nr == kMetadataBlockNr) {
            return false;
        }

        uint64_t packed = packPostingKey(term_id, block_nr);
        MDBX_val key{&packed, sizeof(packed)};
        MDBX_val data;

        int rc = mdbx_get(txn, blocked_term_postings_dbi_, &key, &data);
        if (rc == MDBX_NOTFOUND) {
            return true;
        }
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2217,
                      index_id_,
                      "loadBlockEntries MDBX lookup failed for term "
                              << term_id << " block " << block_nr << ": " << mdbx_strerror(rc));
            return false;
        }

        BlockView view;
        if (!parseBlockViewFromValue(data, block_nr, &view)) {
            LOG_ERROR(2218, index_id_, "Corrupt block payload for term " << term_id << " block " << block_nr);
            return false;
        }

        const BlockHeader* header = (const BlockHeader*)data.iov_base;
        if (out_live_in_block) *out_live_in_block = header->nr_live_in_block;
        if (out_max_value) *out_max_value = header->max_value;
        if (out_found) *out_found = true;

        if (!entries) {
            return true;
        }

        // Update/delete paths want a mutable decoded representation, so convert offsets back to
        // absolute doc_ids here. Search stays zero-copy and does not call this helper.
        entries->resize(view.count);
#if defined(NDD_INV_IDX_STORE_FLOATS)
        const float* vals = (const float*)view.values;
#else
        const uint8_t* vals = (const uint8_t*)view.values;
#endif // NDD_INV_IDX_STORE_FLOATS

        for (uint32_t i = 0; i < view.count; i++) {
            entries->at(i).doc_id = blockOffsetToDocId(block_nr, view.doc_offsets[i]);
#if defined(NDD_INV_IDX_STORE_FLOATS)
            entries->at(i).value = vals[i];
#else
            entries->at(i).value = dequantize(vals[i], header->max_value);
#endif // NDD_INV_IDX_STORE_FLOATS
        }

        return true;
    }

    /**
     * Saves the block header and entries
     */
    bool InvertedIndex::saveBlockEntries(MDBX_txn* txn,
                                        uint32_t term_id,
                                        uint32_t block_nr,
                                        const std::vector<PostingListEntry>& entries,
                                        uint32_t live_in_block,
                                        float max_val)
    {
        if (term_id == kMetadataTermId || block_nr == kMetadataBlockNr) {
            LOG_ERROR(2219, index_id_, "Refusing to save a reserved metadata key as a sparse data block");
            return false;
        }

        if (entries.empty()) {
            return deleteBlock(txn, term_id, block_nr);
        }

        if (entries.size() > kBlockCapacity) {
            LOG_ERROR(2220,
                      index_id_,
                      "Block for term " << term_id << " block " << block_nr
                                        << " exceeds fixed capacity " << kBlockCapacity);
            return false;
        }

        BlockHeader header;
        header.nr_entries = (uint16_t)entries.size();
        header.nr_live_in_block = (uint16_t)live_in_block;
        header.max_value = max_val;

#if defined(NDD_INV_IDX_STORE_FLOATS)
        size_t value_size = sizeof(float);
#else
        size_t value_size = sizeof(uint8_t);
#endif // NDD_INV_IDX_STORE_FLOATS

        size_t total_size = sizeof(BlockHeader)
                            + (entries.size() * sizeof(BlockOffset)) //doc-local offsets
                            + (entries.size() * value_size); //doc weights
        std::vector<uint8_t> buffer(total_size);

        // Serialize back into the compact on-disk layout used by the search iterator.
        std::memcpy(buffer.data(), &header, sizeof(BlockHeader));

        uint8_t* ptr = buffer.data() + sizeof(BlockHeader);
        BlockOffset* offsets_out = reinterpret_cast<BlockOffset*>(ptr);
        ptr += entries.size() * sizeof(BlockOffset);

        BlockOffset prev_offset = 0;
        bool has_prev = false;
        for (size_t i = 0; i < entries.size(); i++) {
            if (docToBlockNr(entries[i].doc_id) != block_nr) {
                LOG_ERROR(2221,
                          index_id_,
                          "Entry doc_id " << entries[i].doc_id << " does not belong to term "
                                          << term_id << " block " << block_nr);
                return false;
            }

            BlockOffset offset = docToBlockOffset(entries[i].doc_id);
            if (has_prev && offset <= prev_offset) {
                LOG_ERROR(2222, index_id_, "Block entries must be strictly sorted by doc offset");
                return false;
            }
            offsets_out[i] = offset;
            prev_offset = offset;
            has_prev = true;
        }

#if defined(NDD_INV_IDX_STORE_FLOATS)
        float* vals_out = (float*)ptr;
        for (size_t i = 0; i < entries.size(); i++) {
            vals_out[i] = entries[i].value;
        }
#else
        uint8_t* vals_out = ptr;
        for (size_t i = 0; i < entries.size(); i++) {
            vals_out[i] = quantize(entries[i].value, max_val);
        }
#endif // NDD_INV_IDX_STORE_FLOATS

        uint64_t packed = packPostingKey(term_id, block_nr);
        MDBX_val key{&packed, sizeof(packed)};
        MDBX_val value{buffer.data(), buffer.size()};

        int rc = mdbx_put(txn, blocked_term_postings_dbi_, &key, &value, MDBX_UPSERT);
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2223,
                      index_id_,
                      "saveBlockEntries MDBX put failed for term "
                              << term_id << " block " << block_nr << ": " << mdbx_strerror(rc));
            return false;
        }

        return true;
    }

    bool InvertedIndex::deleteBlock(MDBX_txn* txn, uint32_t term_id, uint32_t block_nr) {
        uint64_t packed = packPostingKey(term_id, block_nr);
        MDBX_val key{&packed, sizeof(packed)};

        int rc = mdbx_del(txn, blocked_term_postings_dbi_, &key, nullptr);
        return rc == MDBX_SUCCESS || rc == MDBX_NOTFOUND;
    }

    bool InvertedIndex::iterateTermBlocks(
        MDBX_txn* txn,
        uint32_t term_id,
        const std::function<bool(uint32_t block_nr, const MDBX_val& data)>& callback) const {
        // Because keys are packed as (term_id, block_nr), all rows for one term are contiguous.
        // A single seek is enough to walk every block that belongs to that term.
        MDBX_cursor* cursor = nullptr;
        int rc = mdbx_cursor_open(txn, blocked_term_postings_dbi_, &cursor);
        if (rc != MDBX_SUCCESS) {
            return false;
        }

        uint64_t seek_packed = packPostingKey(term_id, 0);
        MDBX_val key{&seek_packed, sizeof(seek_packed)};
        MDBX_val data;

        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
        while (rc == MDBX_SUCCESS) {
            if (key.iov_len != sizeof(uint64_t)) {
                break;
            }

            uint64_t packed_key;
            std::memcpy(&packed_key, key.iov_base, sizeof(uint64_t));
            uint32_t key_term = unpackTermId(packed_key);
            uint32_t block_nr = unpackBlockNr(packed_key);

            if (key_term != term_id) {
                break;
            }

            if (block_nr == kMetadataBlockNr) {
                break;
            }

            if (!callback(block_nr, data)) {
                mdbx_cursor_close(cursor);
                return false;
            }

            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
        }

        mdbx_cursor_close(cursor);
        return true;
    }

    float InvertedIndex::recomputeGlobalMaxFromBlocks(MDBX_txn* txn, uint32_t term_id) const {
        float recomputed_max = 0.0f;

        // Only needed when the previous global max may have been lowered by an in-place update
        // or delete. We then rescan block headers to find the true max for the term.
        bool ok = iterateTermBlocks(txn,
                                    term_id,
                                    [&recomputed_max](uint32_t block_nr, const MDBX_val& data) {
                                        if (data.iov_len < sizeof(BlockHeader)) {
                                            return false;
                                        }
                                        const BlockHeader* header =
                                            (const BlockHeader*)data.iov_base;
                                        if (header->max_value > recomputed_max) {
                                            recomputed_max = header->max_value;
                                        }
                                        return true;
                                    });

        if (!ok) {
            return 0.0f;
        }

        return recomputed_max;
    }

    // =========================================================================
    // Startup
    // =========================================================================

    bool InvertedIndex::loadTermInfo() {
        term_info_.clear();

        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if (rc != MDBX_SUCCESS) {
            LOG_ERROR(2224, index_id_, "Failed to begin loadTermInfo transaction: " << mdbx_strerror(rc));
            return false;
        }

        MDBX_cursor* cursor = nullptr;
        rc = mdbx_cursor_open(txn, blocked_term_postings_dbi_, &cursor);
        if (rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return false;
        }

        MDBX_val key, data;
        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST);
        while (rc == MDBX_SUCCESS) {
            if (key.iov_len == sizeof(uint64_t)) {
                uint64_t packed_key;
                std::memcpy(&packed_key, key.iov_base, sizeof(uint64_t));

                uint32_t term_id = unpackTermId(packed_key);
                uint32_t block_nr = unpackBlockNr(packed_key);

                if (term_id != kMetadataTermId
                    && block_nr == kMetadataBlockNr
                    && data.iov_len >= sizeof(PostingListHeader)) {
                    PostingListHeader header;
                    std::memcpy(&header, data.iov_base, sizeof(PostingListHeader));

                    if (header.nr_live_entries > 0 && header.max_value > settings::NEAR_ZERO) {
                        term_info_[term_id] = header.max_value;
                    }
                }
            }

            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
        }

        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        LOG_INFO(2225, index_id_, "loadTermInfo loaded " << term_info_.size() << " active terms");
        return true;
    }

    // =========================================================================
    // Add / remove internals
    // =========================================================================

    bool InvertedIndex::addDocumentsBatchInternal(
        MDBX_txn* txn,
        const std::vector<std::pair<ndd::idInt, SparseVector>>& docs)
    {
        if (docs.empty()) return true;

        // Reorganize the batch by term so each term can be merged into its posting list
        // independently. The on-disk structure is term-major.
#ifdef ND_SPARSE_INSTRUMENT
        SparseUpdateDebugStats& update_stats = sparseUpdateDebugStats();
        update_stats.add_batch_calls.fetch_add(1, std::memory_order_relaxed);
        update_stats.add_batch_docs.fetch_add(docs.size(), std::memory_order_relaxed);
        uint64_t raw_update_count = 0;
        const auto build_term_updates_start = SteadyClock::now();
#endif // ND_SPARSE_INSTRUMENT

        std::unordered_map<uint32_t, std::vector<std::pair<ndd::idInt, float>>> term_updates;

        for (const auto& [doc_id, sparse_vec] : docs) {
#ifdef ND_SPARSE_INSTRUMENT
            raw_update_count += sparse_vec.indices.size();
#endif // ND_SPARSE_INSTRUMENT
            for (size_t i = 0; i < sparse_vec.indices.size(); i++) {
                uint32_t term_id = sparse_vec.indices[i];
                if (term_id == kMetadataTermId) {
                    LOG_ERROR(2226, index_id_, "term_id UINT32_MAX is reserved for sparse metadata");
                    return false;
                }
                term_updates[term_id].push_back(std::make_pair(doc_id, sparse_vec.values[i]));
            }
        }

#ifdef ND_SPARSE_INSTRUMENT
        update_stats.add_batch_raw_updates.fetch_add(raw_update_count, std::memory_order_relaxed);
        update_stats.add_batch_terms.fetch_add(term_updates.size(), std::memory_order_relaxed);
        update_stats.build_term_updates_total_ns.fetch_add(
                elapsedNsSince(build_term_updates_start), std::memory_order_relaxed);
#endif // ND_SPARSE_INSTRUMENT

        for (auto& [term_id, updates] : term_updates) {
#ifdef ND_SPARSE_INSTRUMENT
            const auto sort_dedup_start = SteadyClock::now();
#endif // ND_SPARSE_INSTRUMENT

            // Merge logic below assumes doc_ids are sorted and unique per term within this batch.
            std::sort(updates.begin(), updates.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });

            // Keep only the last update per doc_id if duplicates are found.
            std::vector<std::pair<ndd::idInt, float>> deduped;
            deduped.reserve(updates.size());
            for (const auto& u : updates) {
                if (!deduped.empty() && deduped.back().first == u.first) {
                    deduped.back().second = u.second;
                } else {
                    deduped.push_back(u);
                }
            }

#ifdef ND_SPARSE_INSTRUMENT
            update_stats.sort_dedup_total_ns.fetch_add(
                    elapsedNsSince(sort_dedup_start), std::memory_order_relaxed);
            update_stats.add_batch_deduped_updates.fetch_add(
                    deduped.size(), std::memory_order_relaxed);
#endif // ND_SPARSE_INSTRUMENT

            bool header_found = false;
            PostingListHeader header = readPostingListHeader(txn, term_id, &header_found);
            float old_global_max = header.max_value;
            bool need_recompute_max = false;

            size_t ui = 0;
            while (ui < deduped.size()) {
                uint32_t block_nr = docToBlockNr(deduped[ui].first);
                size_t block_begin = ui;
                while (ui < deduped.size() && docToBlockNr(deduped[ui].first) == block_nr) {
                    ui++;
                }

#ifdef ND_SPARSE_INSTRUMENT
                update_stats.add_batch_blocks.fetch_add(1, std::memory_order_relaxed);
#endif // ND_SPARSE_INSTRUMENT

                // One MDBX record stores exactly one (term, block_nr) slice, so split the
                // term's updates into block-local chunks before merging.
                std::vector<std::pair<ndd::idInt, float>> block_updates(
                    deduped.begin() + block_begin, deduped.begin() + ui);

                std::vector<PostingListEntry> existing;
                uint32_t old_live_in_block = 0;
                float old_block_max = 0.0f;
                bool block_found = false;

#ifdef ND_SPARSE_INSTRUMENT
                const auto load_block_start = SteadyClock::now();
#endif // ND_SPARSE_INSTRUMENT
                bool load_ok = loadBlockEntries(txn,
                                                term_id,
                                                block_nr,
                                                &existing,
                                                &old_live_in_block,
                                                &old_block_max,
                                                &block_found);
#ifdef ND_SPARSE_INSTRUMENT
                update_stats.load_block_calls.fetch_add(1, std::memory_order_relaxed);
                update_stats.load_block_total_ns.fetch_add(
                        elapsedNsSince(load_block_start), std::memory_order_relaxed);
                update_stats.load_block_entries_total.fetch_add(
                        existing.size(), std::memory_order_relaxed);
#endif // ND_SPARSE_INSTRUMENT
                if (!load_ok) {
                    return false;
                }

#ifdef ND_SPARSE_INSTRUMENT
                const auto merge_start = SteadyClock::now();
#endif // ND_SPARSE_INSTRUMENT
                // Classic merge of two sorted streams: existing postings in the block and the
                // incoming updates for that same block.
                std::vector<PostingListEntry> merged;
                merged.reserve(existing.size() + block_updates.size());

                size_t ei = 0;
                size_t bi = 0;
                while (ei < existing.size() && bi < block_updates.size()) {
                    ndd::idInt existing_id = existing[ei].doc_id;
                    ndd::idInt update_id = block_updates[bi].first;

                    if (existing_id < update_id) {
                        merged.push_back(existing[ei]);
                        ei++;
                    } else if (existing_id > update_id) {
                        merged.push_back(PostingListEntry(update_id, block_updates[bi].second));
                        bi++;
                    } else {
                        merged.push_back(PostingListEntry(update_id, block_updates[bi].second));
                        ei++;
                        bi++;
                    }
                }
                while (ei < existing.size()) {
                    merged.push_back(existing[ei]);
                    ei++;
                }
                while (bi < block_updates.size()) {
                    merged.push_back(PostingListEntry(block_updates[bi].first,
                                                        block_updates[bi].second));
                    bi++;
                }

                uint32_t new_live_in_block = 0;
                float new_block_max = 0.0f;
                for (const auto& e : merged) {
                    if (e.value > 0.0f) {
                        new_live_in_block++;
                        if (e.value > new_block_max) new_block_max = e.value;
                    } else if (e.value == 0.0f) {
                        LOG_WARN(2227,
                                 index_id_,
                                 "addDocumentsBatch received zero value for term "
                                         << term_id << "; entry will be treated as deleted");
                    } else {
                        LOG_WARN(2228,
                                 index_id_,
                                 "addDocumentsBatch received negative value " << e.value
                                                                              << " for term " << term_id
                                                                              << "; treating as dead");
                    }
                }

#ifdef ND_SPARSE_INSTRUMENT
                update_stats.merge_block_calls.fetch_add(1, std::memory_order_relaxed);
                update_stats.merge_block_total_ns.fetch_add(
                        elapsedNsSince(merge_start), std::memory_order_relaxed);
                update_stats.merge_existing_entries_total.fetch_add(
                        existing.size(), std::memory_order_relaxed);
                update_stats.merge_update_entries_total.fetch_add(
                        block_updates.size(), std::memory_order_relaxed);
                update_stats.merge_output_entries_total.fetch_add(
                        merged.size(), std::memory_order_relaxed);
#endif // ND_SPARSE_INSTRUMENT

                uint32_t old_total = static_cast<uint32_t>(existing.size());
                uint32_t new_total = static_cast<uint32_t>(merged.size());
                applyHeaderDelta(header,
                                static_cast<int64_t>(new_total) - static_cast<int64_t>(old_total),
                                static_cast<int64_t>(new_live_in_block)
                                    - static_cast<int64_t>(old_live_in_block));

                if (merged.empty()) {
                    if (!deleteBlock(txn, term_id, block_nr)) return false;
                } else {
#ifdef ND_SPARSE_INSTRUMENT
                    const auto save_block_start = SteadyClock::now();
#endif // ND_SPARSE_INSTRUMENT
                    bool save_ok = saveBlockEntries(txn,
                                                    term_id,
                                                    block_nr,
                                                    merged,
                                                    new_live_in_block,
                                                    new_block_max);
#ifdef ND_SPARSE_INSTRUMENT
                    update_stats.save_block_calls.fetch_add(1, std::memory_order_relaxed);
                    update_stats.save_block_total_ns.fetch_add(
                            elapsedNsSince(save_block_start), std::memory_order_relaxed);
                    update_stats.save_block_entries_total.fetch_add(
                            merged.size(), std::memory_order_relaxed);
#endif // ND_SPARSE_INSTRUMENT
                    if (!save_ok) {
                        return false;
                    }
                }

                if (new_block_max > header.max_value) {
                    header.max_value = new_block_max;
                }

                /**
                 * if the old_global_max was from this block's max and
                 * if this block's max has changed and
                 * if the new block max is less than old_global_max
                 * then we need to recompute the global max from all blocks.
                 *
                 * recompute global max once all the blocks have been updated
                 * from this document batch.
                 */
                if (old_block_max > 0.0f && nearEqual(old_block_max, old_global_max)
                    && new_block_max + settings::NEAR_ZERO < old_global_max) {
                    need_recompute_max = true;
                }
            }

                if (header.nr_entries == 0) {
                    if (!deletePostingListHeader(txn, term_id)) return false;
                    term_info_.erase(term_id);
                    continue;
                }

            // Recompute the term max only when the previous max might have been invalidated.
            if (need_recompute_max) {
#ifdef ND_SPARSE_INSTRUMENT
                const auto recompute_max_start = SteadyClock::now();
#endif // ND_SPARSE_INSTRUMENT
                header.max_value = recomputeGlobalMaxFromBlocks(txn, term_id);
#ifdef ND_SPARSE_INSTRUMENT
                update_stats.recompute_max_calls.fetch_add(1, std::memory_order_relaxed);
                update_stats.recompute_max_total_ns.fetch_add(
                        elapsedNsSince(recompute_max_start), std::memory_order_relaxed);
#endif // ND_SPARSE_INSTRUMENT
            } //while (ui < deduped.size())

            if (header.nr_live_entries == 0) {
                header.max_value = 0.0f;
            }

            if (!writePostingListHeader(txn, term_id, header)) {
                return false;
            }

            if (header.nr_live_entries > 0 && header.max_value > settings::NEAR_ZERO) {
                term_info_[term_id] = header.max_value;
            } else {
                term_info_.erase(term_id);
            }
        }

        return true;
    }


    bool InvertedIndex::removeDocumentInternal(MDBX_txn* txn,
                                            ndd::idInt doc_id,
                                            const SparseVector& vec)
    {
        /**
         * NOTE: This can be slow right now since we provide a single vector to delete
         * at once. It should ideally be faster with a batch.
         */
        for (size_t i = 0; i < vec.indices.size(); i++) {
            uint32_t term_id = vec.indices[i];
            if (term_id == kMetadataTermId) continue;

            bool header_found = false;
            PostingListHeader header = readPostingListHeader(txn, term_id, &header_found);
            if (!header_found || header.nr_entries == 0) continue;

            uint32_t block_nr = docToBlockNr(doc_id);

            std::vector<PostingListEntry> entries;
            uint32_t old_live_in_block = 0;
            float old_block_max = 0.0f;
            bool block_found = false;

            if (!loadBlockEntries(txn,
                                term_id,
                                block_nr,
                                &entries,
                                &old_live_in_block,
                                &old_block_max,
                                &block_found)) {
                return false;
            }
            if (!block_found || entries.empty()) continue;

            size_t lo = 0;
            size_t hi = entries.size();
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (entries[mid].doc_id < doc_id) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }

            if (lo >= entries.size() || entries[lo].doc_id != doc_id) {
                continue;
            }

            if (entries[lo].value <= 0.0f) {
                continue;
            }

            // Deletes are represented as zero-valued tombstones until the tombstone ratio
            // is high enough to justify compacting the block in place.
            entries[lo].value = 0.0f;

            uint32_t new_live_in_block = old_live_in_block > 0 ? old_live_in_block - 1 : 0;
            uint32_t old_total = static_cast<uint32_t>(entries.size());

            float tombstone_ratio = old_total > 0
                ? (float)(old_total - new_live_in_block) / (float)old_total
                : 0.0f;

            if (tombstone_ratio >= settings::INV_IDX_COMPACTION_TOMBSTONE_RATIO) {
                //Compact deleted entries
                size_t write = 0;
                for (size_t j = 0; j < entries.size(); j++) {
                    if (entries[j].value > 0.0f) {
                        entries[write++] = entries[j];
                    }
                }
                entries.resize(write);
            }

            new_live_in_block = 0;
            float new_block_max = 0.0f;
            for (const auto& e : entries) {
                if (e.value > 0.0f) {
                    new_live_in_block++;
                    if (e.value > new_block_max) new_block_max = e.value;
                }
            }

            uint32_t new_total = static_cast<uint32_t>(entries.size());
            applyHeaderDelta(header,
                            static_cast<int64_t>(new_total) - static_cast<int64_t>(old_total),
                            static_cast<int64_t>(new_live_in_block)
                                - static_cast<int64_t>(old_live_in_block));

            bool need_recompute_max = false;
            if (old_block_max > 0.0f && nearEqual(old_block_max, header.max_value)
                && new_block_max + settings::NEAR_ZERO < header.max_value) {
                need_recompute_max = true;
            }

            if (entries.empty()) {
                if (!deleteBlock(txn, term_id, block_nr)) return false;
            } else {
                if (!saveBlockEntries(txn,
                                    term_id,
                                    block_nr,
                                    entries,
                                    new_live_in_block,
                                    new_block_max)) {
                    return false;
                }
            }

            if (header.nr_entries == 0) {
                if (!deletePostingListHeader(txn, term_id)) return false;
                term_info_.erase(term_id);
                continue;
            }

            if (need_recompute_max) {
                header.max_value = recomputeGlobalMaxFromBlocks(txn, term_id);
            }

            if (header.nr_live_entries == 0) {
                header.max_value = 0.0f;
            }

            if (!writePostingListHeader(txn, term_id, header)) {
                return false;
            }

            if (header.nr_live_entries > 0 && header.max_value > settings::NEAR_ZERO) {
                term_info_[term_id] = header.max_value;
            } else {
                term_info_.erase(term_id);
            }
        }

        return true;
    }

    // =========================================================================
    // Pruning
    // =========================================================================

    void InvertedIndex::pruneLongest(std::vector<PostingListIterator*>& iters,
                                float min_score)
    {
        if (iters.size() < 2) return;

        // Pruning only ever advances the single longest remaining list. That keeps the rule
        // simple: if even its maximum possible future contribution cannot beat the current
        // threshold, skip ahead to where the other lists resume.
        size_t longest_idx = 0;
        uint32_t longest_rem = 0;
        for (size_t i = 0; i < iters.size(); i++) {
            uint32_t rem = iters[i]->remainingEntries();
            if (rem > longest_rem) {
                longest_rem = rem;
                longest_idx = i;
            }
        }

        if (longest_idx != 0) {
            PostingListIterator* tmp = iters[0];
            iters[0] = iters[longest_idx];
            iters[longest_idx] = tmp;
        }

        PostingListIterator* longest = iters[0];
        if (longest->current_doc_id == EXHAUSTED_DOC_ID) return;

        ndd::idInt longest_doc = longest->current_doc_id;

        ndd::idInt others_min_doc_id = EXHAUSTED_DOC_ID;
        for (size_t i = 1; i < iters.size(); i++) {
            if (iters[i]->current_doc_id < others_min_doc_id) {
                others_min_doc_id = iters[i]->current_doc_id;
            }
        }

        if (others_min_doc_id <= longest_doc) return;

        float max_possible = longest->upperBound();

        if (max_possible <= min_score) {
#ifdef NDD_INV_IDX_PRUNE_DEBUG
            uint32_t remaining_before_prune = longest->remaining_entries;
#endif // NDD_INV_IDX_PRUNE_DEBUG
            if (others_min_doc_id == EXHAUSTED_DOC_ID) {
                longest->current_doc_id = EXHAUSTED_DOC_ID;
                longest->remaining_entries = 0;
            } else {
                longest->advance(others_min_doc_id);
            }
#ifdef NDD_INV_IDX_PRUNE_DEBUG
            if (remaining_before_prune > longest->remaining_entries) {
                longest->pruned_entries +=
                    (remaining_before_prune - longest->remaining_entries);
            }
#endif // NDD_INV_IDX_PRUNE_DEBUG
        }
    }

    // =========================================================================
    // PostingListIterator methods
    // =========================================================================

    void InvertedIndex::PostingListIterator::init(MDBX_cursor* cursor_in,
                                                uint32_t tid,
                                                float tw,
                                                float gmax,
                                                uint32_t total_entries,
                                                const InvertedIndex* idx) {
        cursor = cursor_in;
        term_id = tid;
        term_weight = tw;
        global_max = gmax;
        index = idx;

        current_block_nr = 0;
        doc_offsets = nullptr;
        values_ptr = nullptr;
        data_size = 0;
        value_bits = 0;
        max_value = 0.0f;

        current_entry_idx = 0;
        current_doc_id = EXHAUSTED_DOC_ID;
        remaining_entries = total_entries;
#ifdef NDD_INV_IDX_PRUNE_DEBUG
        initial_entries = total_entries;
        pruned_entries = 0;
#endif // NDD_INV_IDX_PRUNE_DEBUG

        // Position the iterator on the first non-empty block and then on the first live entry
        // inside that block.
        if (!loadFirstBlock()) {
            current_doc_id = EXHAUSTED_DOC_ID;
            remaining_entries = 0;
            return;
        }

        current_entry_idx = 0;
        advanceToNextLive();
    }

    bool InvertedIndex::PostingListIterator::parseCurrentKV(const MDBX_val& key,
                                                            const MDBX_val& data) {
#ifdef ND_SPARSE_INSTRUMENT
        ParseCurrentKVTimer parse_timer;
#endif // ND_SPARSE_INSTRUMENT
        if (key.iov_len != sizeof(uint64_t)) {
            return false;
        }

        uint64_t packed_key;
        std::memcpy(&packed_key, key.iov_base, sizeof(uint64_t));
        uint32_t key_term = unpackTermId(packed_key);
        uint32_t block_nr = unpackBlockNr(packed_key);

        if (key_term != term_id || block_nr == kMetadataBlockNr) {
            return false;
        }

        BlockView view;
        if (!index->parseBlockViewFromValue(data, block_nr, &view)) {
            return false;
        }

        // Keep raw pointers into the MDBX value so search can read offsets/weights without
        // allocating or copying the block payload.
        current_block_nr = block_nr;
        doc_offsets = view.doc_offsets;
        values_ptr = view.values;
        data_size = view.count;
        value_bits = view.value_bits;
        max_value = view.max_value;
        current_entry_idx = 0;

        return true;
    }

    bool InvertedIndex::PostingListIterator::loadFirstBlock() {
        uint64_t seek_packed = packPostingKey(term_id, 0);
        MDBX_val key{&seek_packed, sizeof(seek_packed)};
        MDBX_val data;

        // Seek once into the contiguous key range for this term, then skip any empty blocks.
        int rc = mdbx_cursor_get(cursor, &key, &data, MDBX_SET_RANGE);
        while (rc == MDBX_SUCCESS) {
            if (key.iov_len != sizeof(uint64_t)) return false;

            uint64_t packed_key;
            std::memcpy(&packed_key, key.iov_base, sizeof(uint64_t));
            uint32_t key_term = unpackTermId(packed_key);
            uint32_t block_nr = unpackBlockNr(packed_key);

            if (key_term != term_id || block_nr == kMetadataBlockNr) {
                return false;
            }

            if (!parseCurrentKV(key, data)) {
                return false;
            }

            if (data_size == 0) {
                rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
                continue;
            }

            return true;
        }

        return false;
    }

    bool InvertedIndex::PostingListIterator::loadNextBlock() {
        // LOG_TIME("loadNextBlock"); this function is not slow
        MDBX_val key;
        MDBX_val data;
        int rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);

        // Stop as soon as the cursor leaves this term's key range. The next term or metadata row
        // belongs to a different posting list.
        while (rc == MDBX_SUCCESS) {
            if (key.iov_len != sizeof(uint64_t)) {
                current_doc_id = EXHAUSTED_DOC_ID;
                data_size = 0;
                return false;
            }

            uint64_t packed_key;
            std::memcpy(&packed_key, key.iov_base, sizeof(uint64_t));
            uint32_t key_term = unpackTermId(packed_key);
            uint32_t block_nr = unpackBlockNr(packed_key);

            if (key_term != term_id || block_nr == kMetadataBlockNr) {
                current_doc_id = EXHAUSTED_DOC_ID;
                data_size = 0;
                return false;
            }

            if (!parseCurrentKV(key, data)) {
                current_doc_id = EXHAUSTED_DOC_ID;
                data_size = 0;
                return false;
            }

            if (data_size == 0) {
                rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
                continue;
            }

            return true;
        }

        current_doc_id = EXHAUSTED_DOC_ID;
        data_size = 0;
        return false;
    }

    void InvertedIndex::PostingListIterator::advanceToNextLive() {
        // LOG_TIME("advanceToNextLive"); //this function is also not slow
        while (true) {
            if (value_bits == 32) {
                const float* vals = (const float*)values_ptr;
                while (current_entry_idx < data_size && vals[current_entry_idx] <= 0.0f) {
                    consumeEntries(1);
                    current_entry_idx++;
                }
            } else {
                uint32_t next_live = static_cast<uint32_t>(index->findNextLiveSIMD(
                    (const uint8_t*)values_ptr,
                    data_size,
                    current_entry_idx));
                consumeEntries(next_live - current_entry_idx);
                current_entry_idx = next_live;
            }

            if (current_entry_idx < data_size) {
                // Found the next non-zero value in the current block.
                current_doc_id = docIdAt(current_entry_idx);
                return;
            }

            // Current block is exhausted; keep scanning forward until we find another non-empty block
            // or run out of rows for this term.
            if (!loadNextBlock()) {
                current_doc_id = EXHAUSTED_DOC_ID;
                return;
            }

            current_entry_idx = 0;
        }
    }

    void InvertedIndex::PostingListIterator::next() {
        if (current_doc_id == EXHAUSTED_DOC_ID) return;
        consumeEntries(1);
        current_entry_idx++;
        advanceToNextLive();
    }

    void InvertedIndex::PostingListIterator::advance(ndd::idInt target_doc_id) {
        if (current_doc_id == EXHAUSTED_DOC_ID || current_doc_id >= target_doc_id) {
            return;
        }

        while (true) {
            if (current_doc_id == EXHAUSTED_DOC_ID) return;

            const uint32_t target_block_nr = docToBlockNr(target_doc_id);
            if (current_block_nr < target_block_nr) {
                // Target is in a later block, so skip the remainder of the current block at once.
                consumeEntries(data_size - current_entry_idx);
                if (!loadNextBlock()) {
                    current_doc_id = EXHAUSTED_DOC_ID;
                    break;
                }
                current_entry_idx = 0;
                continue;
            }

            if (current_block_nr > target_block_nr) {
                current_entry_idx = 0;
                advanceToNextLive();
                break;
            }

            const BlockOffset target_offset = docToBlockOffset(target_doc_id);
            // Within the block, offsets are sorted, so a lower_bound finds the first candidate
            // doc_id without decoding the entire block into absolute ids.
            const BlockOffset* begin = doc_offsets + current_entry_idx;
            const BlockOffset* end = doc_offsets + data_size;
            const BlockOffset* next =
                    std::lower_bound(begin, end, target_offset);
            uint32_t next_idx = static_cast<uint32_t>(next - doc_offsets);

            consumeEntries(next_idx - current_entry_idx);
            current_entry_idx = next_idx;
            advanceToNextLive();
            break;
        }
    }

}  // namespace ndd
