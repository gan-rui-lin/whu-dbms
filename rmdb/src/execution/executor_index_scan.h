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

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "index/ix_scan.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;
    IxIndexHandle *ih_;

    void fill_min(char *key, int offset, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::min();
            memcpy(key + offset, &v, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float v = -std::numeric_limits<float>::max();
            memcpy(key + offset, &v, sizeof(float));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t v = std::numeric_limits<int64_t>::min();
            memcpy(key + offset, &v, sizeof(int64_t));
        } else {
            memset(key + offset, 0, col.len);
        }
    }

    void fill_max(char *key, int offset, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::max();
            memcpy(key + offset, &v, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float v = std::numeric_limits<float>::max();
            memcpy(key + offset, &v, sizeof(float));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t v = std::numeric_limits<int64_t>::max();
            memcpy(key + offset, &v, sizeof(int64_t));
        } else {
            memset(key + offset, 0xff, col.len);
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        ih_ = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        std::vector<char> lower_key(index_meta_.col_tot_len);
        std::vector<char> upper_key(index_meta_.col_tot_len);
        int offset = 0;
        bool stop_prefix = false;
        for (auto &idx_col : index_meta_.cols) {
            fill_min(lower_key.data(), offset, idx_col);
            fill_max(upper_key.data(), offset, idx_col);
            if (!stop_prefix) {
                const Condition *eq = nullptr;
                const Condition *lower = nullptr;
                const Condition *upper = nullptr;
                for (auto &cond : fed_conds_) {
                    if (!cond.is_rhs_val || cond.lhs_col.col_name != idx_col.name) continue;
                    if (cond.op == OP_EQ) eq = &cond;
                    else if (cond.op == OP_GT || cond.op == OP_GE) lower = &cond;
                    else if (cond.op == OP_LT || cond.op == OP_LE) upper = &cond;
                }
                if (eq != nullptr) {
                    memcpy(lower_key.data() + offset, eq->rhs_val.raw->data, idx_col.len);
                    memcpy(upper_key.data() + offset, eq->rhs_val.raw->data, idx_col.len);
                } else {
                    if (lower != nullptr) memcpy(lower_key.data() + offset, lower->rhs_val.raw->data, idx_col.len);
                    if (upper != nullptr) memcpy(upper_key.data() + offset, upper->rhs_val.raw->data, idx_col.len);
                    stop_prefix = true;
                }
            }
            offset += idx_col.len;
        }
        Iid lower = ih_->lower_bound(lower_key.data());
        Iid upper = ih_->upper_bound(upper_key.data());
        scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        while (!scan_->is_end()) {
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (eval_conds(cols_, rec.get(), fed_conds_)) {
                rid_ = scan_->rid();
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        if (scan_ == nullptr || scan_->is_end()) return;
        scan_->next();
        while (!scan_->is_end()) {
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (eval_conds(cols_, rec.get(), fed_conds_)) {
                rid_ = scan_->rid();
                return;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return rid_; }
};
