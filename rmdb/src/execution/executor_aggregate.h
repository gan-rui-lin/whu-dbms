/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "optimizer/plan.h"
#include "system/sm.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<AggCall> agg_calls_;
    std::vector<ColMeta> cols_;
    size_t len_;
    bool produced_;
    bool end_;
    std::unique_ptr<RmRecord> result_;

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<AggCall> agg_calls) {
        prev_ = std::move(prev);
        agg_calls_ = std::move(agg_calls);
        len_ = 0;
        for (auto &agg : agg_calls_) {
            ColMeta col;
            col.tab_name = "";
            col.name = agg.alias;
            col.offset = static_cast<int>(len_);
            if (agg.func_type == ast::AGG_COUNT) {
                col.type = TYPE_INT;
                col.len = sizeof(int);
            } else {
                auto src = prev_->get_col_offset(agg.col);
                col.type = src.type;
                col.len = src.len;
            }
            col.index = false;
            len_ += col.len;
            cols_.push_back(col);
        }
        produced_ = false;
        end_ = false;
    }

    void beginTuple() override {
        result_ = std::make_unique<RmRecord>(len_);
        std::vector<bool> initialized(agg_calls_.size(), false);
        std::vector<double> sums(agg_calls_.size(), 0);
        std::vector<int> counts(agg_calls_.size(), 0);

        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            for (size_t i = 0; i < agg_calls_.size(); ++i) {
                auto &agg = agg_calls_[i];
                char *dest = result_->data + cols_[i].offset;
                if (agg.func_type == ast::AGG_COUNT) {
                    counts[i]++;
                    continue;
                }

                ColMeta src_col = prev_->get_col_offset(agg.col);
                char *src = rec->data + src_col.offset;
                if (agg.func_type == ast::AGG_SUM) {
                    if (src_col.type == TYPE_INT) sums[i] += *reinterpret_cast<int *>(src);
                    else if (src_col.type == TYPE_FLOAT) sums[i] += *reinterpret_cast<float *>(src);
                    else throw IncompatibleTypeError("INT/FLOAT", "STRING");
                    initialized[i] = true;
                    continue;
                }

                if (!initialized[i]) {
                    memcpy(dest, src, src_col.len);
                    initialized[i] = true;
                    continue;
                }
                int cmp = 0;
                if (src_col.type == TYPE_INT) {
                    int l = *reinterpret_cast<int *>(src);
                    int r = *reinterpret_cast<int *>(dest);
                    cmp = (l < r) ? -1 : (l > r ? 1 : 0);
                } else if (src_col.type == TYPE_FLOAT) {
                    float l = *reinterpret_cast<float *>(src);
                    float r = *reinterpret_cast<float *>(dest);
                    cmp = (l < r) ? -1 : (l > r ? 1 : 0);
                } else {
                    cmp = memcmp(src, dest, src_col.len);
                }
                if ((agg.func_type == ast::AGG_MAX && cmp > 0) ||
                    (agg.func_type == ast::AGG_MIN && cmp < 0)) {
                    memcpy(dest, src, src_col.len);
                }
            }
        }

        for (size_t i = 0; i < agg_calls_.size(); ++i) {
            char *dest = result_->data + cols_[i].offset;
            if (agg_calls_[i].func_type == ast::AGG_COUNT) {
                memcpy(dest, &counts[i], sizeof(int));
            } else if (agg_calls_[i].func_type == ast::AGG_SUM) {
                if (cols_[i].type == TYPE_INT) {
                    int v = static_cast<int>(sums[i]);
                    memcpy(dest, &v, sizeof(int));
                } else {
                    float v = static_cast<float>(sums[i]);
                    memcpy(dest, &v, sizeof(float));
                }
            }
        }
        produced_ = false;
        end_ = false;
    }

    void nextTuple() override {
        produced_ = true;
        end_ = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        return std::make_unique<RmRecord>(result_->size, result_->data);
    }

    bool is_end() const override { return end_ || produced_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
