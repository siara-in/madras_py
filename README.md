# madras (Python bindings)

Python bindings for reading (and writing) `.mdsi` madras data source files,
built with pybind11 directly on top of `static_trie_map` (read) and
`madras_builder` (write).

## Layout

```
madras_py/
├── src/
│   ├── madras_key_convert.hpp       # shared value<->key conversion (also used by madras_cli)
│   ├── madras_pybind.cpp            # read bindings -> _madras_native
│   └── madras_builder_pybind.cpp    # write bindings -> _madras_builder_native (COPY TO)
├── madras/
│   ├── __init__.py
│   ├── reader.py                    # MadrasReader: pandas/arrow/lookup convenience wrapper
│   └── copy_to.py                   # copy_from(): pandas/arrow/CSV -> .mdsi
├── setup.py
└── README.md
```

## Build

Requires `pybind11` and a C++11 compiler. Expects the `madras/dv1` headers
(the same ones used by `madras_cli`/the DuckDB extension) at `../include`
relative to this directory -- adjust `include_dirs` in `setup.py` if your
layout differs.

```bash
pip install pybind11 numpy
pip install -e .
```

## Read usage

```python
from madras import MadrasReader

r = MadrasReader("babynames.db.mdsi")
r.metadata            # dict: rows, pk_columns, columns[]
r.to_pandas()          # full table -> pandas.DataFrame
r.to_arrow()            # full table -> pyarrow.Table
r.lookup("name", "John")  # index/word lookup -> pandas.DataFrame of matches

for batch in r.batches(batch_size=200_000):
    ...  # pyarrow.RecordBatch, for streaming large sources
```

## PySpark

There's no direct in-process JVM<->pybind11 bridge, so the practical pattern
today is export-then-read:

```python
r = MadrasReader("data.mdsi")
r.to_arrow().to_pandas()  # or write parquet via pyarrow directly, then:
qdf = spark.read.parquet("data.parquet")
```

A live Spark DataSource (mapInArrow / Data Source API v2) reading directly
off `r.batches()` per partition is possible but not included here -- ask if
you want that built out.

## Write usage ("COPY TO": Python -> `.mdsi`)

```python
from madras import copy_from

copy_from(df, "out.mdsi", pk_columns=["state", "name"], word_index=["bio"])
```

**Status: unverified against the real `madras_builder.hpp` API.** The write
path (`src/madras_builder_pybind.cpp`) was written against an *assumed*
builder API (`add_column`, `begin_row`/`set_val`/`end_row`, `build`,
`write_to_file`, etc.) inferred from how the reader side is shaped, since the
actual method signatures in `madras_builder.hpp` weren't available when this
was written. Every assumed call is marked `// ASSUMED API` in that file --
grep for it and correct method names/argument order/type-code constants
against your actual header before relying on `copy_from()`. The read path
(`madras_pybind.cpp`) is written directly against `static_trie_map` calls
already exercised and tested via `madras_cli`, so it should need little to no
correction.

If writing `.mdsi` from Python turns out not to be needed (e.g. everyone
downstream is fine going CSV/Parquet -> existing madras builder tooling
directly), the write extension can simply be dropped from `setup.py`'s
`ext_modules` list -- the read path is fully independent of it.
