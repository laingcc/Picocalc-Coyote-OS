#ifndef COYOTE_AI_SEARCH_INDEX_H
#define COYOTE_AI_SEARCH_INDEX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Keyword (BM25) search over a card-resident CYX1 index built on a host by
 * tools/search_index/build_index.py.  docs/design/on-device-search.md pins the
 * file format, the tokenizer and the scoring; this reader and the builder
 * must agree on all three.
 *
 * Nothing allocates.  search_index_t holds every buffer it needs and is small
 * enough to keep as a static.  Opening checks the header and the CRC (streamed
 * in SEARCH_IO_CHUNK pieces); a query then reads only the term table entries
 * it binary-searches, the postings of the terms it contains and the document
 * entries of the chunks those postings name.  Every offset and length taken
 * from the file is checked against the section it points into before it is
 * used.  Not reentrant: one query or text read at a time per index.
 */

#define SEARCH_MAX_HITS 8

#define SEARCH_INDEX_MAGIC "CYX1"
#define SEARCH_INDEX_VERSION 1u
#define SEARCH_INDEX_HEADER_SIZE 160u
#define SEARCH_INDEX_SECTION_COUNT 6u

enum {
    SEARCH_SECTION_TERMS = 0,
    SEARCH_SECTION_POSTINGS,
    SEARCH_SECTION_DOCS,
    SEARCH_SECTION_TEXT,
    SEARCH_SECTION_VECTORS,
    SEARCH_SECTION_RESERVED
};

#define SEARCH_TERM_ENTRY_SIZE 12u
#define SEARCH_POSTING_SIZE 6u
#define SEARCH_DOC_ENTRY_SIZE 12u

/* Tokens are 2 to 32 bytes of [a-z0-9_]. */
#define SEARCH_TOKEN_MIN 2u
#define SEARCH_TOKEN_MAX 32u
/* Distinct non-stopword terms a query may hold. */
#define SEARCH_MAX_QUERY_TERMS 16u
/* Largest single read from the card.  A query shares this buffer between the
 * postings lists it is merging. */
#define SEARCH_IO_CHUNK 4096u
/* Document entries kept from one read while a query walks the chunks. */
#define SEARCH_DOC_CACHE_ENTRIES 128u

/*
 * The stopwords, sorted bytewise for binary search.  The builder's STOPWORDS
 * must hold exactly these; its tests compare the two lists.
 */
#define SEARCH_STOPWORDS                                                                         \
    "about", "above", "after", "again", "against", "all", "am", "an", "and", "any", "are", "as", \
    "at", "be", "because", "been", "before", "being", "below", "between", "both", "but", "by",   \
    "can", "did", "do", "does", "doing", "down", "during", "each", "few", "for", "from",          \
    "further", "had", "has", "have", "having", "he", "her", "here", "hers", "herself", "him",     \
    "himself", "his", "how", "if", "in", "into", "is", "it", "its", "itself", "just", "me",       \
    "more", "most", "my", "myself", "no", "nor", "not", "now", "of", "off", "on", "once", "only", \
    "or", "other", "our", "ours", "ourselves", "out", "over", "own", "same", "she", "should",     \
    "so", "some", "such", "than", "that", "the", "their", "theirs", "them", "themselves", "then", \
    "there", "these", "they", "this", "those", "through", "to", "too", "under", "until", "up",    \
    "very", "was", "we", "were", "what", "when", "where", "which", "while", "who", "whom", "why",  \
    "will", "with", "you", "your", "yours", "yourself", "yourselves"

typedef enum {
    SEARCH_OK = 0,
    SEARCH_ERR_ARGUMENT,
    SEARCH_ERR_IO,
    SEARCH_ERR_FORMAT,
    SEARCH_ERR_TOO_LARGE,
} search_status_t;

typedef struct {
    uint32_t chunk_id;
    int32_t score_q16; /* BM25 score x 65536, for ordering and display */
} search_hit_t;

typedef struct {
    uint32_t off; /* absolute, from the start of the file */
    uint32_t len;
} search_section_t;

/* One query term's postings list, read forward through its slice of io. */
typedef struct {
    uint32_t postings_off; /* relative to the postings section */
    uint16_t df;
    uint16_t next;     /* postings consumed so far */
    uint16_t buffered; /* postings held in the slice */
    uint16_t cursor;   /* next posting to hand out from the slice */
    uint32_t chunk_id; /* head posting, valid while next <= df */
    uint16_t tf;
    uint16_t has_head;
    uint32_t last_chunk_id;
    double idf;
} search_cursor_t;

typedef struct search_index {
    FILE *file;
    uint32_t file_size;
    uint16_t flags;
    uint16_t dim;
    uint16_t dtype;
    uint32_t chunk_count;
    uint32_t term_count;
    uint32_t avgdl_q8;
    search_section_t sections[SEARCH_INDEX_SECTION_COUNT];
    /* Bytes read from the file since open, header and CRC pass included. */
    uint32_t bytes_read;
    /* The first chunk id held in doc_cache and how many entries it holds. */
    uint32_t doc_cache_first;
    uint32_t doc_cache_count;
    search_cursor_t cursors[SEARCH_MAX_QUERY_TERMS];
    uint8_t doc_cache[SEARCH_DOC_CACHE_ENTRIES * SEARCH_DOC_ENTRY_SIZE];
    uint8_t io[SEARCH_IO_CHUNK];
} search_index_t; /* fixed-size, no heap */

/*
 * Open path and check its header, section layout and CRC.  SEARCH_ERR_IO when
 * the file cannot be opened or read, SEARCH_ERR_FORMAT when it is not a valid
 * CYX1 index.  On failure the index is left closed.
 */
search_status_t search_index_open(search_index_t *index, const char *path);
void search_index_close(search_index_t *index);
/*
 * Ranks the chunks matching query, best first, ties by lower chunk id.  At
 * most min(max_hits, SEARCH_MAX_HITS) hits are written.  Returns SEARCH_OK with
 * *hit_count == 0 when nothing matches; an empty or all-stopword query is not
 * an error.  A query with more than SEARCH_MAX_QUERY_TERMS distinct terms is
 * SEARCH_ERR_TOO_LARGE.
 */
search_status_t search_index_query(search_index_t *index, const char *query,
                                   search_hit_t *hits, size_t max_hits, size_t *hit_count);
/*
 * Copies a chunk's title (up to the first '\n') or body into buf as
 * NUL-terminated UTF-8.  Text that does not fit is cut at a character boundary
 * and SEARCH_ERR_TOO_LARGE returned with what fits.  A chunk_id past the last
 * chunk is SEARCH_ERR_ARGUMENT.
 */
search_status_t search_index_read_title(search_index_t *index, uint32_t chunk_id,
                                        char *buf, size_t capacity);
search_status_t search_index_read_body(search_index_t *index, uint32_t chunk_id,
                                       char *buf, size_t capacity);

/* The tokenizer's pieces, exposed for tests. */
uint32_t search_index_hash(const char *bytes, size_t length);
int search_index_is_stopword(const char *token, size_t length);

#endif
