/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include <algorithm>
#include <memory>
#include <vector>

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t kJoinBufferBytes = 8 * 1024 * 1024;

    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;
    size_t left_block_capacity_;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    size_t left_block_idx_;
    std::unique_ptr<RmRecord> right_tuple_;
    std::unique_ptr<RmRecord> current_tuple_;

    std::unique_ptr<RmRecord> join_tuple(const RmRecord *left_rec, const RmRecord *right_rec) {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    std::vector<ColMeta>::const_iterator find_col(const std::vector<ColMeta> &cols, const TabCol &target) const {
        return std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
    }

    const ColMeta *get_value_meta(const std::vector<ColMeta> &cols, const TabCol &target) const {
        auto pos = find_col(cols, target);
        if (pos == cols.end()) {
            return nullptr;
        }
        return &(*pos);
    }

    bool eval_join_cond(const RmRecord *left_rec, const RmRecord *right_rec, const Condition &cond) const {
        const ColMeta *lhs_meta = get_value_meta(left_->cols(), cond.lhs_col);
        const char *lhs = nullptr;
        if (lhs_meta != nullptr) {
            lhs = left_rec->data + lhs_meta->offset;
        } else {
            lhs_meta = get_value_meta(right_->cols(), cond.lhs_col);
            if (lhs_meta == nullptr) {
                throw ColumnNotFoundError(cond.lhs_col.tab_name + '.' + cond.lhs_col.col_name);
            }
            lhs = right_rec->data + lhs_meta->offset;
        }

        const char *rhs = nullptr;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            rhs = cond.rhs_val.raw->data;
            rhs_type = cond.rhs_val.type;
        } else {
            const ColMeta *rhs_meta = get_value_meta(left_->cols(), cond.rhs_col);
            if (rhs_meta != nullptr) {
                rhs = left_rec->data + rhs_meta->offset;
            } else {
                rhs_meta = get_value_meta(right_->cols(), cond.rhs_col);
                if (rhs_meta == nullptr) {
                    throw ColumnNotFoundError(cond.rhs_col.tab_name + '.' + cond.rhs_col.col_name);
                }
                rhs = right_rec->data + rhs_meta->offset;
            }
            rhs_type = rhs_meta->type;
        }

        if (lhs_meta->type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_meta->type), coltype2str(rhs_type));
        }
        return compare(lhs, rhs, lhs_meta->type, lhs_meta->len, cond.op);
    }

    bool eval_join_conds(const RmRecord *left_rec, const RmRecord *right_rec) const {
        for (const auto &cond : fed_conds_) {
            if (!eval_join_cond(left_rec, right_rec, cond)) {
                return false;
            }
        }
        return true;
    }

    bool refill_left_block() {
        left_block_.clear();
        left_block_idx_ = 0;
        size_t bytes = 0;
        while (!left_->is_end() && left_block_.size() < left_block_capacity_) {
            auto rec = left_->Next();
            if (rec == nullptr) {
                break;
            }
            bytes += rec->size;
            left_block_.push_back(std::move(rec));
            left_->nextTuple();
            if (bytes >= kJoinBufferBytes) {
                break;
            }
        }
        return !left_block_.empty();
    }

    void begin_right_scan() {
        right_->beginTuple();
        right_tuple_.reset();
    }

    void advance() {
        current_tuple_ = nullptr;
        while (true) {
            if (left_block_.empty()) {
                isend = true;
                return;
            }

            while (!right_->is_end()) {
                if (right_tuple_ == nullptr) {
                    right_tuple_ = right_->Next();
                }

                while (left_block_idx_ < left_block_.size()) {
                    auto *left_rec = left_block_[left_block_idx_].get();
                    if (fed_conds_.empty() || eval_join_conds(left_rec, right_tuple_.get())) {
                        current_tuple_ = join_tuple(left_rec, right_tuple_.get());
                        ++left_block_idx_;
                        isend = false;
                        return;
                    }
                    ++left_block_idx_;
                }

                left_block_idx_ = 0;
                right_->nextTuple();
                right_tuple_.reset();
            }

            if (!refill_left_block()) {
                isend = true;
                return;
            }
            begin_right_scan();
            if (right_->is_end()) {
                isend = true;
                return;
            }
        }
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);

        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
        left_block_capacity_ = std::max<size_t>(1, kJoinBufferBytes / std::max<size_t>(1, left_->tupleLen()));
        left_block_idx_ = 0;
    }

    void beginTuple() override {
        left_->beginTuple();
        isend = false;
        right_tuple_.reset();
        if (!refill_left_block()) {
            isend = true;
            return;
        }
        begin_right_scan();
        if (right_->is_end()) {
            isend = true;
            return;
        }
        advance();
    }

    void nextTuple() override {
        if (isend) return;
        advance();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (current_tuple_ == nullptr) return nullptr;
        return std::make_unique<RmRecord>(current_tuple_->size, current_tuple_->data);
    }

    bool is_end() const override { return isend; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
