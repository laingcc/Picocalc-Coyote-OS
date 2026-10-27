# On-device search index

A card-resident index for the PicoCalc. Round 1 is keyword search (BM25) that needs no
network, no model and no host. Round 2 adds embeddings: the host builds vectors, the device
fetches a query vector from a swappable OpenAI-compatible embedding endpoint and scores them
locally. This document pins the file format so the two rounds, and the host-side builder, stay
compatible.

## Why the split

The RP2350 cannot embed a query — a small transformer costs tens of seconds per query on an
M33. What it can do is arithmetic: 10,000 × 256-dim int8 dot products is ~2.5M MACs, tens of
milliseconds. So the expensive work happens on a host (`wang`, `nomic-embed-text` via Ollama,
768 or 256 dims), and the device only scores vectors that were built earlier and copied to the
card.

BM25 is included because "search that stops working when the server does" is a trap. It also
covers exact-term queries better than vectors do, so both are useful even when the network is
up.

## Non-goals for round 1

- No UI changes, no settings rows, no menu entries. Nothing in `UI/` may be touched.
- No network calls. The embedding client is round 2 and lives behind its own module.
- No new dependencies in either language (Python: standard library only; firmware: none).
- No changes to `ai_config`, providers, or credentials.

## Index format `CYX1` (little-endian)

Fixed header, 160 bytes:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic, ASCII `CYX1` (`0x31 0x59 0x58 0x43` in file order) |
| 4 | 2 | version = 1 |
| 6 | 2 | flags: bit0 = vectors normalized, bit1 = vectors are int8 |
| 8 | 2 | dim (0 when there are no vectors) |
| 10 | 2 | dtype: 0 = none, 1 = int8, 2 = f32 |
| 12 | 4 | chunk_count |
| 16 | 4 | term_count |
| 20 | 4 | avgdl_q8 — average document length in tokens × 256, rounded |
| 24 | 4 | crc32 of every byte after this header field (offset 28 to EOF) |
| 28 | 4 | reserved, 0 |
| 32 | 64 | model_id, NUL padded (empty in round 1) |
| 96 | 6 × 8 | section offset + length pairs: `u32 off`, `u32 len` |
| 144 | 16 | reserved, 0 |

Section order is fixed: 0 terms, 1 postings, 2 docs, 3 text, 4 vectors, 5 reserved. Offsets are
absolute from the start of the file; sections are contiguous in that order and 4-byte aligned.
An empty section has `len = 0` and `off` set to where the next section starts.

**Terms** (section 0), `term_count` × 12 bytes, sorted ascending by `hash`:

| size | field |
|---|---|
| 4 | `u32 hash` — FNV-1a 32-bit of the token bytes |
| 4 | `u32 postings_off` — byte offset relative to the start of the postings section |
| 2 | `u16 df` — how many chunks contain the term |
| 2 | reserved, 0 |

**Postings** (section 1): for each term in the same order, `df` × 6 bytes: `u32 chunk_id`,
`u16 tf`. No padding between terms.

**Docs** (section 2), `chunk_count` × 12 bytes: `u32 text_off` (relative to the text section),
`u32 text_len` (bytes, excluding the terminating NUL), `u16 dl` (tokens in this chunk),
`u16 flags` (0).

**Text** (section 3): each chunk as `title\nbody` followed by a NUL. `text_len` counts the
bytes before the NUL.

**Vectors** (section 4), round 2 only: `chunk_count` × `dim` bytes, int8, one vector per chunk
in chunk order, each L2-normalized before quantization so a dot product is a cosine. Empty in
round 1 (`dim = 0`, `len = 0`).

The reader must reject a file whose magic, version, section bounds, or CRC do not match, and
must never read past a section's declared length.

## Tokenization and BM25

Both the Python builder and the C reader must implement exactly this, or the index will not
match:

1. Bytes ≥ 0x80 are skipped (ASCII only for now). A token is a maximal run of `[a-z0-9_]` after
   lowercasing `A-Z`.
2. Keep tokens of length 2–32. Longer runs are split at 32.
3. Drop tokens in the stopword list (identical list in both implementations, defined in the
   Python module and mirrored in the C header as a sorted array).
4. `hash = FNV-1a 32`: `h = 2166136261; for each byte: h ^= b; h *= 16777619`.
5. Chunking: the builder splits a file at blank lines into paragraphs, then packs consecutive
   paragraphs into chunks of at most 600 characters, never splitting a paragraph unless it
   alone exceeds the limit. Each chunk's `title` is the file's first `#` heading (or the file
   name) and its `body` is the packed text.

Score for one query term in one chunk:

```
idf = ln(1 + (N - df + 0.5) / (df + 0.5))
score += idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * dl / avgdl))
k1 = 1.2, b = 0.75, N = chunk_count, avgdl = avgdl_q8 / 256
```

Chunks scoring 0 are not returned. Ties break by lower `chunk_id`, so results are deterministic.
Duplicate query terms count once.

## Device API (`ai/search_index.h`)

```c
#define SEARCH_MAX_HITS 8

typedef enum {
    SEARCH_OK = 0,
    SEARCH_ERR_ARGUMENT,
    SEARCH_ERR_IO,
    SEARCH_ERR_FORMAT,
    SEARCH_ERR_TOO_LARGE,
} search_status_t;

typedef struct {
    uint32_t chunk_id;
    int32_t score_q16;   /* BM25 score x 65536, for ordering and display */
} search_hit_t;

typedef struct search_index search_index_t;  /* fixed-size, no heap */

search_status_t search_index_open(search_index_t *index, const char *path);
void search_index_close(search_index_t *index);
/* Ranks the chunks matching query. Returns SEARCH_OK with *hit_count == 0 when nothing
 * matches; an empty or all-stopword query is not an error. */
search_status_t search_index_query(search_index_t *index, const char *query,
                                   search_hit_t *hits, size_t max_hits, size_t *hit_count);
/* Copies a chunk's title (up to the first '\n') or body into buf as NUL-terminated UTF-8. */
search_status_t search_index_read_title(search_index_t *index, uint32_t chunk_id,
                                        char *buf, size_t capacity);
search_status_t search_index_read_body(search_index_t *index, uint32_t chunk_id,
                                       char *buf, size_t capacity);
```

Rules:

- No `malloc`. The struct holds its own buffers; keep it under 16 KB so a caller can hold one
  as a static. Read from the file in 4 KB chunks through the existing `pico-vfs`/FatFs
  `fs_*`/`f_*` API used by `storage/file_store.c`, not by loading whole sections.
- Opening reads and validates the header only. Section data is streamed per query.
- A query reads only the postings of the terms it contains (binary search over the sorted term
  table), never the whole index.
- The module must build and pass host tests without the Pico SDK or network. Use the same
  `fs_*` abstraction the other host-testable modules use; if the existing code is FatFs-only,
  put the file access behind two small functions in `search_index.c` and keep the rest pure so
  tests can drive scoring directly.

## Host builder (`tools/search_index/build_index.py`)

```
python3 tools/search_index/build_index.py --corpus <dir> --out <file> [--limit N]
python3 tools/search_index/build_index.py --index <file> --query "text" [--top N]
```

- Walks `*.md` and `*.txt` recursively, in sorted path order.
- Builds BM25 sections and writes a valid `CYX1` file. `model_id` is empty, `dim` is 0, and the
  vectors section is empty in round 1.
- `--query` reads an existing index, tokenizes the same way, scores, and prints
  `chunk_id<TAB>score<TAB>title` per result — this is the reference implementation used to
  check the C ranking.
- Standard library only.

## Tests

C, in `tests/test_search_index.c`, registered in `tests/CMakeLists.txt` and called from
`test_ai_config()`'s runner (`tests/test_main.c`):

- The test writes its own fixture file (literal bytes assembled in the test) so it does not
  depend on Python. Cover: a query matching several chunks ranks by BM25, not by tf alone; a
  stopword-only query returns zero hits with `SEARCH_OK`; a truncated file, bad magic, bad
  version, out-of-bounds section and bad CRC each return `SEARCH_ERR_FORMAT`; `NULL` arguments
  return `SEARCH_ERR_ARGUMENT`; `max_hits` smaller than the number of matches truncates to the
  best ones.
- Ranking ties break by chunk id.

Python, in `tools/search_index/test_build_index.py` (stdlib `unittest`):
- Tokenizer: case folding, stopwords, length bounds, `_` and digits kept.
- Header fields, section contiguity, term sorting, postings counts, CRC.
- A 4-document corpus where the expected ranking is obvious.

## Acceptance

1. `cmake -S tests -B build-tests && cmake --build build-tests && ./build-tests/coyote_ai_tests`
   prints `N checks, 0 failures`.
2. `python3 -m unittest discover -s tools/search_index -p 'test_*.py'` passes.
3. `python3 tools/search_index/build_index.py --corpus <fixture dir> --out /tmp/a.idx` then
   `--index /tmp/a.idx --query "..."` returns the expected order.
4. The firmware still cross-builds for `pico2_w` with no new warnings.
