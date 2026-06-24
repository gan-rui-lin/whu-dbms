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
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) const {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }

    bool compare(const char *lhs, const char *rhs, ColType type, int len, CompOp op) const {
        int cmp = 0;
        if (type == TYPE_INT) {
            int l = *reinterpret_cast<const int *>(lhs);
            int r = *reinterpret_cast<const int *>(rhs);
            cmp = (l < r) ? -1 : (l > r ? 1 : 0);
        } else if (type == TYPE_FLOAT) {
            float l = *reinterpret_cast<const float *>(lhs);
            float r = *reinterpret_cast<const float *>(rhs);
            cmp = (l < r) ? -1 : (l > r ? 1 : 0);
        } else {
            cmp = memcmp(lhs, rhs, len);
        }
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
        }
        return false;
    }

    bool eval_cond(const std::vector<ColMeta> &rec_cols, const RmRecord *rec, const Condition &cond) const {
        auto lhs_col = get_col(rec_cols, cond.lhs_col);
        const char *lhs = rec->data + lhs_col->offset;
        const char *rhs = nullptr;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            rhs = cond.rhs_val.raw->data;
            rhs_type = cond.rhs_val.type;
        } else {
            auto rhs_col = get_col(rec_cols, cond.rhs_col);
            rhs = rec->data + rhs_col->offset;
            rhs_type = rhs_col->type;
        }
        if (lhs_col->type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_type));
        }
        return compare(lhs, rhs, lhs_col->type, lhs_col->len, cond.op);
    }

    bool eval_conds(const std::vector<ColMeta> &rec_cols, const RmRecord *rec,
                    const std::vector<Condition> &conds) const {
        for (auto &cond : conds) {
            if (!eval_cond(rec_cols, rec, cond)) return false;
        }
        return true;
    }
};
