/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstring>
#include <algorithm>
#include "log_manager.h"

namespace {
void serialize_record(char *dest, int &offset, const RmRecord &record) {
    memcpy(dest + offset, &record.size, sizeof(int));
    offset += sizeof(int);
    memcpy(dest + offset, record.data, record.size);
    offset += record.size;
}

RmRecord deserialize_record(const char *src, int &offset) {
    int size = *reinterpret_cast<const int *>(src + offset);
    offset += sizeof(int);
    RmRecord record(size, const_cast<char *>(src + offset));
    offset += size;
    return record;
}

void serialize_tail(char *dest, int &offset, const Rid &rid, const std::string &table_name) {
    memcpy(dest + offset, &rid, sizeof(Rid));
    offset += sizeof(Rid);
    uint32_t size = static_cast<uint32_t>(table_name.size());
    memcpy(dest + offset, &size, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    memcpy(dest + offset, table_name.data(), size);
}

void deserialize_tail(const char *src, int &offset, Rid &rid, std::string &table_name) {
    rid = *reinterpret_cast<const Rid *>(src + offset);
    offset += sizeof(Rid);
    uint32_t size = *reinterpret_cast<const uint32_t *>(src + offset);
    offset += sizeof(uint32_t);
    table_name.assign(src + offset, size);
}
}  // namespace

DeleteLogRecord::DeleteLogRecord() {
    log_type_ = LogType::DELETE;
    lsn_ = INVALID_LSN;
    log_tot_len_ = LOG_HEADER_SIZE;
    log_tid_ = INVALID_TXN_ID;
    prev_lsn_ = INVALID_LSN;
}

DeleteLogRecord::DeleteLogRecord(txn_id_t txn_id, const RmRecord &delete_value, const Rid &rid,
                                 std::string table_name)
    : DeleteLogRecord() {
    log_tid_ = txn_id;
    delete_value_ = delete_value;
    rid_ = rid;
    table_name_ = std::move(table_name);
    log_tot_len_ += sizeof(int) + delete_value_.size + sizeof(Rid) + sizeof(uint32_t) + table_name_.size();
}

void DeleteLogRecord::serialize(char *dest) const {
    LogRecord::serialize(dest);
    int offset = OFFSET_LOG_DATA;
    serialize_record(dest, offset, delete_value_);
    serialize_tail(dest, offset, rid_, table_name_);
}

void DeleteLogRecord::deserialize(const char *src) {
    LogRecord::deserialize(src);
    int offset = OFFSET_LOG_DATA;
    delete_value_ = deserialize_record(src, offset);
    deserialize_tail(src, offset, rid_, table_name_);
}

UpdateLogRecord::UpdateLogRecord() {
    log_type_ = LogType::UPDATE;
    lsn_ = INVALID_LSN;
    log_tot_len_ = LOG_HEADER_SIZE;
    log_tid_ = INVALID_TXN_ID;
    prev_lsn_ = INVALID_LSN;
}

UpdateLogRecord::UpdateLogRecord(txn_id_t txn_id, const RmRecord &old_value, const RmRecord &new_value,
                                 const Rid &rid, std::string table_name)
    : UpdateLogRecord() {
    log_tid_ = txn_id;
    old_value_ = old_value;
    new_value_ = new_value;
    rid_ = rid;
    table_name_ = std::move(table_name);
    log_tot_len_ += 2 * sizeof(int) + old_value_.size + new_value_.size + sizeof(Rid) +
                    sizeof(uint32_t) + table_name_.size();
}

void UpdateLogRecord::serialize(char *dest) const {
    LogRecord::serialize(dest);
    int offset = OFFSET_LOG_DATA;
    serialize_record(dest, offset, old_value_);
    serialize_record(dest, offset, new_value_);
    serialize_tail(dest, offset, rid_, table_name_);
}

void UpdateLogRecord::deserialize(const char *src) {
    LogRecord::deserialize(src);
    int offset = OFFSET_LOG_DATA;
    old_value_ = deserialize_record(src, offset);
    new_value_ = deserialize_record(src, offset);
    deserialize_tail(src, offset, rid_, table_name_);
}

void LogManager::initialize_lsn() {
    if (initialized_) return;
    initialized_ = true;
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    int offset = 0;
    lsn_t max_lsn = INVALID_LSN;
    char header[LOG_HEADER_SIZE];
    while (offset + LOG_HEADER_SIZE <= file_size) {
        if (disk_manager_->read_log(header, LOG_HEADER_SIZE, offset) != LOG_HEADER_SIZE) break;
        uint32_t length = *reinterpret_cast<uint32_t *>(header + OFFSET_LOG_TOT_LEN);
        lsn_t lsn = *reinterpret_cast<lsn_t *>(header + OFFSET_LSN);
        if (length < LOG_HEADER_SIZE || offset + static_cast<int>(length) > file_size) break;
        max_lsn = std::max(max_lsn, lsn);
        offset += length;
    }
    global_lsn_.store(max_lsn + 1);
    persist_lsn_ = max_lsn;
}

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    if (log_record == nullptr || log_record->log_tot_len_ > LOG_BUFFER_SIZE) {
        throw InternalError("invalid log record size");
    }
    std::scoped_lock lock{latch_};
    initialize_lsn();
    if (log_buffer_.is_full(log_record->log_tot_len_)) flush_log_to_disk_unlocked();
    log_record->lsn_ = global_lsn_++;
    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += log_record->log_tot_len_;
    flush_log_to_disk_unlocked();
    return log_record->lsn_;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    std::scoped_lock lock{latch_};
    initialize_lsn();
    flush_log_to_disk_unlocked();
}

void LogManager::flush_log_to_disk_unlocked() {
    if (log_buffer_.offset_ == 0) return;
    disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
    int offset = 0;
    while (offset < log_buffer_.offset_) {
        const char *record = log_buffer_.buffer_ + offset;
        persist_lsn_ = *reinterpret_cast<const lsn_t *>(record + OFFSET_LSN);
        offset += *reinterpret_cast<const uint32_t *>(record + OFFSET_LOG_TOT_LEN);
    }
    log_buffer_.offset_ = 0;
    memset(log_buffer_.buffer_, 0, sizeof(log_buffer_.buffer_));
}
