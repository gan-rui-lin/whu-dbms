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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
        context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
    }
    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid : rids_) {
            auto old_rec = fh_->get_record(rid, context_);
            RmRecord new_rec(old_rec->size, old_rec->data);
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                memcpy(new_rec.data + col->offset, set_clause.rhs.raw->data, col->len);
            }

            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                std::vector<char> new_key(index.col_tot_len);
                std::vector<char> old_key(index.col_tot_len);
                int offset = 0;
                for (auto &col : index.cols) {
                    memcpy(old_key.data() + offset, old_rec->data + col.offset, col.len);
                    memcpy(new_key.data() + offset, new_rec.data + col.offset, col.len);
                    offset += col.len;
                }
                if (memcmp(old_key.data(), new_key.data(), index.col_tot_len) != 0) {
                    std::vector<Rid> result;
                    if (ih->get_value(new_key.data(), &result, context_->txn_)) {
                        std::vector<std::string> col_names;
                        for (auto &col : index.cols) col_names.push_back(col.name);
                        throw IndexExistsError(tab_name_, col_names);
                    }
                }
            }

            if (context_->log_mgr_ != nullptr) {
                UpdateLogRecord log(context_->txn_->get_transaction_id(), *old_rec, new_rec, rid, tab_name_);
                log.prev_lsn_ = context_->txn_->get_prev_lsn();
                context_->txn_->set_prev_lsn(context_->log_mgr_->add_log_to_buffer(&log));
            }
            context_->txn_->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *old_rec));

            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                std::vector<char> old_key(index.col_tot_len);
                std::vector<char> new_key(index.col_tot_len);
                int offset = 0;
                for (auto &col : index.cols) {
                    memcpy(old_key.data() + offset, old_rec->data + col.offset, col.len);
                    memcpy(new_key.data() + offset, new_rec.data + col.offset, col.len);
                    offset += col.len;
                }
                if (memcmp(old_key.data(), new_key.data(), index.col_tot_len) != 0) {
                    ih->delete_entry(old_key.data(), context_->txn_);
                    ih->insert_entry(new_key.data(), rid, context_->txn_);
                }
            }
            fh_->update_record(rid, new_rec.data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
