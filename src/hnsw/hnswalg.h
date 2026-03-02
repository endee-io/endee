#pragma once

#include "visited_list_pool.h"
#include "hnswlib.h"
#include "vector_cache.h"
#include "log.hpp"
#include "../utils/settings.hpp"
#include "../quant/dispatch.hpp"
#include <atomic>
#include <random>
#include <stdlib.h>
#include <assert.h>
#include <unordered_set>
#include <list>
#include <functional>
#include <sstream>
#include <iostream>
#include <thread>
#include <set>
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <type_traits>

namespace hnswlib {

    template <typename T> struct CompareByFirst {
        constexpr bool operator()(const T& p1, const T& p2) const noexcept {
            return p1.first < p2.first;
        }
    };
    template <typename T> struct CompareBySecond {
        constexpr bool operator()(const T& p1, const T& p2) const noexcept {
            return p2.first < p1.first;
        }
    };

    template <typename dist_t> class HierarchicalNSW : public AlgorithmInterface<dist_t> {
        using distance_type = std::pair<dist_t, idhInt>;
        using max_heap_pq = std::priority_queue<distance_type,
                                                std::vector<distance_type>,
                                                CompareByFirst<distance_type>>;
        using min_heap_pq = std::priority_queue<distance_type,
                                                std::vector<distance_type>,
                                                CompareBySecond<distance_type>>;
        using VectorFetcher = std::function<bool(idInt, uint8_t*)>;
        // Batch fetcher: fetches multiple vectors in one MDBX txn
        // Args: labels array, output buffers (flat, count*vector_size), success flags, count
        // Returns: number of successful fetches
        using VectorFetcherBatch = std::function<size_t(const idInt*, uint8_t*, bool*, size_t)>;
        using SimBatchFunc = void (*)(const void* query,
                          const void* const* vectors,
                          size_t count,
                          const void* params,
                          float* out);

    public:
        // Constructors and destructor
        HierarchicalNSW(SpaceInterface<dist_t>* s) {}
        // Used for loading an existing index
        HierarchicalNSW(const std::string& location, size_t max_elements = 0) :
            linkListLocks_(settings::MAX_LINK_LIST_LOCKS) {
            // Initilaize label lookup vector. Values will be filled in loadIndex
            loadIndex(location, max_elements);
        }
        // Used for creating a new index
        HierarchicalNSW(
                size_t max_elements,
                SpaceType space_type,
                size_t dimension,
                size_t M = settings::DEFAULT_M,
                size_t ef_construction = settings::DEFAULT_EF_CONSTRUCT,
                size_t random_seed = settings::RANDOM_SEED,
                ndd::quant::QuantizationLevel quant_level = ndd::quant::QuantizationLevel::INT8,
                int32_t checksum = -1) :
            maxElements_(max_elements),
            dimension_(dimension),
            space_type_(space_type),
            quant_level_(quant_level),
            M_(M),
            M0_(M * 2),
            efConstruction_(std::max(ef_construction, M_)),
            linkListLocks_(settings::MAX_LINK_LIST_LOCKS),
            checksum_(checksum),
            dataUpperLayer_(maxElements_),
            labelLookup_(maxElements_, INVALID_ID) {

            // Create appropriate space based on type
            space_ = std::unique_ptr<SpaceInterface<float>>(
                    createSpace<float>(space_type_, dimension_, quant_level_));
            data_size_ = space_->get_data_size();
            fstDistFunc_ = space_->get_dist_func();
            fstSimFunc_ = space_->get_sim_func();
            dist_func_param_ = space_->get_dist_func_param();
            LOG_DEBUG("Space initialized with data size: "
                      << data_size_ << ", dimension: " << dimension_
                      << ", quant_level: " << static_cast<int>(quant_level_));

            // Initialize cache
            size_t cache_bits = VectorCache::calculateCacheBits(maxElements_);
            if (cache_bits > 0) {
                vector_cache_ = std::make_unique<VectorCache>(data_size_, cache_bits);
                LOG_DEBUG("Vector cache initialized for " << maxElements_ << " elements with " << (1ULL << cache_bits) << " slots");
                size_t cache_bytes = vector_cache_->getMemoryUsage();
                LOG_DEBUG("Vector cache allocated: " << cache_bytes << " bytes (" << (cache_bytes / MB) << " MB)");
            }

            LOG_DEBUG("Unified layer data size: " << data_size_);

            // M_ cannot be more than settings::MAX_M
            if(M_ > settings::MAX_M) {
                M_ = settings::MAX_M;
                M0_ = M_ * 2;
                LOG_DEBUG("Capping M parameter to settings::MAX_M" << settings::MAX_M);
            }
            //efConstruction cannot be more than MAX_EF_CONSTRUCT
            if(efConstruction_ > settings::MAX_EF_CONSTRUCT) {
                efConstruction_ = settings::MAX_EF_CONSTRUCT;
                LOG_DEBUG("Capping efConstruction to settings::MAX_EF_CONSTRUCT "
                          << settings::MAX_EF_CONSTRUCT);
            }

            level_generator_.seed(random_seed);
            update_probability_generator_.seed(random_seed + 1);

            // links will also store number of linked elements in the first element
            sizeLinksUpperLayers_ = sizeof(idhInt) + M_ * sizeof(idhInt);
            sizeLinksBaseLayer_ = sizeof(idhInt) + M0_ * sizeof(idhInt);
            // We are not storing the data in the level 0 memory, only the links and label
            sizeDataAtBaseLayer_ = sizeLinksBaseLayer_ + sizeof(flagInt) + sizeof(idInt);
            labelOffset_ = sizeLinksBaseLayer_ + sizeof(flagInt);

            dataBaseLayer_ = (char*)malloc(maxElements_ * sizeDataAtBaseLayer_);
            if(!dataBaseLayer_) {
                throw std::runtime_error(
                        "Unable to allocate "
                        + std::to_string((maxElements_ * sizeDataAtBaseLayer_) / KB) + " KB");
            }

            mult_ = 1 / log(1.0 * M_);

            visited_list_pool_ =
                    std::unique_ptr<VisitedListPool>(new VisitedListPool(1, maxElements_));
        }

        ~HierarchicalNSW() {
            LOG_DEBUG("HierarchicalNSW destructor called");
            if(dataBaseLayer_) {
                free(dataBaseLayer_);
            }
        }
        // Public getters and setters
        ndd::quant::QuantizationLevel getQuantLevel() const { return quant_level_; }
        int32_t getChecksum() const { return checksum_; }

        SpaceType getSpaceType() const { return space_type_; }
        std::string getSpaceTypeStr() const { return getSpaceTypeString(space_type_); }
        SpaceInterface<dist_t>* getSpace() const { return space_.get(); }
        size_t getDataSize() const { return data_size_; }
        void setVectorFetcher(VectorFetcher fetcher) { vector_fetcher_ = fetcher; }
        void setVectorFetcherBatch(VectorFetcherBatch fetcher) { vector_fetcher_batch_ = fetcher; }
        size_t getDimension() const { return dimension_; }
        size_t getM() const { return M_; }
        size_t getEfConstruction() const { return efConstruction_; }
        size_t getRemainingCapacity() const { return maxElements_ - curElementsCount_; }
        size_t getMaxElements() const { return maxElements_; }
        // Get active elements count
        size_t getElementsCount() const { return curElementsCount_ - deletedElementsCount_; }
        size_t getDeletedCount() const { return deletedElementsCount_; }
        std::string getElementStats() const {
            std::stringstream ss;
            ss << "Elements: " << curElementsCount_ << ", MaxLevel: " << maxLevel_
               << ", Deleted: " << deletedElementsCount_;
            return ss.str();
        }
        size_t getApproxSizeGB() const {
            size_t size = 0;

            // Level 0: links + flags + labels
            size += maxElements_ * sizeDataAtBaseLayer_;

            // Approximate level > 0 count
            size_t upper_layer_estimate = maxElements_ / M_;

                // Upper layer calculation using runtime data size
                size += upper_layer_estimate
                    * (data_size_ + sizeof(levelInt) + sizeLinksUpperLayers_);

            if (vector_cache_) {
                size += vector_cache_->getMemoryUsage();
            }

            return size / GB;  // GB
        }

        // Cache management getters/setters
        // Removed as cache is managed externally

        template <typename FilterFunctor>
        std::vector<std::pair<dist_t, idInt>>
        searchKnn(const void* query_data,
                  size_t k,
                  size_t ef,
                  FilterFunctor* isIdAllowed,
                  size_t filter_boost_percentage = settings::FILTER_BOOST_PERCENTAGE) const { // Default true as requested
            LOG_DEBUG("Inside searchKnn, element count: " << curElementsCount_);
            std::vector<std::pair<dist_t, idInt>> result;
            if(curElementsCount_ == 0) {
                return result;
            }
            LOG_DEBUG("Searching for k=" << k << " nearest neighbors");
            idhInt currObj = entryPoint_;
            dist_t curSim;

            if(maxLevel_ > 0) {
                // Use direct pointer for upper layers
                const uint8_t* ep_data = getUpperLayerDataPtr(currObj);
                if(!ep_data) {
                    return result;
                }

                curSim = fstSimFunc_(query_data, ep_data, dist_func_param_);
            }

            dist_t s;
            // Upper layer traversal - greedy search (levels maxLevel_ down to 1)
            for(levelInt level = maxLevel_; level > 0; level--) {
                bool changed = true;
                while(changed) {
                    changed = false;
                    // TODO - This is dead-locking the mutex
                    //std::unique_lock<std::shared_mutex> lock(linkListLocks_[currObj]);
                    idhInt* ll_cur = (idhInt*)get_linklist(currObj, level);
                    if(!ll_cur) {
                        continue;
                    }

                    int size = getListCount(ll_cur);
                    idhInt* data = (idhInt*)(ll_cur + 1);

                    std::vector<idhInt> valid_candidates;
                    valid_candidates.reserve(size);
                    std::vector<const void*> candidate_ptrs;
                    candidate_ptrs.reserve(size);

                    for(int i = 0; i < size; i++) {
                        idhInt candidate = data[i];
                        if(candidate >= curElementsCount_) {
                            continue;
                        }

                        const uint8_t* candidate_data = getUpperLayerDataPtr(candidate);
                        if(!candidate_data) {
                            continue;
                        }

                        valid_candidates.push_back(candidate);
                        candidate_ptrs.push_back(candidate_data);
                    }

                    if(valid_candidates.empty()) {
                        continue;
                    }

                    std::vector<dist_t> sims;
                    computeBatchSimilaritiesFromPtrs(query_data,
                                                     candidate_ptrs,
                                                     fstSimFunc_,
                                                     dist_func_param_,
                                                     sims);

                    for(size_t i = 0; i < valid_candidates.size(); ++i) {
                        s = sims[i];
                        if(s > curSim) {
                            curSim = s;
                            currObj = valid_candidates[i];
                            changed = true;
                        }
                    }
                }
            }

            std::vector<idhInt> entry_points;
            if (maxLevel_ > 0) {
                entry_points.push_back(currObj);
            } else {
                entry_points.push_back(entryPoint_);
            }

            std::vector<std::pair<dist_t, idhInt>> top_candidates;
            LOG_DEBUG("Starting search in level 0..");
            if(deletedElementsCount_) {
                top_candidates = searchBaseLayer<false, true, FilterFunctor>(
                        entry_points, query_data, 0, std::max(ef, k), isIdAllowed, filter_boost_percentage);  // Level 0 for final search
            } else {
                top_candidates = searchBaseLayer<false, false, FilterFunctor>(
                        entry_points, query_data, 0, std::max(ef, k), isIdAllowed, filter_boost_percentage);  // Level 0 for final search
            }
            LOG_DEBUG("Search in level 0 completed. Found " << top_candidates.size()
                                                            << " candidates");

            // Get external labels and return k elements
            for(size_t i = 0; i < std::min(k, top_candidates.size()); ++i) {
                result.emplace_back(top_candidates[i].first,
                                    getExternalLabel(top_candidates[i].second));
            }
            return result;
        }

        std::vector<std::pair<dist_t, idInt>>
        searchKnn(const void* query_data,
                  size_t k,
                  size_t ef,
                  BaseFilterFunctor* isIdAllowed = nullptr,
                  size_t filter_boost_percentage = settings::FILTER_BOOST_PERCENTAGE) const override {
            if (isIdAllowed) {
                 return searchKnn<BaseFilterFunctor>(query_data, k, ef, isIdAllowed, filter_boost_percentage);
            } else {
                 return searchKnn<void>(query_data, k, ef, nullptr, filter_boost_percentage);
            }
        }

        void saveIndex(const std::string& location) override {
            // Lock the index so that addPoint and markDelete are not called
            std::unique_lock<std::shared_mutex> lock(index_lock_);

            std::ofstream output(location, std::ios::binary);

            // Save version (first 2 bytes)
            writeBinaryPOD(output, settings::INDEX_VERSION);

            // 8 byte space for future flags
            writeBinaryPOD(output, flags_);

            // Save index parameters
            writeBinaryPOD(output, checksum_);
            writeBinaryPOD(output, space_type_);
            writeBinaryPOD(output, dimension_);
            writeBinaryPOD(output, quant_level_);
            writeBinaryPOD(output, maxElements_);
            writeBinaryPOD(output, curElementsCount_);
            writeBinaryPOD(output, deletedElementsCount_);
            writeBinaryPOD(output, labelOffset_);
            writeBinaryPOD(output, maxLevel_);
            writeBinaryPOD(output, entryPoint_);
            writeBinaryPOD(output, M_);
            writeBinaryPOD(output, M0_);
            writeBinaryPOD(output, mult_);
            writeBinaryPOD(output, efConstruction_);

            // Save level 0 data
            output.write(dataBaseLayer_, maxElements_ * sizeDataAtBaseLayer_);
            // Marker to check alignment of data
            uint64_t upper_marker = 0xDEADBEEFDEADBEEF;
            writeBinaryPOD(output, upper_marker);

            levelInt level;
            size_t total_size;
            // Write upper layer data using sentinel-based stream
            for(size_t i = 0; i < dataUpperLayer_.size(); ++i) {
                if(!dataUpperLayer_[i]) {
                    continue;
                }

                level = *reinterpret_cast<levelInt*>(dataUpperLayer_[i].get() + data_size_);
                total_size = data_size_ + sizeof(levelInt) + level * sizeLinksUpperLayers_;

                writeBinaryPOD(output, static_cast<idhInt>(i));  // write ID
                output.write(reinterpret_cast<char*>(dataUpperLayer_[i].get()),
                             total_size);  // write blob
            }

            // Sentinel to mark end
            idhInt sentinel = INVALID_ID;
            writeBinaryPOD(output, sentinel);
            output.close();
        }

        void loadIndex(const std::string& location, size_t maxElements_i = 0) {
            std::ifstream input(location, std::ios::binary);
            if(!input.is_open()) {
                throw std::runtime_error("Cannot open file");
            }

            // Read version
            uint16_t version;
            readBinaryPOD(input, version);
            if(version != settings::INDEX_VERSION) {
                LOG_DEBUG("Index version mismatch. Expected: " << settings::INDEX_VERSION
                                                               << ", Found: " << version);
                throw std::runtime_error("Index version mismatch");
            }

            // Read flags
            readBinaryPOD(input, flags_);

            // Load index parameters
            readBinaryPOD(input, checksum_);
            readBinaryPOD(input, space_type_);
            readBinaryPOD(input, dimension_);
            readBinaryPOD(input, quant_level_);
            readBinaryPOD(input, maxElements_);
            LOG_DEBUG("Loading index with maxElements: " << maxElements_);
            readBinaryPOD(input, curElementsCount_);
            LOG_DEBUG("Current elements count: " << curElementsCount_);
            readBinaryPOD(input, deletedElementsCount_);
            readBinaryPOD(input, labelOffset_);
            readBinaryPOD(input, maxLevel_);
            readBinaryPOD(input, entryPoint_);
            readBinaryPOD(input, M_);
            readBinaryPOD(input, M0_);
            readBinaryPOD(input, mult_);
            readBinaryPOD(input, efConstruction_);

            if(maxElements_i > 0) {
                maxElements_ = maxElements_i;
            }
            // links will also store number of linked elements
            sizeLinksUpperLayers_ = sizeof(idInt) + M_ * sizeof(idInt);
            sizeLinksBaseLayer_ = sizeof(idInt) + M0_ * sizeof(idInt);
            // We are not storing the data in the level 0 memory, only the links and labels
            sizeDataAtBaseLayer_ = sizeLinksBaseLayer_ + sizeof(flagInt) + sizeof(idInt);
            labelOffset_ = sizeLinksBaseLayer_ + sizeof(flagInt);

            // Create appropriate space based on stored type
            space_ = std::unique_ptr<SpaceInterface<float>>(
                    createSpace<float>(space_type_, dimension_, quant_level_));
            data_size_ = space_->get_data_size();
            fstDistFunc_ = space_->get_dist_func();
            fstSimFunc_ = space_->get_sim_func();
            dist_func_param_ = space_->get_dist_func_param();

            // Initialize cache for loaded index
            size_t cache_bits = VectorCache::calculateCacheBits(maxElements_);
            LOG_INFO("Calculated cache bits for loaded index: " << cache_bits);
            if (cache_bits > 0) {
                 vector_cache_ = std::make_unique<VectorCache>(data_size_, cache_bits);
                 LOG_DEBUG("Vector cache initialized for " << maxElements_ << " elements with " << (1ULL << cache_bits) << " slots");
            }

            // Allocate memory and load level 0 data
            dataBaseLayer_ = (char*)malloc(maxElements_ * sizeDataAtBaseLayer_);
            if(dataBaseLayer_ == nullptr) {
                throw std::runtime_error("Not enough memory");
            }
            input.read(dataBaseLayer_, maxElements_ * sizeDataAtBaseLayer_);

            uint64_t upper_marker_check;
            readBinaryPOD(input, upper_marker_check);
            if(upper_marker_check != 0xDEADBEEFDEADBEEF) {
                LOG_DEBUG("Corrupt index file: dataUpperLayer_ marker missing or mismatched");
                throw std::runtime_error(
                        "Corrupt index file: dataUpperLayer_ marker missing or mismatched");
            }
            labelLookup_.resize(maxElements_, INVALID_ID);
            for(size_t i = 0; i < curElementsCount_; i++) {
                idInt label = getExternalLabel(i);
                if(label >= maxElements_) {
                    // Oops.. The index is corrupted
                    LOG_DEBUG("Corrupt index: label "
                              << label << " at i=" << i
                              << " exceeds maxElements_ = " << maxElements_);
                    throw std::runtime_error("Corrupt index: label " + std::to_string(label)
                                             + " at i=" + std::to_string(i)
                                             + " exceeds maxElements_ = "
                                             + std::to_string(maxElements_));
                }
                labelLookup_[label] = i;
                //labelLookup_[getExternalLabel(i)] = i;
            }

            dataUpperLayer_.resize(maxElements_);
            while(true) {
                idhInt id;
                readBinaryPOD(input, id);
                if(id == INVALID_ID) {
                    break;
                }

                size_t header_size;
                // Step 1: Read vector + level header
                header_size = data_size_ + sizeof(levelInt);

                std::vector<uint8_t> header_buf(header_size);
                input.read(reinterpret_cast<char*>(header_buf.data()), header_size);
                if(!input) {
                    throw std::runtime_error("Failed to read upper layer header");
                }

                levelInt level;
                level = *reinterpret_cast<levelInt*>(header_buf.data() + data_size_);

                size_t total_size = header_size + level * sizeLinksUpperLayers_;

                // Step 2: Allocate and copy header
                auto mem = std::make_unique<uint8_t[]>(total_size);
                memcpy(mem.get(), header_buf.data(), header_size);

                // Step 3: Read linklists
                input.read(reinterpret_cast<char*>(mem.get() + header_size),
                           level * sizeLinksUpperLayers_);
                if(!input) {
                    throw std::runtime_error("Failed to read upper layer linklists");
                }

                dataUpperLayer_[id] = std::move(mem);
            }

            input.close();

            visited_list_pool_ =
                    std::unique_ptr<VisitedListPool>(new VisitedListPool(1, maxElements_));
            if(visited_list_pool_ == nullptr) {
                throw std::runtime_error("Not enough memory");
            }

            // Adjust cache based on element count and cache percentage threshold (default
            // VECTOR_CACHE_PERCENTAGE) adjustCacheForElementCount(curElementsCount_);
        }


        template <bool is_new> void addPoint(const void* datapoint, idInt label) {
            LOG_TIME("addPoint");

            std::unique_lock<std::shared_mutex> update_lock(index_lock_, std::defer_lock);

            idhInt cur_c = 0;
            levelInt curLevel = 0;
            idhInt update_seed = INVALID_ID;
            if(is_new) {
                // Adding a new point
                // Using fetch_add (or post-increment) ensures unique IDs even under contention.
                cur_c = curElementsCount_.fetch_add(1);

                if(cur_c >= maxElements_) {
                    // Restore count if we exceeded limit (optional, but good for correctness)
                    curElementsCount_--;
                    throw std::runtime_error("The number of elements exceeds the specified limit");
                }

                labelLookup_[label] = cur_c;
                setExternalLabel(cur_c, label);
                curLevel = getRandomLevel(mult_);
            } else {
                idhInt searchId = labelLookup_[label];
                if(searchId != INVALID_ID) {
                    if(searchId == entryPoint_) {
                        update_lock.lock();
                    }

                    // If the element is deleted, mark is undeleted first before calling update
                    // point
                    if(isMarkedDeleted(searchId)) {
                        unmarkDeletedInternal(searchId);
                    }
                    curLevel = getElementLevel(searchId);

                    // If we're updating the current entry point, pick an alternate traversal seed
                    // before disconnecting this node. Starting from a just-disconnected entry
                    // point can degrade into self-only reinsertion.
                    if(searchId == entryPoint_) {
                        // Scan from higher levels down, and accept the first valid non-deleted neighbor.
                        for(int level = curLevel; level >= 0 && update_seed == INVALID_ID; --level) {
                            idhInt* ll_self = reinterpret_cast<idhInt*>(
                                    level == 0 ? get_linklist0(searchId)
                                               : get_linklist(searchId, level));
                            if(!ll_self) {
                                continue;
                            }
                            idhInt sz = getListCount(ll_self);
                            idhInt* neighbors = ll_self + 1;
                            for(idhInt i = 0; i < sz; ++i) {
                                idhInt candidate = neighbors[i];
                                if(candidate == searchId || candidate >= curElementsCount_) {
                                    continue;
                                }
                                if(isMarkedDeleted(candidate)) {
                                    continue;
                                }
                                update_seed = candidate;
                                break;
                            }
                        }

                    }

                    removeAllConnections2(searchId, curLevel);
                    cur_c = searchId;
                } else {
                    LOG_DEBUG("Label not found, can't update the point" << label);
                    return;
                }
            }
            // Update cached value only if this id is already present in cache.
            // Do not insert on add/update path; cold ids can be populated on read hot path.
            if constexpr(!is_new) {
                if(vector_cache_) {
                    // Fast atomic invalidation instead of taking a cache lock that can deadlock.
                    // The cache will automatically refresh this vector on the next read miss.
                    vector_cache_->invalidateSlot(cur_c);
                }
            }

            // std::unique_lock <std::shared_mutex> lock_el(getLinkListMutex(cur_c));

            // Initialize level 0 links
            // TODO - check if it is required
            {
                char* linklist = get_linklist0(cur_c);
                memset(linklist, 0, sizeLinksBaseLayer_);
            }

            // Create data in upper levels
            size_t total_size;
            if(curLevel > 0) {

                total_size = data_size_ + sizeof(levelInt) + curLevel * sizeLinksUpperLayers_;

                auto mem = std::make_unique<uint8_t[]>(total_size);

                                // copy vector
                                memcpy(mem.get(), datapoint, data_size_);
                                memcpy(mem.get() + data_size_, &curLevel, sizeof(levelInt));
                // zero initialize linklists
                                memset(mem.get() + data_size_ + sizeof(levelInt),
                       0,
                       curLevel * sizeLinksUpperLayers_);

                dataUpperLayer_[cur_c] = std::move(mem);
            }

            if(cur_c != 0) {

                levelInt maxlevelcopy = maxLevel_;

                // IMPORTANT: Check if this element has a higher level than the current max
                bool has_higher_level = (curLevel > maxlevelcopy);
                idhInt currObj = (update_seed != INVALID_ID) ? update_seed : entryPoint_;
                levelInt seed_level = getElementLevel(currObj);
                levelInt start_search_level = std::min(maxlevelcopy, seed_level);

                // Traverse to find closest neighbors at each level
                // Greedy search till the current level
                for(int level = start_search_level; level > curLevel; level--) {
                    bool changed = true;
                    while(changed) {
                        changed = false;
                        int* ll_cur;
                        ll_cur = (int*)get_linklist(currObj, level);
                        if(!ll_cur) {
                            continue;
                        }
                        int size = getListCount((idhInt*)ll_cur);
                        idhInt* datal = (idhInt*)(ll_cur + 1);

                        const uint8_t* curr_data = getUpperLayerDataPtr(currObj);
                        if(!curr_data) {
                            continue;
                        }

                        dist_t curr_sim = fstSimFunc_(datapoint, curr_data, dist_func_param_);

                        for(int i = 0; i < size; i++) {
                            idhInt candidate_id = datal[i];
                            dist_t s;
                            
                            const uint8_t* candidate_data = getUpperLayerDataPtr(candidate_id);
                            if(!candidate_data) {
                                continue;
                            }
                            
                            s = fstSimFunc_(datapoint, candidate_data, dist_func_param_);

                            if(s > curr_sim) {
                                curr_sim = s;
                                currObj = candidate_id;
                                changed = true;
                            }
                        }
                    }
                }

                // Add connections from curLevel down to 0
                levelInt connect_start_level = std::min(curLevel, start_search_level);
                for(int level = connect_start_level; level >= 0; level--) {
                    std::vector<std::pair<dist_t, idhInt>> sorted_candidates;
                    
                    const void* level_datapoint = datapoint;
                    
                    std::vector<idhInt> cur_eps = {currObj};
                    if(deletedElementsCount_) {
                        sorted_candidates = searchBaseLayer<true, true>(
                                cur_eps, level_datapoint, level, efConstruction_);
                    } else {  // No deleted elements
                        sorted_candidates = searchBaseLayer<true, false>(
                                cur_eps, level_datapoint, level, efConstruction_);
                    }
                    currObj = mutuallyConnectNewElement(
                            level_datapoint, cur_c, sorted_candidates, level);
                }

                if (has_higher_level) {
                   entryPoint_ = cur_c;
                   maxLevel_ = curLevel;
                }
            }
        }

        void markDelete(idInt label) {
            std::shared_lock<std::shared_mutex> lock(index_lock_);
            auto searchId = labelLookup_[label];
            if(searchId == INVALID_ID) {
                throw std::runtime_error("Label not found");
            }
            markDeletedInternal(searchId);
        }

        inline bool isMarkedDeleted(idhInt internal_id) const {
            const flagInt* flags = reinterpret_cast<const flagInt*>(get_linklist0(internal_id)
                                                                    + sizeLinksBaseLayer_);
            return (*flags & DELETE_MARK) != 0;
        }

        void resizeIndex(size_t new_max_elements) {
            std::unique_lock<std::shared_mutex> lock(index_lock_);
            if(new_max_elements < curElementsCount_) {
                throw std::runtime_error(
                        "Cannot resize, max element is less than the current number of elements");
            }

            // Reset and reallocate visited list pool with new size
            visited_list_pool_.reset(new VisitedListPool(1, new_max_elements));

            // Reallocate base layer (dataBaseLayer_)
            char* dataBaseLayer_new =
                    (char*)realloc(dataBaseLayer_, new_max_elements * sizeDataAtBaseLayer_);
            if(dataBaseLayer_new == nullptr) {
                throw std::runtime_error(
                        "Not enough memory: resizeIndex failed to allocate base layer");
            }
            dataBaseLayer_ = dataBaseLayer_new;

            // Reallocate upper layer (dataUpperLayer_)
            dataUpperLayer_.resize(new_max_elements);

            // Resize label lookup vector and fill it with INVALID_ID
            labelLookup_.resize(new_max_elements, INVALID_ID);

            // Update maxElements_ count
            maxElements_ = new_max_elements;
        }

    private:
        // Invalid id for the label
        static constexpr idhInt INVALID_ID = static_cast<idhInt>(-1);
        static const unsigned char DELETE_MARK = 0x01;
        // TODO - We need to pass indexId in the constructor.
        // This may be helpful for logs
        std::string indexId_;
        size_t M_{0};
        size_t M0_{0};
        size_t efConstruction_{0};
        size_t ef_{0};
        SpaceType space_type_;  // Now using SpaceType
        ndd::quant::QuantizationLevel quant_level_;
        int32_t checksum_;
        uint64_t flags_{0};  //Not using flags now. We can use it in future for various options
        std::unique_ptr<SpaceInterface<dist_t>> space_;

        size_t dimension_;

        VectorFetcher vector_fetcher_;
        VectorFetcherBatch vector_fetcher_batch_;
        mutable std::shared_mutex index_lock_;

        size_t maxElements_{0};
        mutable std::atomic<size_t> curElementsCount_{0};
        mutable std::atomic<size_t> deletedElementsCount_{0};
        size_t sizeDataAtBaseLayer_{0};
        // Link list size for upper levels for each level (a node may exit at multiple upper levels)
        size_t sizeLinksUpperLayers_{0};
        // Link list size for level 0
        size_t sizeLinksBaseLayer_{0};

        double mult_{0.0};
        levelInt maxLevel_{0};

        std::unique_ptr<VisitedListPool> visited_list_pool_{nullptr};
        mutable std::mutex global;
        mutable std::vector<std::shared_mutex> linkListLocks_;

        idhInt entryPoint_{0};

        size_t labelOffset_{0};

        // Stores link lists and labels
        // Structure: idInt + linklist + label
        char* dataBaseLayer_{nullptr};
        // Since upper layer can have variable layers, the size of the link list
        // is not fixed. So we use a vector of unique_ptrs to store the list
        // Structure: vector_data + level (unint32_t) + [idInt + linklist]
        std::vector<std::unique_ptr<uint8_t[]>> dataUpperLayer_;

        // This will vary based on fp16 or fp32
        size_t data_size_{0};
        DISTFUNC<dist_t> fstDistFunc_;
        SIMFUNC<dist_t> fstSimFunc_;
        void* dist_func_param_{nullptr};

        // Cache for vectors
        mutable std::unique_ptr<VectorCache> vector_cache_;
        static constexpr size_t CACHE_LOCK_STRIPE_BITS = 10; // 1024 striped locks in HNSW
        static constexpr size_t CACHE_LOCK_STRIPE_COUNT = 1 << CACHE_LOCK_STRIPE_BITS;
        static constexpr size_t CACHE_LOCK_STRIPE_MASK = CACHE_LOCK_STRIPE_COUNT - 1;
        mutable std::array<std::shared_mutex, CACHE_LOCK_STRIPE_COUNT> vectorCacheLocks_;

        struct CacheReadView {
            std::shared_lock<std::shared_mutex> lock;
            const uint8_t* data = nullptr;

            explicit operator bool() const {
                return data != nullptr;
            }
        };

        std::shared_mutex& getCacheMutex(idhInt internal_id) const {
            size_t cache_index = static_cast<size_t>(internal_id);
            if(vector_cache_) {
                cache_index = vector_cache_->getCacheIndex(internal_id);
            }
            size_t stripe_id = cache_index & CACHE_LOCK_STRIPE_MASK;
            return vectorCacheLocks_[stripe_id];
        }

    public:
        const VectorCache* getCache() const {
             return vector_cache_.get();
        }
        // Maps external label to internal id
        std::vector<idhInt> labelLookup_;

        std::default_random_engine level_generator_;
        std::default_random_engine update_probability_generator_;

    public:
        std::shared_mutex& getLinkListMutex(idhInt id) const {
            size_t lock_id = id & (settings::MAX_LINK_LIST_LOCKS - 1);
            return linkListLocks_[lock_id];
        }

        // Internal function to mark an element as deleted
        void markDeletedInternal(idhInt internal_id) {
            flagInt* flags =
                    reinterpret_cast<flagInt*>(get_linklist0(internal_id) + sizeLinksBaseLayer_);
            *flags |= DELETE_MARK;
            deletedElementsCount_++;
        }

        void unmarkDeletedInternal(idhInt internal_id) {
            flagInt* flags =
                    reinterpret_cast<flagInt*>(get_linklist0(internal_id) + sizeLinksBaseLayer_);
            *flags &= ~DELETE_MARK;
            deletedElementsCount_--;
        }

        // Generate level for a new point
        levelInt getRandomLevel(double mult) {
            std::uniform_real_distribution<double> distribution(0.0, 1.0);
            double r = -log(distribution(level_generator_)) * mult;
            return (levelInt)r;
        }

        const uint8_t* getUpperLayerDataPtr(idhInt internal_id) const {
            if(dataUpperLayer_[internal_id] == nullptr) {
                return nullptr;
            }
            return dataUpperLayer_[internal_id].get();
        }

        // Modified function returning bool and filling buffer.
        // For upper-layer vectors, data is returned zero-copy via cache_read_handle only.
        // For layer 0, callers can optionally receive zero-copy cache hits via cache_read_handle.
        bool getDataByInternalId(idhInt internal_id,
                                 levelInt layer,
                                 uint8_t* buffer,
                                 CacheReadView* cache_read_handle = nullptr) const {
            if(cache_read_handle) {
                *cache_read_handle = CacheReadView();
            }

            const uint8_t* upper_ptr = getUpperLayerDataPtr(internal_id);
            if(upper_ptr) {
                if(cache_read_handle) {
                    cache_read_handle->data = upper_ptr;
                    return true;
                }
                return false;
            }

            if(layer == 0) {
                // Check cache first
                if(vector_cache_) {
                    if(cache_read_handle) {
                        cache_read_handle->lock =
                                std::shared_lock<std::shared_mutex>(getCacheMutex(internal_id));
                        const uint8_t* cached_ptr = vector_cache_->getPointer(internal_id);
                        if(cached_ptr) {
                            cache_read_handle->data = cached_ptr;
                            return true;
                        }
                        cache_read_handle->lock.unlock();
                    }

                    if(buffer) {
                        std::shared_lock<std::shared_mutex> lock(getCacheMutex(internal_id));
                        const uint8_t* cached_ptr = vector_cache_->getPointer(internal_id);
                        if(cached_ptr) {
                            memcpy(buffer, cached_ptr, data_size_);
                            return true;
                        }
                    }
                }

                idInt external_label = getExternalLabel(internal_id);
                if(vector_fetcher_ && buffer) {
                    // Directly fetch to buffer
                    bool success = vector_fetcher_(external_label, buffer);
                    
                    // Populate cache on successful fetch
                    if (success && vector_cache_) {
                        std::unique_lock<std::shared_mutex> write_lock(getCacheMutex(internal_id), std::try_to_lock);
                        if(write_lock.owns_lock()) {
                            vector_cache_->insert(internal_id, buffer);
                        }
                    }
                    return success;
                }
                return false;
            }

            // layer > 0 path with no upper-layer blob available
            return false;
        }

        // Batch fetch for level 0: check upper-layer data/cache first, then fetch misses in one MDBX txn.
        // internal_ids: array of internal IDs to fetch
        // buffers: flat output buffer, count * data_size_ bytes
        // success: output array of bools
        // count: number of IDs
        // data_ptrs: optional per-item pointers to fetched data (zero-copy for upper-layer)
        void getDataByInternalIdBatch(const idhInt* internal_ids, uint8_t* buffers,
                                       bool* success, size_t count,
                                       const void** data_ptrs = nullptr) const {
            // Phase 1: Check cache for all IDs, collect misses
            std::vector<size_t> miss_indices;      // index into the batch
            std::vector<idInt> miss_labels;         // external labels for MDBX lookup
            miss_indices.reserve(count);
            miss_labels.reserve(count);

            for(size_t i = 0; i < count; i++) {
                uint8_t* buf = buffers + i * data_size_;
                if(data_ptrs) {
                    data_ptrs[i] = nullptr;
                }

                const uint8_t* upper_ptr = getUpperLayerDataPtr(internal_ids[i]);
                if(upper_ptr) {
                    if(data_ptrs) {
                        data_ptrs[i] = upper_ptr;
                    } else {
                        std::memcpy(buf, upper_ptr, data_size_);
                    }
                    success[i] = true;
                    continue;
                }

                if(vector_cache_) {
                    std::shared_lock<std::shared_mutex> lock(getCacheMutex(internal_ids[i]));
                    const uint8_t* cached_ptr = vector_cache_->getPointer(internal_ids[i]);
                    if(cached_ptr) {
                        std::memcpy(buf, cached_ptr, data_size_);
                        if(data_ptrs) {
                            data_ptrs[i] = buf;
                        }
                        success[i] = true;
                        continue;
                    }
                }

                success[i] = false;
                miss_indices.push_back(i);
                miss_labels.push_back(getExternalLabel(internal_ids[i]));
            }

            // Phase 2: Batch fetch all misses in one MDBX txn
            if(!miss_indices.empty() && vector_fetcher_batch_) {
                // Temp buffers for the batch fetch
                std::vector<uint8_t> miss_buffers(miss_indices.size() * data_size_);
                auto miss_success = std::make_unique<bool[]>(miss_indices.size());
                std::memset(miss_success.get(), 0, miss_indices.size() * sizeof(bool));

                vector_fetcher_batch_(miss_labels.data(), miss_buffers.data(),
                                      miss_success.get(), miss_indices.size());

                // Phase 3: Copy results back and populate cache
                for(size_t mi = 0; mi < miss_indices.size(); mi++) {
                    size_t i = miss_indices[mi];
                    if(miss_success[mi]) {
                        uint8_t* buf = buffers + i * data_size_;
                        std::memcpy(buf, miss_buffers.data() + mi * data_size_, data_size_);
                        if(data_ptrs) {
                            data_ptrs[i] = buf;
                        }
                        success[i] = true;
                        // Populate cache
                        if(vector_cache_) {
                                std::unique_lock<std::shared_mutex> write_lock(
                                    getCacheMutex(internal_ids[i]), std::try_to_lock);
                            if(write_lock.owns_lock()) {
                                vector_cache_->insert(internal_ids[i], buf);
                            }
                        }
                    }
                }
            } else if(!miss_indices.empty() && vector_fetcher_) {
                // Fallback: single-fetch for each miss
                for(size_t mi = 0; mi < miss_indices.size(); mi++) {
                    size_t i = miss_indices[mi];
                    uint8_t* buf = buffers + i * data_size_;
                    bool ok = vector_fetcher_(miss_labels[mi], buf);
                    success[i] = ok;
                    if(ok && data_ptrs) {
                        data_ptrs[i] = buf;
                    }
                    if(ok && vector_cache_) {
                        std::unique_lock<std::shared_mutex> write_lock(
                                getCacheMutex(internal_ids[i]), std::try_to_lock);
                        if(write_lock.owns_lock()) {
                            vector_cache_->insert(internal_ids[i], buf);
                        }
                    }
                }
            }
        }

        SimBatchFunc resolveBatchSimFunc() const {
            ndd::quant::QuantizerDispatch dispatch;
            try {
                dispatch = ndd::quant::get_quantizer_dispatch(quant_level_);
            } catch(...) {
                return nullptr;
            }

            switch(space_type_) {
                case L2_SPACE:
                    return dispatch.sim_l2_batch;
                case IP_SPACE:
                    return dispatch.sim_ip_batch;
                case COSINE_SPACE:
                    return dispatch.sim_cosine_batch;
                default:
                    return nullptr;
            }
        }

        void computeBatchSimilaritiesFromPtrs(const void* query_data,
                                              const std::vector<const void*>& vector_ptrs,
                                              SIMFUNC<dist_t> fallback_sim_func,
                                              void* dist_param,
                                              std::vector<dist_t>& out_sims) const {
            out_sims.clear();
            if(vector_ptrs.empty()) {
                return;
            }

            out_sims.resize(vector_ptrs.size());

            SimBatchFunc batch_func = resolveBatchSimFunc();
            if(!batch_func) {
                for(size_t i = 0; i < vector_ptrs.size(); ++i) {
                    out_sims[i] = fallback_sim_func(query_data, vector_ptrs[i], dist_param);
                }
                return;
            }

            if constexpr(std::is_same_v<dist_t, float>) {
                batch_func(query_data,
                           vector_ptrs.data(),
                           vector_ptrs.size(),
                           dist_param,
                           out_sims.data());
            } else {
                std::vector<float> sim_out(vector_ptrs.size());
                batch_func(query_data,
                           vector_ptrs.data(),
                           vector_ptrs.size(),
                           dist_param,
                           sim_out.data());

                for(size_t i = 0; i < sim_out.size(); ++i) {
                    out_sims[i] = static_cast<dist_t>(sim_out[i]);
                }
            }
        }

        char* get_linklist0(idhInt internal_id) const {
            return dataBaseLayer_ + internal_id * sizeDataAtBaseLayer_;
        }

        inline char* get_linklist(idhInt id, levelInt level) const {
            if(level == 0) {
                return get_linklist0(id);
            }
            // int levels = getElementLevel(id);
            // if (level > levels) return nullptr;
            return reinterpret_cast<char*>(
                    dataUpperLayer_[id].get() + data_size_ + sizeof(levelInt)
                    + (level - 1) * sizeLinksUpperLayers_

            );
        }
        inline idhInt getListCount(idhInt* ptr) const { return *ptr; }

        inline void setListCount(idhInt* ptr, idhInt size) const { *ptr = size; }

        idInt getExternalLabel(idhInt internal_id) const {
            idInt return_label;
            memcpy(&return_label,
                   dataBaseLayer_ + internal_id * sizeDataAtBaseLayer_ + labelOffset_,
                   sizeof(idInt));
            return return_label;
        }

        void setExternalLabel(idhInt internal_id, idInt label) const {
            memcpy(dataBaseLayer_ + internal_id * sizeDataAtBaseLayer_ + labelOffset_,
                   &label,
                   sizeof(idInt));
        }

        inline levelInt getElementLevel(idhInt id) const {
            if(!dataUpperLayer_[id]) {
                return 0;
            }
            return *reinterpret_cast<const levelInt*>(dataUpperLayer_[id].get() + data_size_);
        }

        // This function is used to get the neighbors based on heuristic
        // We let the neighbors grow beyond M (now curM) and then prune them based on heuristic
        // The input is a sorted list (reverse order) by similarity
        std::vector<std::pair<dist_t, idhInt>>
        getNeighborsByHeuristic2(const std::vector<std::pair<dist_t, idhInt>>& candidates_sorted,
                                 size_t curM,
                                 levelInt level) {
            if(candidates_sorted.size() <= curM) {
                return candidates_sorted;
            }

            std::vector<std::pair<dist_t, idhInt>> result;
            result.reserve(curM);

            std::vector<std::pair<dist_t, idhInt>> fill_back_ids;
            fill_back_ids.reserve(candidates_sorted.size() - curM);

            // Generic awareness
            auto curSimFunc = fstSimFunc_;
            auto curDistParam = dist_func_param_;
            size_t curDataSize = data_size_;

            std::vector<uint8_t> cand_buf(curDataSize);      // Only used for level 0
            // Cache selected vectors to avoid redundant re-fetches in inner loop
            // Without this, each selected vector is re-fetched O(candidates) times → O(M²) fetches
            std::vector<std::vector<uint8_t>> selected_vecs_cache;  // Only used for level 0
            if(level == 0) {
                selected_vecs_cache.reserve(curM);
            }

            for(const auto& candidate : candidates_sorted) {
                if(result.size() == curM) {
                    break;
                }

                const void* cand_vec = nullptr;
                CacheReadView cand_cache_handle;
                if(level == 0) {
                    if(getDataByInternalId(candidate.second,
                                           level,
                                           cand_buf.data(),
                                           &cand_cache_handle)) {
                        cand_vec = cand_cache_handle
                                   ? static_cast<const void*>(cand_cache_handle.data)
                                   : static_cast<const void*>(cand_buf.data());
                    }
                } else {
                    cand_vec = getUpperLayerDataPtr(candidate.second);
                }

                if(!cand_vec) {
                    continue;
                }

                bool good = true;
                dist_t sim;
                for(size_t si = 0; si < result.size(); si++) {
                    const void* selected_vec_ptr = nullptr;
                    if(level == 0) {
                        selected_vec_ptr = selected_vecs_cache[si].data();
                    } else {
                        selected_vec_ptr = getUpperLayerDataPtr(result[si].second);
                    }

                    if(!selected_vec_ptr) {
                        continue;
                    }

                    sim = curSimFunc(selected_vec_ptr, cand_vec, curDistParam);

                    if(sim > candidate.first) {
                        good = false;
                        break;
                    }
                }

                if(good) {
                    result.push_back(candidate);
                    // Cache the vector data so inner loop never re-fetches it
                    if(level == 0) {
                        selected_vecs_cache.emplace_back(
                            static_cast<const uint8_t*>(cand_vec),
                            static_cast<const uint8_t*>(cand_vec) + curDataSize);
                    }
                } else {
                    fill_back_ids.push_back(candidate);
                }
            }

            size_t current_backfill_buffer = (level == 0) ? (settings::BACKFILL_BUFFER * 2)
                                                          : settings::BACKFILL_BUFFER;

            size_t target_backfill_size = (curM > current_backfill_buffer)
                                                  ? (curM - current_backfill_buffer)
                                                  : 0;

            for(const auto& fb : fill_back_ids) {
                if(result.size() >= target_backfill_size) {
                    break;
                }
                result.push_back(fb);
            }

            return result;
        }

        // This function is used to connect the new element to its neighbors
        // It takes the data point, current element id, sorted candidates and level
        idhInt
        mutuallyConnectNewElement(const void* data_point,
                                  idhInt cur_c,
                                  const std::vector<std::pair<dist_t, idhInt>>& sorted_candidates,
                                  levelInt level) {
            LOG_TIME("mutuallyConnectNewElement");

            size_t curM = level ? M_ : M0_;

            // Generic awareness
            auto curSimFunc = fstSimFunc_;
            auto curDistParam = dist_func_param_;
            size_t curDataSize = data_size_;

            auto selected = getNeighborsByHeuristic2(sorted_candidates, curM, level);
            if(selected.empty()) {  // the graph is empty or disconnected
                return 0;           // Or better handling
            }

            idhInt next_closest_entry_point = selected[0].second;

            // Step 2: Set connections for cur_c
            {
                idhInt* ll_cur = reinterpret_cast<idhInt*>(level == 0 ? get_linklist0(cur_c)
                                                                      : get_linklist(cur_c, level));
                if(ll_cur) {
                    setListCount(ll_cur, selected.size());
                    idhInt* data = (ll_cur + 1);
                    for(size_t idx = 0; idx < selected.size(); idx++) {
                        data[idx] = selected[idx].second;
                    }
                }
            }

            // Step 3: Add cur_c to neighbors' lists
            std::vector<uint8_t> neighbor_buf(curDataSize);  // Used for level 0
            std::vector<uint8_t> data_buf(curDataSize);      // Used for level 0

            for(const auto& p : selected) {
                idhInt neighbor = p.second;
                if(neighbor == cur_c) {
                    continue;
                }

                std::unique_lock<std::shared_mutex> lock(getLinkListMutex(neighbor));
                idhInt* ll_other = reinterpret_cast<idhInt*>(
                        level == 0 ? get_linklist0(neighbor) : get_linklist(neighbor, level));
                if(!ll_other) {
                    continue;
                }

                idhInt sz = getListCount(ll_other);
                idhInt* data = (ll_other + 1);

                if(sz < curM) {
                    data[sz] = cur_c;
                    setListCount(ll_other, sz + 1);
                } else {
                    const void* neighbor_data = nullptr;
                    CacheReadView neighbor_cache_handle;
                    if(level == 0) {
                        if(getDataByInternalId(neighbor,
                                               level,
                                               neighbor_buf.data(),
                                               &neighbor_cache_handle)) {
                            neighbor_data = neighbor_cache_handle
                                            ? static_cast<const void*>(neighbor_cache_handle.data)
                                            : static_cast<const void*>(neighbor_buf.data());
                        }
                    } else {
                        neighbor_data = getUpperLayerDataPtr(neighbor);
                    }

                    if(!neighbor_data) {
                        continue;
                    }

                    std::vector<std::pair<dist_t, idhInt>> all_candidates;
                    all_candidates.reserve(sz + 1);

                    all_candidates.emplace_back(curSimFunc(neighbor_data, data_point, curDistParam),
                                                cur_c);

                    for(size_t j = 0; j < sz; j++) {
                        dist_t sim;
                        const void* other_neighbor_data = nullptr;
                        CacheReadView other_cache_handle;
                        if(level == 0) {
                            if(getDataByInternalId(data[j],
                                                   level,
                                                   data_buf.data(),
                                                   &other_cache_handle)) {
                                other_neighbor_data = other_cache_handle
                                                      ? static_cast<const void*>(other_cache_handle.data)
                                                      : static_cast<const void*>(data_buf.data());
                            }
                        } else {
                            other_neighbor_data = getUpperLayerDataPtr(data[j]);
                        }
                        if(!other_neighbor_data) {
                            continue;
                        }

                        sim = curSimFunc(neighbor_data, other_neighbor_data, curDistParam);

                        all_candidates.emplace_back(sim, data[j]);
                    }
                    std::sort(all_candidates.begin(),
                              all_candidates.end(),
                              [](const auto& a, const auto& b) { return a.first > b.first; });

                    auto pruned = getNeighborsByHeuristic2(all_candidates, curM, level);
                    for(size_t j = 0; j < pruned.size(); j++) {
                        data[j] = pruned[j].second;
                    }
                    setListCount(ll_other, pruned.size());
                }
            }

            return next_closest_entry_point;
        }

        // Search function for the base layer
        // Returns a vector of top candidates sorted by similarity (1-distance) in reverse order
        template <bool is_insert, bool has_deletions, typename FilterFunctor = void>
        std::vector<std::pair<dist_t, idhInt>>
        searchBaseLayer(const std::vector<idhInt>& ep_ids, 
                        const void* data_point, 
                        idhInt layer, 
                        size_t ef, 
                        FilterFunctor* filter = nullptr, 
                        size_t filter_boost_percentage = settings::FILTER_BOOST_PERCENTAGE) const {
            LOG_TIME("searchBaseLayer");
            VisitedList* vl = visited_list_pool_->getFreeVisitedList();
            vl_type* visited_array = vl->mass;
            vl_type visited_array_tag = vl->curV;

            max_heap_pq candidate_set;
            min_heap_pq top_candidates;

            // Generic awareness
            auto curSimFunc = fstSimFunc_;
            auto curDistParam = dist_func_param_;
            size_t curDataSize = data_size_;
            std::vector<uint8_t> buffer;
            if(layer == 0) {
                buffer.resize(curDataSize);
            }

            size_t dist_computations = 0;
            dist_t lowerBound = std::numeric_limits<dist_t>::lowest();

            for (idhInt ep_id : ep_ids) {
                if (visited_array[ep_id] == visited_array_tag) {
                    continue;
                }
                visited_array[ep_id] = visited_array_tag;

                dist_t sim = std::numeric_limits<dist_t>::lowest();
                if(!has_deletions || !isMarkedDeleted(ep_id)) {
                    const void* vec_data = nullptr;
                    CacheReadView ep_cache_handle;
                    if(layer == 0) {
                        if(getDataByInternalId(ep_id,
                                               layer,
                                               buffer.data(),
                                               &ep_cache_handle)) {
                            vec_data = ep_cache_handle
                                       ? static_cast<const void*>(ep_cache_handle.data)
                                       : static_cast<const void*>(buffer.data());
                        }
                    } else {
                        vec_data = getUpperLayerDataPtr(ep_id);
                    }

                    if(vec_data) {
                        sim = curSimFunc(data_point, vec_data, curDistParam);
                        dist_computations++;

                        if constexpr(std::is_same_v<FilterFunctor, void>) {
                            top_candidates.emplace(sim, ep_id);
                            candidate_set.emplace(sim, ep_id);
                        } else if constexpr(std::is_same_v<FilterFunctor, BaseFilterFunctor>) {
                            // Virtual call path
                            bool allowed = !filter || (*filter)(getExternalLabel(ep_id));
                            candidate_set.emplace(sim, ep_id); // Always explore
                            if (allowed) {
                                top_candidates.emplace(sim, ep_id);
                            }
                        } else {
                            // Templated path
                            bool allowed = !filter || (*filter)(getExternalLabel(ep_id));
                            candidate_set.emplace(sim, ep_id);
                            if (allowed) {
                                top_candidates.emplace(sim, ep_id);
                            }
                        }
                        
                        // Maintain ef size in top_candidates during init
                        if(top_candidates.size() > ef) {
                             top_candidates.pop();
                        }
                    } else {
                        // Data fetch failed
                         candidate_set.emplace(std::numeric_limits<dist_t>::lowest(), ep_id);
                    }
                } else {
                    // Deleted
                    candidate_set.emplace(std::numeric_limits<dist_t>::lowest(), ep_id);
                }
            }

            if (!top_candidates.empty()) {
                lowerBound = top_candidates.top().first;
            }

            int below_threshold_count = 0;
            int max_below_threshold = is_insert ? settings::EARLY_EXIT_BUFFER_INSERT
                                                : settings::EARLY_EXIT_BUFFER_QUERY;

            // Progressive Fatigue Logic:
            
            // Base budget: ef * M . 
            size_t fatigue_base = ef * M_;

            // Apply filter boost if filter is active
            if constexpr(!std::is_same_v<FilterFunctor, void>) {
                 if (filter != nullptr && filter_boost_percentage > 0) {
                     fatigue_base = fatigue_base * (100 + filter_boost_percentage) / 100;
                 }
            }

            size_t fatigue_tail = fatigue_base * 5; // Taper duration

            while(!candidate_set.empty()) {
                auto current_pair = candidate_set.top();
                idhInt current_id = current_pair.second;
                // Early exit if we have enough candidates
                if(current_pair.first < lowerBound && top_candidates.size() >= ef) {
                    below_threshold_count++;
                    if(below_threshold_count > max_below_threshold) {
                        break;
                    }
                } else {
                    below_threshold_count = 0;
                }

                candidate_set.pop();

                // Get neighbors
                idhInt* data = (layer == 0) ? (idhInt*)get_linklist0(current_id)
                                            : (idhInt*)get_linklist(current_id, layer);
                if(!data) {
                    LOG_DEBUG("No linklist found for id: " << current_id);
                    continue;
                }
                idhInt size = getListCount((idhInt*)data);
                idhInt* datal = (idhInt*)(data + 1);

                // --- Batch prefetch path for layer 0 ---
                if(layer == 0) {
                    // Phase 1: Collect valid (non-visited, non-deleted) candidate IDs
                    std::vector<idhInt> valid_ids;
                    valid_ids.reserve(size);
                    for(idhInt j = 0; j < size; j++) {
                        idhInt candidate_id = *(datal + j);
                        if(visited_array[candidate_id] == visited_array_tag) continue;
                        visited_array[candidate_id] = visited_array_tag;
                        if(has_deletions && isMarkedDeleted(candidate_id)) continue;
                        valid_ids.push_back(candidate_id);
                    }

                    if(valid_ids.empty()) continue;

                    // Phase 2: Batch fetch all vectors
                    std::vector<uint8_t> batch_buffers(valid_ids.size() * data_size_);
                    std::vector<const void*> batch_ptrs(valid_ids.size(), nullptr);
                    auto batch_success = std::make_unique<bool[]>(valid_ids.size());
                    std::memset(batch_success.get(), 0, valid_ids.size() * sizeof(bool));
                    getDataByInternalIdBatch(valid_ids.data(), batch_buffers.data(),
                                             batch_success.get(), valid_ids.size(),
                                             batch_ptrs.data());

                    // Phase 3: Process fetched vectors
                    std::vector<idhInt> pass_filter_candidate_ids;
                    pass_filter_candidate_ids.reserve(valid_ids.size());
                    std::vector<const void*> pass_filter_ptrs;
                    pass_filter_ptrs.reserve(valid_ids.size());

                    for(size_t vi = 0; vi < valid_ids.size(); vi++) {
                        if(!batch_success[vi]) continue;

                        idhInt candidate_id = valid_ids[vi];
                        const void* neighbor_data = batch_ptrs[vi];
                        if(!neighbor_data) {
                            continue;
                        }

                        bool pass_filter = true;
                        if constexpr(!std::is_same_v<FilterFunctor, void>) {
                            if(filter != nullptr) {
                                if(!(*filter)(getExternalLabel(candidate_id))) {
                                    pass_filter = false;
                                }
                            }
                        }

                        if(!pass_filter) {
                            if(dist_computations > fatigue_base) {
                                size_t excess = dist_computations - fatigue_base;
                                if(excess >= fatigue_tail) continue;
                                size_t drop_prob = (excess * 255) / fatigue_tail;
                                size_t hash = (candidate_id * 104729) & 0xFF;
                                if(hash < drop_prob) continue;
                            }

                            dist_t sim = curSimFunc(data_point, neighbor_data, curDistParam);
                            dist_computations++;
                            if(top_candidates.size() < ef || sim > lowerBound) {
                                candidate_set.emplace(sim, candidate_id);
                            }
                            continue;
                        }

                        pass_filter_candidate_ids.push_back(candidate_id);
                        pass_filter_ptrs.push_back(neighbor_data);
                    }

                    std::vector<dist_t> pass_filter_sims;
                    computeBatchSimilaritiesFromPtrs(data_point,
                                             pass_filter_ptrs,
                                             curSimFunc,
                                             curDistParam,
                                             pass_filter_sims);

                    for(size_t pi = 0; pi < pass_filter_candidate_ids.size(); ++pi) {
                        idhInt candidate_id = pass_filter_candidate_ids[pi];
                        dist_t sim = pass_filter_sims[pi];
                        dist_computations++;

                        if(top_candidates.size() < ef || sim > lowerBound) {
                            candidate_set.emplace(sim, candidate_id);
                            if(!has_deletions || !isMarkedDeleted(candidate_id)) {
                                top_candidates.emplace(sim, candidate_id);
                                if(top_candidates.size() > ef) {
                                    top_candidates.pop();
                                }
                                if(!top_candidates.empty()) {
                                    lowerBound = top_candidates.top().first;
                                }
                            }
                        }
                    }
                } else {
                    // --- Upper layer path: data is in-memory, no batching needed ---
                    for(idhInt j = 0; j < size; j++) {
                        idhInt candidate_id = *(datal + j);
                        if(visited_array[candidate_id] == visited_array_tag) continue;
                        visited_array[candidate_id] = visited_array_tag;
                        if(has_deletions && isMarkedDeleted(candidate_id)) continue;

                        const void* neighbor_data = getUpperLayerDataPtr(candidate_id);
                        if(!neighbor_data) continue;

                        bool pass_filter = true;
                        if constexpr(!std::is_same_v<FilterFunctor, void>) {
                            if (filter != nullptr) {
                                if (!(*filter)(getExternalLabel(candidate_id))) {
                                    pass_filter = false;
                                }
                            }
                        }

                        dist_t sim;
                        if(!pass_filter) {
                            if (dist_computations > fatigue_base) {
                                size_t excess = dist_computations - fatigue_base;
                                if (excess >= fatigue_tail) continue;
                                size_t drop_prob = (excess * 255) / fatigue_tail;
                                size_t hash = (candidate_id * 104729) & 0xFF;
                                if (hash < drop_prob) continue;
                            }
                            sim = curSimFunc(data_point, neighbor_data, curDistParam);
                            dist_computations++;
                            if (top_candidates.size() < ef || sim > lowerBound) {
                                candidate_set.emplace(sim, candidate_id);
                            }
                            continue;
                        }

                        sim = curSimFunc(data_point, neighbor_data, curDistParam);
                        dist_computations++;

                        if(top_candidates.size() < ef || sim > lowerBound) {
                            candidate_set.emplace(sim, candidate_id);
                            if(!has_deletions || !isMarkedDeleted(candidate_id)) {
                                top_candidates.emplace(sim, candidate_id);
                                if(top_candidates.size() > ef) {
                                    top_candidates.pop();
                                }
                                if(!top_candidates.empty()) {
                                    lowerBound = top_candidates.top().first;
                                }
                            }
                        }
                    }
                }
            }

            visited_list_pool_->releaseVisitedList(vl);
            std::vector<std::pair<dist_t, idhInt>> sorted_candidates;
            sorted_candidates.reserve(top_candidates.size());
            while(!top_candidates.empty()) {
                sorted_candidates.push_back(top_candidates.top());
                top_candidates.pop();
            }
            std::reverse(sorted_candidates.begin(), sorted_candidates.end());
            return sorted_candidates;
        }
        // This function removes all connections to and from the target element across all levels
        // There is a new version removeAllConnections2 that tries to reconnect neighbors to each other instead of just removing the target element from their lists 
        void removeAllConnections(idhInt internal_id, levelInt elem_level) {

            for(int level = 0; level <= elem_level; ++level) {
                idhInt* ll_self = (idhInt*)get_linklist(internal_id, level);
                idhInt size = getListCount(ll_self);
                idhInt* neighbors = (idhInt*)(ll_self + 1);

                for(idhInt i = 0; i < size; ++i) {
                    idhInt neighbor_id = neighbors[i];
                    if(neighbor_id >= curElementsCount_) {
                        continue;
                    }

                    // Fix neighbor's link list
                    {
                        std::unique_lock<std::shared_mutex> lock_neighbor(
                                getLinkListMutex(neighbor_id));
                        idhInt* ll_other = (idhInt*)get_linklist(neighbor_id, level);
                        idhInt sz = getListCount(ll_other);
                        idhInt* data = (idhInt*)(ll_other + 1);

                        idhInt new_size = 0;
                        for(idhInt j = 0; j < sz; ++j) {
                            if(data[j] != internal_id) {
                                data[new_size++] = data[j];
                            }
                        }
                        setListCount(ll_other, new_size);
                    }
                }
                // Now clear own links (make neighbor count 0)
                std::unique_lock<std::shared_mutex> lock_self(getLinkListMutex(internal_id));
                setListCount(ll_self, 0);
            }
        }
        // This is a more efficient version of removeAllConnections that reconnects neighbors to each other  
        // instead of just removing the target element from their lists
        void removeAllConnections2(idhInt internal_id, levelInt elem_level) {
            auto containsNeighborNoLock = [this](idhInt* linklist, idhInt target) {
                idhInt sz = getListCount(linklist);
                idhInt* data = linklist + 1;
                for(idhInt i = 0; i < sz; ++i) {
                    if(data[i] == target) {
                        return true;
                    }
                }
                return false;
            };

            auto connectPair = [&](idhInt left, idhInt right, levelInt level) {
                if(left == right || left >= curElementsCount_ || right >= curElementsCount_) {
                    return;
                }

                size_t max_degree = level ? M_ : M0_;
                auto& left_mutex = getLinkListMutex(left);
                auto& right_mutex = getLinkListMutex(right);

                if(&left_mutex == &right_mutex) {
                    std::unique_lock<std::shared_mutex> lock(left_mutex);

                    idhInt* ll_left = (idhInt*)get_linklist(left, level);
                    idhInt* ll_right = (idhInt*)get_linklist(right, level);
                    if(!ll_left || !ll_right) {
                        return;
                    }

                    if(containsNeighborNoLock(ll_left, right)
                       || containsNeighborNoLock(ll_right, left)) {
                        return;
                    }

                    idhInt left_sz = getListCount(ll_left);
                    idhInt right_sz = getListCount(ll_right);
                    if(left_sz >= static_cast<idhInt>(max_degree)
                       || right_sz >= static_cast<idhInt>(max_degree)) {
                        return;
                    }

                    (ll_left + 1)[left_sz] = right;
                    (ll_right + 1)[right_sz] = left;
                    setListCount(ll_left, left_sz + 1);
                    setListCount(ll_right, right_sz + 1);
                    return;
                }

                idhInt first_id = left < right ? left : right;
                idhInt second_id = left < right ? right : left;

                auto& first_mutex = getLinkListMutex(first_id);
                auto& second_mutex = getLinkListMutex(second_id);

                std::unique_lock<std::shared_mutex> first_lock(first_mutex);
                std::unique_lock<std::shared_mutex> second_lock(second_mutex);

                idhInt* ll_left = (idhInt*)get_linklist(left, level);
                idhInt* ll_right = (idhInt*)get_linklist(right, level);
                if(!ll_left || !ll_right) {
                    return;
                }
                if(containsNeighborNoLock(ll_left, right)
                   || containsNeighborNoLock(ll_right, left)) {
                    return;
                }

                idhInt left_sz = getListCount(ll_left);
                idhInt right_sz = getListCount(ll_right);
                if(left_sz >= static_cast<idhInt>(max_degree)
                   || right_sz >= static_cast<idhInt>(max_degree)) {
                    return;
                }

                (ll_left + 1)[left_sz] = right;
                (ll_right + 1)[right_sz] = left;
                setListCount(ll_left, left_sz + 1);
                setListCount(ll_right, right_sz + 1);
            };

            for(int level = 0; level <= elem_level; ++level) {
                idhInt* ll_self = (idhInt*)get_linklist(internal_id, level);
                if(!ll_self) {
                    continue;
                }
                std::vector<idhInt> level_neighbors;

                {
                    std::unique_lock<std::shared_mutex> lock_self(getLinkListMutex(internal_id));

                    idhInt size = getListCount(ll_self);
                    idhInt* neighbors = (idhInt*)(ll_self + 1);
                    level_neighbors.reserve(size);

                    for(idhInt i = 0; i < size; ++i) {
                        idhInt neighbor_id = neighbors[i];
                        if(neighbor_id >= curElementsCount_ || neighbor_id == internal_id) {
                            continue;
                        }
                        level_neighbors.push_back(neighbor_id);
                    }

                    setListCount(ll_self, 0);
                }

                std::sort(level_neighbors.begin(), level_neighbors.end());
                level_neighbors.erase(
                        std::unique(level_neighbors.begin(), level_neighbors.end()),
                        level_neighbors.end());

                for(idhInt neighbor_id : level_neighbors) {
                    std::unique_lock<std::shared_mutex> lock_neighbor(getLinkListMutex(neighbor_id));
                    idhInt* ll_other = (idhInt*)get_linklist(neighbor_id, level);
                    if(!ll_other) {
                        continue;
                    }

                    idhInt sz = getListCount(ll_other);
                    idhInt* data = (idhInt*)(ll_other + 1);

                    idhInt new_size = 0;
                    for(idhInt j = 0; j < sz; ++j) {
                        if(data[j] != internal_id) {
                            data[new_size++] = data[j];
                        }
                    }
                    setListCount(ll_other, new_size);
                }

                size_t left = 0;
                if(!level_neighbors.empty()) {
                    size_t right = level_neighbors.size() - 1;
                    while(left < right) {
                        connectPair(level_neighbors[left], level_neighbors[right], level);
                        ++left;
                        --right;
                    }
                }
            }
        }
    };
}  // namespace hnswlib
