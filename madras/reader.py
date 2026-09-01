from ._madras_native import MadrasReader as _NativeReader

# Descriptions ported directly from MadrasTypeDesc()/MadrasEncDesc() in
# madras_duckdb_meta.cpp, so `describe()` below matches what
# `read_madras_meta()` shows in DuckDB. Keep these in sync if the extension's
# tables change.
_TYPE_DESC = {
    '*': "blob", 't': "text", 'i': "int", 'I': "bigint",
    '.': "double",
    '0': "decimal0", '1': "decimal1", '2': "decimal2", '3': "decimal3", '4': "decimal4",
    '5': "decimal5", '6': "decimal6", '7': "decimal7", '8': "decimal8", '9': "decimal9",
    'j': "date", 'k': "time", 'l': "time_tz",
    'm': "timestamp", 'n': "timestamp_tz", 'o': "timestamp_ms", 'p': "timestamp_ns", 'q': "timestamp_sec",
    'S': "sec",  # secondary index column marker
}

_ENC_DESC = {
    't': "trie", 'T': "trie_index",
    's': "trie_sint",  # NOTE: collides with 's'=store in the source comment; verify against your build
    'w': "words", 'W': "words_index",
    'u': "dict", 'd': "dict_delta",
    'v': "vint", 'V': "vint_delta",
    'p': "bitpack", 'P': "bitpack_delta",
}


def describe_type(dt_char):
    """Human-readable column type description, matching read_madras_meta()."""
    return _TYPE_DESC.get(dt_char, f"{dt_char}:unknown")


def describe_encoding(enc_char):
    """Human-readable column encoding description, matching read_madras_meta()."""
    return _ENC_DESC.get(enc_char, f"{enc_char}:unknown")


class MadrasReader:
    """Pythonic wrapper over the native madras reader."""

    def __init__(self, path, mmap=True):
        self._r = _NativeReader(path, mmap)
        self.metadata = self._r.metadata()
        self.columns = [c["name"] for c in self.metadata["columns"] if c["type"] != "S"]

    def rows(self, offset=0, count=-1, columns=None):
        """Row-oriented fetch -> list[dict]. Fine for small result sets."""
        return self._r.get_rows(offset, count, columns or [])

    def to_pandas(self, offset=0, count=-1, columns=None):
        import pandas as pd
        cols = self._r.get_columns(offset, count, columns or [])
        return pd.DataFrame(cols)

    def to_arrow(self, offset=0, count=-1, columns=None):
        import pyarrow as pa
        cols = self._r.get_columns(offset, count, columns or [])
        return pa.table(cols)

    def lookup(self, column, value, columns=None):
        """Index/word lookup -> pandas.DataFrame of matching rows."""
        import pandas as pd
        col_idx = self._col_index(column)
        row_ids = self._r.lookup_row_ids(col_idx, str(value))
        cols = self._r.get_columns_by_ids(row_ids, columns or [])
        return pd.DataFrame(cols)

    def _col_index(self, name_or_idx):
        if isinstance(name_or_idx, int):
            return name_or_idx
        for c in self.metadata["columns"]:
            if c["name"] == name_or_idx:
                return c["index"]
        raise KeyError(f"No such column: {name_or_idx}")

    def describe(self):
        """
        Returns a pandas.DataFrame of (column_name, column_type, column_encoding)
        with human-readable descriptions, equivalent to
        `SELECT * FROM read_madras_meta('file.mdsi')` in the DuckDB extension.
        """
        import pandas as pd
        rows = []
        for c in self.metadata["columns"]:
            rows.append({
                "column_name": c["name"],
                "column_type": describe_type(c["type"]),
                "column_encoding": describe_encoding(c["encoding"]),
            })
        return pd.DataFrame(rows)

    def batches(self, batch_size=100_000, columns=None):
        """Generator yielding pyarrow.RecordBatch chunks -- for streaming large
        sources without materializing the whole thing in memory at once."""
        import pyarrow as pa
        total = self.metadata["rows"]
        offset = 0
        while offset < total:
            n = min(batch_size, total - offset)
            cols = self._r.get_columns(offset, n, columns or [])
            yield pa.record_batch(cols)
            offset += n
