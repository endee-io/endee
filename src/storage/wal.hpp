// wal.hpp
#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cstring>
#include <array>
#include "../core/types.hpp"
#include "../utils/log.hpp"
#include "mdbx/mdbx.h"

enum class WALOperationType : uint8_t { VECTOR_ADD = 1, VECTOR_DELETE = 2, VECTOR_UPDATE = 3 };

class WriteAheadLog {
public:
    static constexpr size_t PACKED_ENTRY_SIZE = 1 + sizeof(ndd::idInt);

    // WAL entry structure for operations
    struct WALEntry {
        WALOperationType op_type;
        ndd::idInt numeric_id;
    };

private:
    std::string index_id_;
    std::atomic<bool> enabled_{true};

    // XXX entry_count_ can drift on abort — see docs/followups.md.
    std::atomic<size_t> entry_count_{0};
    MDBX_env* env_ = nullptr;
    MDBX_dbi dbi_ = 0;

    static std::array<uint8_t, PACKED_ENTRY_SIZE> packEntry(const WALEntry& entry) {
        std::array<uint8_t, PACKED_ENTRY_SIZE> bytes{};
        bytes[0] = static_cast<uint8_t>(entry.op_type);
        std::memcpy(bytes.data() + 1, &entry.numeric_id, sizeof(entry.numeric_id));
        return bytes;
    }

    static bool unpackEntry(const MDBX_val& data, WALEntry& out) {
        if(data.iov_len != PACKED_ENTRY_SIZE) {
            return false;
        }
        const auto* bytes = static_cast<const uint8_t*>(data.iov_base);
        out.op_type = static_cast<WALOperationType>(bytes[0]);
        std::memcpy(&out.numeric_id, bytes + 1, sizeof(out.numeric_id));
        return true;
    }

    void init_mdbx_dbi() {
        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_READWRITE, &txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to begin op_log init transaction: "
                                     + std::string(mdbx_strerror(rc)));
        }

        rc = mdbx_dbi_open(txn, "op_log", MDBX_CREATE | MDBX_INTEGERKEY, &dbi_);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            throw std::runtime_error("Failed to open op_log DBI: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_stat stat{};
        rc = mdbx_dbi_stat(txn, dbi_, &stat, sizeof(stat));
        if(rc == MDBX_SUCCESS && stat.ms_entries > 0) {
            entry_count_ = stat.ms_entries;
        }

        rc = mdbx_txn_commit(txn);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to commit op_log init transaction: "
                                     + std::string(mdbx_strerror(rc)));
        }
    }

    uint64_t next_sequence(MDBX_txn* txn) {
        MDBX_cursor* cursor = nullptr;
        int rc = mdbx_cursor_open(txn, dbi_, &cursor);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to open op_log cursor: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_val key{};
        MDBX_val data{};
        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_LAST);
        uint64_t next = 0;
        if(rc == MDBX_SUCCESS && key.iov_len == sizeof(uint64_t)) {
            uint64_t last = 0;
            std::memcpy(&last, key.iov_base, sizeof(last));
            next = last + 1;
        } else if(rc != MDBX_NOTFOUND) {
            mdbx_cursor_close(cursor);
            throw std::runtime_error("Failed to seek op_log cursor: "
                                     + std::string(mdbx_strerror(rc)));
        }
        mdbx_cursor_close(cursor);
        return next;
    }

public:
    WriteAheadLog(MDBX_env* env, const std::string& index_id) :
        index_id_(index_id),
        env_(env) {
        init_mdbx_dbi();
    }

    ~WriteAheadLog() {
        if(env_ && dbi_) {
            mdbx_dbi_close(env_, dbi_);
        }
    }

    // Check if WAL has entries that need recovery
    bool hasEntries() const { return entry_count_ > 0; }
    // Get the number of entries added since last clear
    size_t getEntryCount() const { return entry_count_.load(); }
    void log(MDBX_txn* txn, const std::vector<WALEntry>& entries) {
        if(!enabled_ || entries.empty()) {
            return;
        }

        uint64_t seq = next_sequence(txn);
        for(const auto& entry : entries) {
            auto packed = packEntry(entry);
            MDBX_val key{&seq, sizeof(seq)};
            MDBX_val data{packed.data(), packed.size()};
            int rc = mdbx_put(txn, dbi_, &key, &data, MDBX_APPEND);
            if(rc != MDBX_SUCCESS) {
                throw std::runtime_error("Failed to append op_log entry: "
                                         + std::string(mdbx_strerror(rc)));
            }
            ++seq;
        }
        entry_count_ += entries.size();
    }

    std::vector<WALEntry> readEntries() {
        std::vector<WALEntry> entries;

        MDBX_txn* txn = nullptr;
        int rc = mdbx_txn_begin(env_, nullptr, MDBX_TXN_RDONLY, &txn);
        if(rc != MDBX_SUCCESS) {
            return entries;
        }

        MDBX_cursor* cursor = nullptr;
        rc = mdbx_cursor_open(txn, dbi_, &cursor);
        if(rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return entries;
        }

        MDBX_val key{};
        MDBX_val data{};
        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST);
        while(rc == MDBX_SUCCESS) {
            WALEntry entry{};
            if(unpackEntry(data, entry)) {
                entries.push_back(entry);
            }
            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
        }

        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        return entries;
    }

    void clear(MDBX_txn* txn) {
        MDBX_cursor* cursor = nullptr;
        int rc = mdbx_cursor_open(txn, dbi_, &cursor);
        if(rc != MDBX_SUCCESS) {
            throw std::runtime_error("Failed to open op_log clear cursor: "
                                     + std::string(mdbx_strerror(rc)));
        }

        MDBX_val key{};
        MDBX_val data{};
        rc = mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST);
        while(rc == MDBX_SUCCESS) {
            int del_rc = mdbx_cursor_del(cursor, static_cast<MDBX_put_flags_t>(0));
            if(del_rc != MDBX_SUCCESS) {
                mdbx_cursor_close(cursor);
                throw std::runtime_error("Failed to delete op_log entry: "
                                         + std::string(mdbx_strerror(del_rc)));
            }
            rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
        }
        mdbx_cursor_close(cursor);
        if(rc != MDBX_NOTFOUND) {
            throw std::runtime_error("Failed while clearing op_log: "
                                     + std::string(mdbx_strerror(rc)));
        }
        entry_count_ = 0;
    }

    void disable() { enabled_ = false; }
    void enable() { enabled_ = true; }
    MDBX_env* env() const { return env_; }
};
