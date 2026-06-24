/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include "execution_defs.h"
#include "executor_abstract.h"

class LimitExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    size_t len_;
    int limit_;
    int count_;

   public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> prev, int limit) {
        prev_ = std::move(prev);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        limit_ = limit;
        count_ = 0;
    }

    void beginTuple() override {
        count_ = 0;
        prev_->beginTuple();
    }

    void nextTuple() override {
        if (!is_end()) {
            ++count_;
            prev_->nextTuple();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        return prev_->Next();
    }

    bool is_end() const override {
        return limit_ >= 0 && count_ >= limit_ ? true : prev_->is_end();
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return prev_->rid(); }
};
