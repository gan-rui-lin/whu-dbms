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

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "defs.h"
#include "errors.h"
#include "record/rm_defs.h"

inline bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

inline int days_in_month(int year, int month) {
    static const int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        return is_leap_year(year) ? 29 : 28;
    }
    return kDays[month];
}

inline bool parse_datetime_string(const std::string &value, int64_t *encoded) {
    if (value.size() != 19 || value[4] != '-' || value[7] != '-' || value[10] != ' ' || value[13] != ':' ||
        value[16] != ':') {
        return false;
    }
    auto parse_component = [&](int pos, int len) -> int {
        int result = 0;
        for (int i = 0; i < len; ++i) {
            char ch = value[pos + i];
            if (ch < '0' || ch > '9') {
                return -1;
            }
            result = result * 10 + (ch - '0');
        }
        return result;
    };
    int year = parse_component(0, 4);
    int month = parse_component(5, 2);
    int day = parse_component(8, 2);
    int hour = parse_component(11, 2);
    int minute = parse_component(14, 2);
    int second = parse_component(17, 2);
    if (year < 1000 || year > 9999 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    if (day > days_in_month(year, month)) {
        return false;
    }
    *encoded = static_cast<int64_t>(year) * 10000000000LL + static_cast<int64_t>(month) * 100000000LL +
               static_cast<int64_t>(day) * 1000000LL + static_cast<int64_t>(hour) * 10000LL +
               static_cast<int64_t>(minute) * 100LL + second;
    return true;
}

inline std::string format_datetime_value(int64_t encoded) {
    int second = static_cast<int>(encoded % 100);
    encoded /= 100;
    int minute = static_cast<int>(encoded % 100);
    encoded /= 100;
    int hour = static_cast<int>(encoded % 100);
    encoded /= 100;
    int day = static_cast<int>(encoded % 100);
    encoded /= 100;
    int month = static_cast<int>(encoded % 100);
    encoded /= 100;
    int year = static_cast<int>(encoded);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << year << "-" << std::setw(2) << month << "-" << std::setw(2) << day
        << " " << std::setw(2) << hour << ":" << std::setw(2) << minute << ":" << std::setw(2) << second;
    return oss.str();
}


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
        int64_t bigint_val;  // bigint or datetime encoded value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void set_bigint(int64_t bigint_val_) {
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }

    void set_datetime(int64_t datetime_val_) {
        type = TYPE_DATETIME;
        bigint_val = datetime_val_;
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_BIGINT || type == TYPE_DATETIME) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(raw->data) = bigint_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

inline void coerce_value_type(Value &value, ColType target_type, int len) {
    if (value.type == target_type) {
        if (target_type == TYPE_STRING && len < static_cast<int>(value.str_val.size())) {
            throw StringOverflowError();
        }
        return;
    }
    if (target_type == TYPE_INT) {
        if (value.type == TYPE_BIGINT) {
            if (value.bigint_val < std::numeric_limits<int>::min() ||
                value.bigint_val > std::numeric_limits<int>::max()) {
                throw ValueOutOfRangeError("INT", std::to_string(value.bigint_val));
            }
            value.set_int(static_cast<int>(value.bigint_val));
            return;
        }
    } else if (target_type == TYPE_FLOAT) {
        if (value.type == TYPE_INT) {
            value.set_float(static_cast<float>(value.int_val));
            return;
        }
        if (value.type == TYPE_BIGINT) {
            value.set_float(static_cast<float>(value.bigint_val));
            return;
        }
    } else if (target_type == TYPE_BIGINT) {
        if (value.type == TYPE_INT) {
            value.set_bigint(value.int_val);
            return;
        }
    } else if (target_type == TYPE_DATETIME) {
        if (value.type == TYPE_STRING) {
            int64_t encoded = 0;
            if (!parse_datetime_string(value.str_val, &encoded)) {
                throw InvalidDateTimeError(value.str_val);
            }
            value.set_datetime(encoded);
            return;
        }
    }
    throw IncompatibleTypeError(coltype2str(target_type), coltype2str(value.type));
}

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};
