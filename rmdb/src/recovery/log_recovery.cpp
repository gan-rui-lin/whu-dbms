/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include "record/rm_scan.h"

namespace {
std::unique_ptr<LogRecord> make_log_record(LogType type) {
    switch (type) {
        case LogType::INSERT: return std::make_unique<InsertLogRecord>();
        case LogType::DELETE: return std::make_unique<DeleteLogRecord>();
        case LogType::UPDATE: return std::make_unique<UpdateLogRecord>();
        case LogType::begin: return std::make_unique<BeginLogRecord>();
        case LogType::commit: return std::make_unique<CommitLogRecord>();
        case LogType::ABORT: return std::make_unique<AbortLogRecord>();
    }
    throw InternalError("unknown log record type");
}

bool has_record(RmFileHandle *file, const Rid &rid) {
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.page_no >= file->get_file_hdr().num_pages) return false;
    return file->is_record(rid);
}
}  // namespace

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    logs_.clear();
    committed_txns_.clear();
    aborted_txns_.clear();
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;

    int offset = 0;
    char header[LOG_HEADER_SIZE];
    while (offset + LOG_HEADER_SIZE <= file_size) {
        int read = disk_manager_->read_log(header, LOG_HEADER_SIZE, offset);
        if (read != LOG_HEADER_SIZE) break;
        LogType type = *reinterpret_cast<LogType *>(header + OFFSET_LOG_TYPE);
        uint32_t length = *reinterpret_cast<uint32_t *>(header + OFFSET_LOG_TOT_LEN);
        if (length < LOG_HEADER_SIZE || length > static_cast<uint32_t>(LOG_BUFFER_SIZE) ||
            offset + static_cast<int>(length) > file_size) {
            break;  // Ignore a torn tail record after a crash.
        }
        std::vector<char> bytes(length);
        if (disk_manager_->read_log(bytes.data(), length, offset) != static_cast<int>(length)) break;
        auto record = make_log_record(type);
        record->deserialize(bytes.data());
        if (record->log_type_ == LogType::commit) committed_txns_.insert(record->log_tid_);
        if (record->log_type_ == LogType::ABORT) aborted_txns_.insert(record->log_tid_);
        logs_.push_back(std::move(record));
        offset += length;
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    for (const auto &base : logs_) {
        if (aborted_txns_.count(base->log_tid_) != 0) continue;
        if (base->log_type_ != LogType::INSERT && base->log_type_ != LogType::DELETE &&
            base->log_type_ != LogType::UPDATE) continue;

        std::string table_name;
        Rid rid{};
        if (base->log_type_ == LogType::INSERT) {
            auto *log = static_cast<InsertLogRecord *>(base.get());
            table_name = log->table_name_;
            rid = log->rid_;
            if (!sm_manager_->db_.is_table(table_name)) continue;
            auto *file = sm_manager_->fhs_.at(table_name).get();
            if (!has_record(file, rid)) file->insert_record(rid, log->insert_value_.data);
        } else if (base->log_type_ == LogType::DELETE) {
            auto *log = static_cast<DeleteLogRecord *>(base.get());
            table_name = log->table_name_;
            rid = log->rid_;
            if (!sm_manager_->db_.is_table(table_name)) continue;
            auto *file = sm_manager_->fhs_.at(table_name).get();
            if (has_record(file, rid)) file->delete_record(rid, nullptr);
        } else {
            auto *log = static_cast<UpdateLogRecord *>(base.get());
            table_name = log->table_name_;
            rid = log->rid_;
            if (!sm_manager_->db_.is_table(table_name)) continue;
            auto *file = sm_manager_->fhs_.at(table_name).get();
            if (has_record(file, rid)) file->update_record(rid, log->new_value_.data, nullptr);
        }
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    for (auto it = logs_.rbegin(); it != logs_.rend(); ++it) {
        LogRecord *base = it->get();
        if (committed_txns_.count(base->log_tid_) != 0 || aborted_txns_.count(base->log_tid_) != 0) continue;
        if (base->log_type_ == LogType::INSERT) {
            auto *log = static_cast<InsertLogRecord *>(base);
            if (!sm_manager_->db_.is_table(log->table_name_)) continue;
            auto *file = sm_manager_->fhs_.at(log->table_name_).get();
            if (has_record(file, log->rid_)) file->delete_record(log->rid_, nullptr);
        } else if (base->log_type_ == LogType::DELETE) {
            auto *log = static_cast<DeleteLogRecord *>(base);
            if (!sm_manager_->db_.is_table(log->table_name_)) continue;
            auto *file = sm_manager_->fhs_.at(log->table_name_).get();
            if (!has_record(file, log->rid_)) file->insert_record(log->rid_, log->delete_value_.data);
        } else if (base->log_type_ == LogType::UPDATE) {
            auto *log = static_cast<UpdateLogRecord *>(base);
            if (!sm_manager_->db_.is_table(log->table_name_)) continue;
            auto *file = sm_manager_->fhs_.at(log->table_name_).get();
            if (has_record(file, log->rid_)) file->update_record(log->rid_, log->old_value_.data, nullptr);
        }
    }
    rebuild_indexes();
    for (auto &table_entry : sm_manager_->fhs_) {
        RmFileHandle *file = table_entry.second.get();
        file->rebuild_free_page_list();
        RmFileHdr header = file->get_file_hdr();
        buffer_pool_manager_->flush_all_pages(file->GetFd());
        disk_manager_->write_page(file->GetFd(), RM_FILE_HDR_PAGE,
                                  reinterpret_cast<const char *>(&header), sizeof(header));
    }
    disk_manager_->reset_log();
}

void RecoveryManager::rebuild_indexes() {
    IxManager *ix_manager = sm_manager_->get_ix_manager();
    for (auto &table_entry : sm_manager_->fhs_) {
        const std::string &table_name = table_entry.first;
        TabMeta &table = sm_manager_->db_.get_table(table_name);
        for (const auto &index : table.indexes) {
            std::string index_name = ix_manager->get_index_name(table_name, index.cols);
            auto open = sm_manager_->ihs_.find(index_name);
            if (open != sm_manager_->ihs_.end()) {
                ix_manager->close_index(open->second.get());
                sm_manager_->ihs_.erase(open);
            }
            if (ix_manager->exists(table_name, index.cols)) ix_manager->destroy_index(table_name, index.cols);
            ix_manager->create_index(table_name, index.cols);
            auto handle = ix_manager->open_index(table_name, index.cols);
            auto *raw_handle = handle.get();
            sm_manager_->ihs_[index_name] = std::move(handle);

            RmFileHandle *file = sm_manager_->fhs_.at(table_name).get();
            for (RmScan scan(file); !scan.is_end(); scan.next()) {
                auto record = file->get_record(scan.rid(), nullptr);
                std::vector<char> key(index.col_tot_len);
                int key_offset = 0;
                for (const auto &col : index.cols) {
                    memcpy(key.data() + key_offset, record->data + col.offset, col.len);
                    key_offset += col.len;
                }
                raw_handle->insert_entry(key.data(), scan.rid(), nullptr);
            }
            ix_manager->close_index(raw_handle);
            sm_manager_->ihs_.erase(index_name);
            sm_manager_->ihs_[index_name] = ix_manager->open_index(table_name, index.cols);
        }
    }
}
