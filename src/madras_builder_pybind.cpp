// madras_builder_pybind.cpp
//
// Write path ("COPY TO": Python -> .mdsi), built against the REAL
// madras::dv1::builder / ColumnarTableBuilder API confirmed from
// madras_duckdb_copyto.cpp and data_ingestion.hpp:
//
//   madras::dv1::builder(filename, names_csv, col_count,
//                         dt_codes_cstr, enc_codes_cstr, "0", 0, pk_col_count)
//   builder->get_col_table_bldr() -> ColumnarTableBuilder*
//   col_tbl_bldr->append_vector(col_idx, data_ptr, validity_bitmap_ptr, count)
//     -- for DB_INT32/DB_DATE:      data_ptr -> contiguous int32_t[count]
//     -- for DB_INT64/DB_TIME/etc:  data_ptr -> contiguous int64_t[count]
//     -- for DB_DOUBLE:              data_ptr -> contiguous double[count]
//     -- for DB_TEXT/DB_BLOB:        data_ptr -> contiguous db_string_t[count]
//        (per data_ingestion.hpp's column_storage::append_value, which casts
//        data to `const db_string_t *` and calls s[i].data()/s[i].length())
//   builder->insert_record(col_vals.data(), valids.data(), row, false)   [PK tables only]
//   builder->build_and_write_all()
//
// TEXT/BLOB columns: db_string_t's exact internal layout wasn't shown, but
// its constructor pattern `db_string_t(const char *data, length)` is used
// consistently elsewhere in data_ingestion.hpp, so this file constructs
// db_string_t instances via that public constructor rather than replicating
// its layout -- no guessing required. If db_string_t turns out to need a
// different constructor signature, that will show as a compile error here,
// not a silent data-corruption bug.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

#include "madras/dv1/common.hpp"
#include "madras/dv1/builder/madras_builder.hpp"

typedef int64_t idx_t;

namespace py = pybind11;
using namespace madras::dv1;

/* ---------------------------------------------------------
   Type/encoding char mapping (mirrors MadrasLogicalTypeToChar /
   the dt/enc conventions used in madras_duckdb_copyto.cpp)
--------------------------------------------------------- */

struct ColumnSpec {
    std::string name;
    char dt;         // MST_* type char (e.g. MST_INT, MST_BIGINT, MST_DECV, MST_TEXT, MST_DATE, ...)
    char enc;         // MSE_* / shorthand encoding char (e.g. 'v','p','t','u','w', or 'a' for auto)
    bool is_pk = false;
};

/* ---------------------------------------------------------
   Validity bitmap: DuckDB-style packed bits, 1 = valid (matches
   ValidityMask::GetData() layout, and matches append_vector's own bit-read
   convention: `(valid[i >> 6] >> (i & 63)) & 1`).
--------------------------------------------------------- */

static std::vector<uint64_t> BuildValidityBitmap(const py::array_t<bool> &is_null_mask) {
    size_t n = is_null_mask.size();
    size_t words = (n + 63) / 64;
    std::vector<uint64_t> bits(words, ~0ULL); // default: all valid
    auto m = is_null_mask.unchecked<1>();
    for (size_t i = 0; i < n; i++) {
        if (m(i)) { // true == is null
            bits[i / 64] &= ~(1ULL << (i % 64));
        }
    }
    return bits;
}

/* ---------------------------------------------------------
   MadrasBuilder
--------------------------------------------------------- */

class MadrasBuilder {
public:
    MadrasBuilder(const std::string &dest_path, const std::string &table_name,
                  const std::vector<ColumnSpec> &columns)
        : dest_path_(dest_path) {
        col_count_ = columns.size();
        uint16_t pk_count = 0;

        std::string names_csv = table_name;
        for (auto &c : columns) {
            names_csv += ",";
            names_csv += c.name;
            dt_codes_.push_back(c.dt);
            enc_codes_.push_back(c.enc);
            col_names_.push_back(c.name);
            if (c.is_pk) pk_count++;
        }
        dt_codes_.push_back('\0');
        enc_codes_.push_back('\0');
        pk_col_count_ = pk_count;

        bldr_ = std::unique_ptr<madras::dv1::builder>(
          new madras::dv1::builder(
            dest_path_.c_str(), names_csv.c_str(), (int) col_count_,
            dt_codes_.data(), enc_codes_.data(), "0", 0, pk_col_count_));
        bldr_->set_print_enabled(false);
    }

    // Appends one full column's worth of numeric data in one call. `values`
    // must be a contiguous numpy array of the matching physical type for
    // this column's dt code (int32 for MST_INT/MST_DATE; int64 for
    // MST_BIGINT and date/time/timestamp variants; float64 for
    // MST_DECV/MST_DEC0..9). `is_null` is a bool mask, True = NULL, same
    // length as `values`.
    void append_numeric_column(uint32_t col_idx, py::array values, py::array_t<bool> is_null) {
        if ((size_t) col_idx >= col_count_) {
            throw std::runtime_error("append_numeric_column: column index out of range");
        }
        char dt = dt_codes_[col_idx];
        auto validity = BuildValidityBitmap(is_null);
        auto *col_tbl_bldr = bldr_->get_col_table_bldr();
        uint64_t count = (uint64_t) values.shape(0);

        if (dt == MST_INT || dt == MST_DATE) {
            auto arr = values.cast<py::array_t<int32_t>>();
            col_tbl_bldr->append_vector((size_t) col_idx, (void *) arr.data(),
                                         (uint64_t *) validity.data(), count);
        } else if (dt == MST_BIGINT || (dt >= MST_TIME && dt <= MST_TIMESTAMP_SEC)) {
            auto arr = values.cast<py::array_t<int64_t>>();
            col_tbl_bldr->append_vector((size_t) col_idx, (void *) arr.data(),
                                         (uint64_t *) validity.data(), count);
        } else if (dt == MST_DECV || (dt >= MST_DEC0 && dt <= MST_DEC9)) {
            auto arr = values.cast<py::array_t<double>>();
            col_tbl_bldr->append_vector((size_t) col_idx, (void *) arr.data(),
                                         (uint64_t *) validity.data(), count);
        } else {
            throw std::runtime_error(
                "append_numeric_column called for a non-numeric dt code -- "
                "use append_text_column for TEXT/BIN columns");
        }
        row_count_ = count; // assumes all columns appended with equal length, in one shot
    }

    // Appends one full TEXT/BIN column. `values` is a list of str/bytes/None
    // (None = NULL), one entry per row. Builds a contiguous db_string_t[]
    // array via db_string_t's (const char*, length) constructor and calls
    // append_vector the same way append_row/append_vector expects for
    // DB_TEXT/DB_BLOB columns (see column_storage::append_value in
    // data_ingestion.hpp).
    void append_text_column(uint32_t col_idx, py::list values) {
        if ((size_t) col_idx >= col_count_) {
            throw std::runtime_error("append_text_column: column index out of range");
        }
        char dt = dt_codes_[col_idx];
        if (dt != MST_TEXT && dt != MST_BIN) {
            throw std::runtime_error(
                "append_text_column called for a non-text/blob dt code -- "
                "use append_numeric_column instead");
        }

        size_t n = values.size();
        // Owns the actual byte data for the lifetime of this call; db_string_t
        // is assumed to be a non-owning view (per its usage pattern elsewhere
        // in data_ingestion.hpp), so these std::strings must outlive the
        // append_vector call below, which is synchronous/blocking, so a
        // local vector is sufficient.
        std::vector<std::string> owned(n);
        std::vector<db_string_t> views;
        views.reserve(n);

        py::array_t<bool> is_null(n);
        auto null_buf = is_null.mutable_unchecked<1>();

        for (size_t i = 0; i < n; i++) {
            py::object item = values[i];
            if (item.is_none()) {
                null_buf(i) = true;
                owned[i] = std::string(); // placeholder; append_value skips reading it when invalid
            } else {
                null_buf(i) = false;
                if (py::isinstance<py::bytes>(item)) {
                    owned[i] = item.cast<std::string>();
                } else {
                    owned[i] = item.cast<std::string>(); // py::str also converts to std::string (UTF-8)
                }
            }
            views.emplace_back(owned[i].data(), (uint32_t) owned[i].size());
        }

        auto validity = BuildValidityBitmap(is_null);
        auto *col_tbl_bldr = bldr_->get_col_table_bldr();
        col_tbl_bldr->append_vector((size_t) col_idx, (void *) views.data(),
                                     (uint64_t *) validity.data(), (uint64_t) n);
        row_count_ = n;
    }

    // Builds primary-key index entries for all rows appended so far. Only
    // meaningful/needed when pk_col_count > 0 -- mirrors the DuckDB
    // extension's per-chunk insert_record loop, but done once over the full
    // appended row range instead of per-chunk.
    void index_pk_rows() {
        if (pk_col_count_ == 0) return;
        auto *col_tbl_bldr = bldr_->get_col_table_bldr();
        uintxx_t rec_count = col_tbl_bldr->get_record_count();
        std::vector<madras::dv1::col_value> col_vals(col_count_);
        std::vector<uint8_t> valids(col_count_);
        for (uintxx_t row = 0; row < rec_count; row++) {
            for (size_t c = 0; c < col_count_; c++) {
                madras::dv1::column_storage *col = col_tbl_bldr->get_data(c);
                col_vals[c] = col->get_col_value(row);
                valids[c] = (*col->get_null_bv())[row] ? 0 : 1;
            }
            bldr_->insert_record(col_vals.data(), valids.data(), row, false);
        }
    }

    void finish() {
        if (pk_col_count_ > 0) index_pk_rows();
        bldr_->build_and_write_all();
    }

private:
    std::string dest_path_;
    std::vector<char> dt_codes_;
    std::vector<char> enc_codes_;
    std::vector<std::string> col_names_;
    size_t col_count_ = 0;
    uint16_t pk_col_count_ = 0;
    uint64_t row_count_ = 0;
    std::unique_ptr<madras::dv1::builder> bldr_;
};

PYBIND11_MODULE(_madras_builder_native, m) {
    m.doc() = "Native write bindings for madras (.mdsi) files -- COPY TO support";

    py::class_<ColumnSpec>(m, "ColumnSpec")
        .def(py::init<>())
        .def_readwrite("name", &ColumnSpec::name)
        .def_readwrite("dt", &ColumnSpec::dt)
        .def_readwrite("enc", &ColumnSpec::enc)
        .def_readwrite("is_pk", &ColumnSpec::is_pk);

    py::class_<MadrasBuilder>(m, "MadrasBuilder")
        .def(py::init<const std::string &, const std::string &, const std::vector<ColumnSpec> &>(),
             py::arg("dest_path"), py::arg("table_name"), py::arg("columns"))
        .def("append_numeric_column", &MadrasBuilder::append_numeric_column,
             py::arg("col_idx"), py::arg("values"), py::arg("is_null"))
        .def("append_text_column", &MadrasBuilder::append_text_column,
             py::arg("col_idx"), py::arg("values"))
        .def("finish", &MadrasBuilder::finish);
}

