# madras/copy_to.py
#
# "COPY TO" in the sense you meant: write a pandas DataFrame (or CSV/Arrow
# source) OUT to a new .mdsi file, via madras::dv1::builder.
#
# Supports both numeric and text/blob columns -- see
# src/madras_builder_pybind.cpp for how each is fed into
# ColumnarTableBuilder::append_vector().

import numpy as np

from ._madras_builder_native import MadrasBuilder, ColumnSpec

# MST_* type chars, mirroring MadrasLogicalTypeToChar() in
# madras_duckdb_copyto.cpp. Confirm these against madras/dv1/common.hpp if
# results look wrong -- these are copied from the DuckDB extension's mapping,
# not independently re-derived.
MST_INT = 'i'
MST_BIGINT = 'I'
MST_DECV = '.'
MST_DATE = 'j'
MST_TIME = 'k'
MST_TIME_TZ = 'l'
MST_TIMESTAMP = 'm'
MST_TIMESTAMP_TZ = 'n'
MST_TIMESTAMP_MS = 'o'
MST_TIMESTAMP_NS = 'p'
MST_TIMESTAMP_SEC = 'q'
MST_TEXT = 't'
MST_BIN = '*'

_NUMPY_DTYPE_TO_MST = {
    "int8": MST_INT, "int16": MST_INT, "int32": MST_INT,
    "uint8": MST_INT, "uint16": MST_INT, "uint32": MST_INT,
    "int64": MST_BIGINT, "uint64": MST_BIGINT,
    "float32": MST_DECV, "float64": MST_DECV,
    "bool": MST_INT,
}

# Auto-encoding sentinel, matching the DuckDB extension's 'a' convention
# (resolved server-side there via ColumnLooksMultiWord/DetectNumericEncoding
# sampling; here we just pick a fixed reasonable default per type, since
# there's no bulk-sample auto-detection pass implemented on the Python side
# yet). Pass encodings explicitly via `encodings=` to copy_from() to override.
_DEFAULT_ENC_FOR_MST = {
    MST_INT: 'v', MST_BIGINT: 'v', MST_DECV: 'v',
    MST_DATE: 'v', MST_TIME: 'v', MST_TIME_TZ: 'v',
    MST_TIMESTAMP: 'v', MST_TIMESTAMP_TZ: 'v',
    MST_TIMESTAMP_MS: 'v', MST_TIMESTAMP_NS: 'v', MST_TIMESTAMP_SEC: 'v',
    MST_TEXT: 't',   # plain trie encoding by default; pass encodings={'col': 'W'} for word search
    MST_BIN: 'v',
}


def _infer_mst_type(dtype_str, is_bytes=False):
    if dtype_str == "object" or dtype_str.startswith("string"):
        return MST_BIN if is_bytes else MST_TEXT
    mst_type = _NUMPY_DTYPE_TO_MST.get(dtype_str)
    if mst_type is None:
        raise NotImplementedError(f"Unsupported dtype: {dtype_str}")
    return mst_type


def _infer_columns(df, pk_columns, encodings, word_index):
    pk_columns = pk_columns or []
    encodings = encodings or {}
    word_index = set(word_index or [])

    specs = []
    for name in df.columns:
        series = df[name]
        dtype_str = str(series.dtype)
        is_bytes = dtype_str == "object" and len(series) > 0 and isinstance(
            series.dropna().iloc[0] if len(series.dropna()) else None, (bytes, bytearray)
        )
        mst_type = _infer_mst_type(dtype_str, is_bytes)

        spec = ColumnSpec()
        spec.name = name
        spec.dt = mst_type
        if name in encodings:
            spec.enc = encodings[name]
        elif name in word_index:
            spec.enc = 'w'  # word/phrase-search encoding, matches DetectTextEncoding's 'w'
        elif name in pk_columns or mst_type == MST_TEXT:
            spec.enc = 't'  # trie-indexed by default for PK/text columns, enabling exact-match lookup
        else:
            spec.enc = _DEFAULT_ENC_FOR_MST.get(mst_type, 'v')
        spec.is_pk = name in pk_columns
        specs.append(spec)
    return specs


def copy_from(source, dest_path, table_name=None, pk_columns=None,
              encodings=None, word_index=None):
    """
    Write a pandas DataFrame to a new .mdsi file.

    source: a pandas.DataFrame, pyarrow.Table, or CSV file path.
    dest_path: output .mdsi file path.
    table_name: table name embedded in the file's column-name CSV header.
        Defaults to dest_path's filename stem.
    pk_columns: ordered list of column names forming the primary key --
        moved to the front of column order (PK-columns-first convention).
    encodings: optional dict {column_name: encoding_char} overriding the
        default encoding choice per column (e.g. {'name': 'W'} for
        word-search on a text column).
    word_index: convenience list of column names to encode with 'w'
        (word/phrase search) -- equivalent to encodings={name: 'w', ...}.
    """
    df = _coerce_to_dataframe(source)

    if table_name is None:
        import os
        table_name = os.path.splitext(os.path.basename(dest_path))[0]

    pk_columns = pk_columns or []
    if pk_columns:
        remaining = [c for c in df.columns if c not in pk_columns]
        df = df[list(pk_columns) + remaining]

    columns = _infer_columns(df, pk_columns, encodings, word_index)
    builder = MadrasBuilder(dest_path, table_name, columns)

    for idx, (col_name, spec) in enumerate(zip(df.columns, columns)):
        series = df[col_name]
        is_null = series.isna().to_numpy(dtype=bool)

        if spec.dt in (MST_TEXT, MST_BIN):
            # None for null rows; append_text_column reads these positions'
            # placeholder values only when marked valid, so nulls can be any
            # value here as long as the mask is correct.
            values = [None if pd_isna else v for v, pd_isna in zip(series.tolist(), is_null)]
            builder.append_text_column(idx, values)
        else:
            values = series.fillna(0).to_numpy()
            builder.append_numeric_column(idx, values, is_null)

    builder.finish()


def _coerce_to_dataframe(source):
    import pandas as pd

    if isinstance(source, pd.DataFrame):
        return source

    try:
        import pyarrow as pa
        if isinstance(source, pa.Table):
            return source.to_pandas()
    except ImportError:
        pass

    if isinstance(source, str):
        return pd.read_csv(source)

    raise TypeError(
        "copy_from() source must be a pandas.DataFrame, pyarrow.Table, or a CSV file path; "
        f"got {type(source)}"
    )
