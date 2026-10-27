#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ai/search_index.h"
#include "test_util.h"

#define FIXTURE_FILE "search_index_test.idx"

/*
 * The fixture is assembled here, byte by byte, so these tests do not need
 * Python.  Its chunks, document lengths and postings are those
 * tools/search_index/build_index.py produces for the same texts, and the
 * expected scores below are what its --query prints for them (x 65536), so the
 * two implementations are checked against each other.
 */
typedef struct {
    const char *text; /* "title\nbody" */
    uint16_t dl;
} fixture_chunk_t;

typedef struct {
    const char *term;
    uint16_t postings[3][2]; /* {chunk_id, tf}; tf 0 ends the list */
} fixture_term_t;

static const fixture_chunk_t chunks[] = {
    {"Notes\nThe keyboard reads keys over i2c and the display draws text over spi; the radio is mentioned "
     "here, and the radio again, among many other words about matrix scanning debounce timing layers "
     "modifiers repeat rates and power",
     24u},
    {"Wireless\nradio antenna tuning", 4u},
    {"Battery\nbattery charge gauge battery cells", 6u},
    {"Battery\nbattery charge gauge battery cells", 6u},
    {"Sound \xc3\xa9\npwm audio speaker tones", 5u},
    {"Radio antenna\nantenna placement near the radio shield", 7u},
};
#define CHUNK_COUNT (sizeof(chunks) / sizeof(chunks[0]))
#define AVGDL_Q8 2219u /* round(52 * 256 / 6) */

static const fixture_term_t terms[] = {
    {"among", {{0, 1}}},
    {"antenna", {{1, 1}, {5, 2}}},
    {"audio", {{4, 1}}},
    {"battery", {{2, 3}, {3, 3}}},
    {"cells", {{2, 1}, {3, 1}}},
    {"charge", {{2, 1}, {3, 1}}},
    {"debounce", {{0, 1}}},
    {"display", {{0, 1}}},
    {"draws", {{0, 1}}},
    {"gauge", {{2, 1}, {3, 1}}},
    {"i2c", {{0, 1}}},
    {"keyboard", {{0, 1}}},
    {"keys", {{0, 1}}},
    {"layers", {{0, 1}}},
    {"many", {{0, 1}}},
    {"matrix", {{0, 1}}},
    {"mentioned", {{0, 1}}},
    {"modifiers", {{0, 1}}},
    {"near", {{5, 1}}},
    {"notes", {{0, 1}}},
    {"placement", {{5, 1}}},
    {"power", {{0, 1}}},
    {"pwm", {{4, 1}}},
    {"radio", {{0, 2}, {1, 1}, {5, 2}}},
    {"rates", {{0, 1}}},
    {"reads", {{0, 1}}},
    {"repeat", {{0, 1}}},
    {"scanning", {{0, 1}}},
    {"shield", {{5, 1}}},
    {"sound", {{4, 1}}},
    {"speaker", {{4, 1}}},
    {"spi", {{0, 1}}},
    {"text", {{0, 1}}},
    {"timing", {{0, 1}}},
    {"tones", {{4, 1}}},
    {"tuning", {{1, 1}}},
    {"wireless", {{1, 1}}},
    {"words", {{0, 1}}},
};
#define TERM_COUNT (sizeof(terms) / sizeof(terms[0]))

static uint8_t fixture[4096];
static size_t fixture_length;

static void put_u16(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value) {
    put_u16(p, value);
    put_u16(p + 2, value >> 16);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Written out again rather than taken from the module under test. */
static uint32_t fnv1a(const char *text) {
    uint32_t hash = 2166136261u;

    for (; *text != '\0'; text++) {
        hash = (hash ^ (uint8_t)*text) * 16777619u;
    }
    return hash;
}

static uint32_t crc32_of(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0u; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

static void seal(uint8_t *file, size_t length) {
    put_u32(file + 24, crc32_of(file + 28, length - 28u));
}

static size_t align4(size_t n) {
    return (n + 3u) & ~(size_t)3u;
}

static void set_section(uint8_t *file, unsigned section, size_t off, size_t len) {
    put_u32(file + 96 + 8u * section, (uint32_t)off);
    put_u32(file + 100 + 8u * section, (uint32_t)len);
}

/* Assemble the CYX1 file for chunks and terms into fixture. */
static void build_fixture(void) {
    size_t order[TERM_COUNT];
    size_t i;
    size_t j;
    size_t at;
    size_t postings_start;
    size_t text_start;
    size_t text_at;

    memset(fixture, 0, sizeof(fixture));
    for (i = 0u; i < TERM_COUNT; i++) {
        order[i] = i;
    }
    for (i = 1u; i < TERM_COUNT; i++) { /* the term table is sorted by hash */
        for (j = i; j > 0u && fnv1a(terms[order[j - 1u]].term) > fnv1a(terms[order[j]].term); j--) {
            size_t swap = order[j];
            order[j] = order[j - 1u];
            order[j - 1u] = swap;
        }
    }

    memcpy(fixture, "CYX1", 4u);
    put_u16(fixture + 4, 1u);
    put_u32(fixture + 12, (uint32_t)CHUNK_COUNT);
    put_u32(fixture + 16, (uint32_t)TERM_COUNT);
    put_u32(fixture + 20, AVGDL_Q8);

    /* terms, then postings */
    at = SEARCH_INDEX_HEADER_SIZE;
    postings_start = at + TERM_COUNT * 12u;
    {
        size_t posting_at = postings_start;
        for (i = 0u; i < TERM_COUNT; i++) {
            const fixture_term_t *term = &terms[order[i]];
            uint16_t df = 0u;

            for (j = 0u; j < 3u && term->postings[j][1] != 0u; j++) {
                put_u32(fixture + posting_at, term->postings[j][0]);
                put_u16(fixture + posting_at + 4u, term->postings[j][1]);
                posting_at += 6u;
                df++;
            }
            put_u32(fixture + at, fnv1a(term->term));
            put_u32(fixture + at + 4u, (uint32_t)(posting_at - postings_start - df * 6u));
            put_u16(fixture + at + 8u, df);
            at += 12u;
        }
        set_section(fixture, SEARCH_SECTION_TERMS, SEARCH_INDEX_HEADER_SIZE, TERM_COUNT * 12u);
        set_section(fixture, SEARCH_SECTION_POSTINGS, postings_start, posting_at - postings_start);
        at = align4(posting_at);
    }

    /* docs and text */
    set_section(fixture, SEARCH_SECTION_DOCS, at, CHUNK_COUNT * 12u);
    text_start = align4(at + CHUNK_COUNT * 12u);
    text_at = text_start;
    for (i = 0u; i < CHUNK_COUNT; i++) {
        size_t length = strlen(chunks[i].text);

        put_u32(fixture + at, (uint32_t)(text_at - text_start));
        put_u32(fixture + at + 4u, (uint32_t)length);
        put_u16(fixture + at + 8u, chunks[i].dl);
        at += 12u;
        memcpy(fixture + text_at, chunks[i].text, length + 1u);
        text_at += length + 1u;
    }
    set_section(fixture, SEARCH_SECTION_TEXT, text_start, text_at - text_start);
    fixture_length = align4(text_at);
    set_section(fixture, SEARCH_SECTION_VECTORS, fixture_length, 0u);
    set_section(fixture, SEARCH_SECTION_RESERVED, fixture_length, 0u);
    seal(fixture, fixture_length);
}

static void write_file(const uint8_t *data, size_t length) {
    FILE *file = fopen(FIXTURE_FILE, "wb");

    if (file != NULL) {
        fwrite(data, 1u, length, file);
        fclose(file);
    }
}

/* Open a copy of the fixture changed by edit (which may reseal it). */
static search_status_t open_edited(search_index_t *index, void (*edit)(uint8_t *file, size_t *length)) {
    static uint8_t copy[sizeof(fixture)];
    size_t length = fixture_length;

    memcpy(copy, fixture, sizeof(copy));
    edit(copy, &length);
    write_file(copy, length);
    return search_index_open(index, FIXTURE_FILE);
}

static size_t term_entry(const uint8_t *file, const char *term) {
    size_t i;

    for (i = 0u; i < TERM_COUNT; i++) {
        size_t at = SEARCH_INDEX_HEADER_SIZE + i * 12u;
        if (read_u32(file + at) == fnv1a(term)) {
            return at;
        }
    }
    return 0u;
}

static search_index_t index_;

static size_t run_query(const char *query, size_t max_hits, search_hit_t *hits, search_status_t *status) {
    size_t count = 99u;

    *status = search_index_query(&index_, query, hits, max_hits, &count);
    return count;
}

static void check_open_reads_header(void) {
    CHECK(search_index_open(&index_, FIXTURE_FILE) == SEARCH_OK);
    CHECK(index_.chunk_count == CHUNK_COUNT);
    CHECK(index_.term_count == TERM_COUNT);
    CHECK(index_.avgdl_q8 == AVGDL_Q8);
    CHECK(index_.dim == 0u);
    /* A fixed-size struct a caller can keep as a static. */
    CHECK(sizeof(search_index_t) < 16u * 1024u);
    search_index_close(&index_);
    CHECK(index_.file == NULL);
}

static void check_ranking(void) {
    search_hit_t hits[SEARCH_MAX_HITS];
    search_status_t status;
    size_t count;

    CHECK(search_index_open(&index_, FIXTURE_FILE) == SEARCH_OK);

    /* Chunk 1 has "radio" once in 4 tokens and outranks chunk 0, which has
     * it twice in 24: BM25, not term frequency alone. */
    count = run_query("radio", SEARCH_MAX_HITS, hits, &status);
    CHECK(status == SEARCH_OK);
    CHECK(count == 3u);
    CHECK(hits[0].chunk_id == 5u && hits[0].score_q16 == 66035);
    CHECK(hits[1].chunk_id == 1u && hits[1].score_q16 == 58262);
    CHECK(hits[2].chunk_id == 0u && hits[2].score_q16 == 41711);

    count = run_query("Antenna, RADIO?", SEARCH_MAX_HITS, hits, &status);
    CHECK(status == SEARCH_OK);
    CHECK(count == 3u);
    CHECK(hits[0].chunk_id == 5u && hits[0].score_q16 == 164125);
    CHECK(hits[1].chunk_id == 1u && hits[1].score_q16 == 144805);
    CHECK(hits[2].chunk_id == 0u && hits[2].score_q16 == 41711);

    count = run_query("pwm", SEARCH_MAX_HITS, hits, &status);
    CHECK(status == SEARCH_OK);
    CHECK(count == 1u && hits[0].chunk_id == 4u && hits[0].score_q16 == 122090);

    /* A repeated term counts once. */
    count = run_query("radio radio RADIO", SEARCH_MAX_HITS, hits, &status);
    CHECK(status == SEARCH_OK);
    CHECK(count == 3u && hits[0].score_q16 == 66035 && hits[2].score_q16 == 41711);

    search_index_close(&index_);
}

static void check_ties_and_truncation(void) {
    search_hit_t hits[SEARCH_MAX_HITS];
    search_hit_t again[SEARCH_MAX_HITS];
    search_status_t status;
    size_t count;

    CHECK(search_index_open(&index_, FIXTURE_FILE) == SEARCH_OK);

    /* Chunks 2 and 3 are identical: the lower id comes first. */
    count = run_query("battery", SEARCH_MAX_HITS, hits, &status);
    CHECK(status == SEARCH_OK);
    CHECK(count == 2u);
    CHECK(hits[0].chunk_id == 2u && hits[1].chunk_id == 3u);
    CHECK(hits[0].score_q16 == 113523 && hits[1].score_q16 == 113523);
    count = run_query("battery", 1u, hits, &status);
    CHECK(status == SEARCH_OK && count == 1u && hits[0].chunk_id == 2u);

    /* Fewer slots than matches keeps the best ones, in order. */
    count = run_query("radio", 2u, hits, &status);
    CHECK(status == SEARCH_OK && count == 2u);
    CHECK(hits[0].chunk_id == 5u && hits[1].chunk_id == 1u);
    count = run_query("radio", 1u, hits, &status);
    CHECK(status == SEARCH_OK && count == 1u && hits[0].chunk_id == 5u);
    count = run_query("radio", 0u, hits, &status);
    CHECK(status == SEARCH_OK && count == 0u);
    /* More slots than SEARCH_MAX_HITS are not an error. */
    count = run_query("radio", 100u, hits, &status);
    CHECK(status == SEARCH_OK && count == 3u);

    /* The same query gives the same ranking. */
    count = run_query("antenna radio battery pwm", SEARCH_MAX_HITS, hits, &status);
    CHECK(status == SEARCH_OK && count == 6u);
    CHECK(run_query("antenna radio battery pwm", SEARCH_MAX_HITS, again, &status) == count);
    CHECK_BYTES_EQ(hits, again, count * sizeof(hits[0]));

    search_index_close(&index_);
}

static void check_empty_queries(void) {
    search_hit_t hits[SEARCH_MAX_HITS];
    search_status_t status;
    char many[256];
    size_t i;

    CHECK(search_index_open(&index_, FIXTURE_FILE) == SEARCH_OK);
    CHECK(run_query("the and of", SEARCH_MAX_HITS, hits, &status) == 0u);
    CHECK(status == SEARCH_OK);
    CHECK(run_query("", SEARCH_MAX_HITS, hits, &status) == 0u);
    CHECK(status == SEARCH_OK);
    CHECK(run_query("a ! ? \xc3\xa9", SEARCH_MAX_HITS, hits, &status) == 0u);
    CHECK(status == SEARCH_OK);
    CHECK(run_query("zebra unicorn", SEARCH_MAX_HITS, hits, &status) == 0u);
    CHECK(status == SEARCH_OK);

    /* More distinct terms than SEARCH_MAX_QUERY_TERMS. */
    many[0] = '\0';
    for (i = 0u; i <= SEARCH_MAX_QUERY_TERMS; i++) {
        char word[16];
        snprintf(word, sizeof(word), "w%02u ", (unsigned)i);
        strcat(many, word);
    }
    CHECK(run_query(many, SEARCH_MAX_HITS, hits, &status) == 0u);
    CHECK(status == SEARCH_ERR_TOO_LARGE);
    search_index_close(&index_);
}

static void check_query_reads_little(void) {
    search_hit_t hits[SEARCH_MAX_HITS];
    search_status_t status;
    uint32_t before;

    CHECK(search_index_open(&index_, FIXTURE_FILE) == SEARCH_OK);
    before = index_.bytes_read;
    CHECK(before == SEARCH_INDEX_HEADER_SIZE + fixture_length - 28u); /* header, then the CRC pass */
    CHECK(run_query("pwm", SEARCH_MAX_HITS, hits, &status) == 1u);
    /* A term table probe of 12 bytes per step, one 6-byte posting, and the
     * document entries from chunk 4 on. */
    CHECK(index_.bytes_read - before <= 7u * 12u + 6u + 2u * 12u);
    search_index_close(&index_);
}

static void check_text(void) {
    char buf[64];

    CHECK(search_index_open(&index_, FIXTURE_FILE) == SEARCH_OK);
    CHECK(search_index_read_title(&index_, 5u, buf, sizeof(buf)) == SEARCH_OK);
    CHECK_STR_EQ(buf, "Radio antenna");
    CHECK(search_index_read_body(&index_, 5u, buf, sizeof(buf)) == SEARCH_OK);
    CHECK_STR_EQ(buf, "antenna placement near the radio shield");
    CHECK(search_index_read_title(&index_, 0u, buf, sizeof(buf)) == SEARCH_OK);
    CHECK_STR_EQ(buf, "Notes");

    /* Too small: what fits, cut at a character boundary. */
    CHECK(search_index_read_title(&index_, 5u, buf, 6u) == SEARCH_ERR_TOO_LARGE);
    CHECK_STR_EQ(buf, "Radio");
    CHECK(search_index_read_title(&index_, 4u, buf, 9u) == SEARCH_OK);
    CHECK_STR_EQ(buf, "Sound \xc3\xa9");
    CHECK(search_index_read_title(&index_, 4u, buf, 8u) == SEARCH_ERR_TOO_LARGE);
    CHECK_STR_EQ(buf, "Sound ");
    CHECK(search_index_read_body(&index_, 0u, buf, sizeof(buf)) == SEARCH_ERR_TOO_LARGE);
    CHECK(strlen(buf) == sizeof(buf) - 1u);
    CHECK(strncmp(buf, "The keyboard reads keys", 23u) == 0);

    CHECK(search_index_read_title(&index_, (uint32_t)CHUNK_COUNT, buf, sizeof(buf)) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_read_body(&index_, 0xFFFFFFFFu, buf, sizeof(buf)) == SEARCH_ERR_ARGUMENT);
    search_index_close(&index_);
}

static void check_arguments(void) {
    search_hit_t hits[SEARCH_MAX_HITS];
    size_t count = 99u;
    char buf[16];

    CHECK(search_index_open(NULL, FIXTURE_FILE) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_open(&index_, NULL) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_open(&index_, "search_index_missing.idx") == SEARCH_ERR_IO);
    search_index_close(NULL);

    /* A closed index. */
    CHECK(search_index_query(&index_, "radio", hits, SEARCH_MAX_HITS, &count) == SEARCH_ERR_ARGUMENT);
    CHECK(count == 0u);
    CHECK(search_index_read_title(&index_, 0u, buf, sizeof(buf)) == SEARCH_ERR_ARGUMENT);

    CHECK(search_index_open(&index_, FIXTURE_FILE) == SEARCH_OK);
    CHECK(search_index_query(NULL, "radio", hits, SEARCH_MAX_HITS, &count) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_query(&index_, NULL, hits, SEARCH_MAX_HITS, &count) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_query(&index_, "radio", NULL, SEARCH_MAX_HITS, &count) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_query(&index_, "radio", hits, SEARCH_MAX_HITS, NULL) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_read_title(NULL, 0u, buf, sizeof(buf)) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_read_title(&index_, 0u, NULL, sizeof(buf)) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_read_title(&index_, 0u, buf, 0u) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_read_body(NULL, 0u, buf, sizeof(buf)) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_read_body(&index_, 0u, NULL, sizeof(buf)) == SEARCH_ERR_ARGUMENT);
    CHECK(search_index_read_body(&index_, 0u, buf, 0u) == SEARCH_ERR_ARGUMENT);
    search_index_close(&index_);
}

/* ---- damaged files ----------------------------------------------------- */

static void cut_header(uint8_t *file, size_t *length) {
    (void)file;
    *length = 100u;
}

static void cut_tail(uint8_t *file, size_t *length) {
    (void)file;
    *length -= 4u;
}

static void bad_magic(uint8_t *file, size_t *length) {
    (void)length;
    memcpy(file, "1YXC", 4u);
}

static void bad_version(uint8_t *file, size_t *length) {
    (void)length;
    put_u16(file + 4, 2u);
}

static void text_past_end(uint8_t *file, size_t *length) {
    put_u32(file + 100 + 8u * SEARCH_SECTION_TEXT, 0x7FFFFFF0u);
    seal(file, *length);
}

static void section_gap(uint8_t *file, size_t *length) {
    /* Docs moved 4 bytes on: no longer contiguous with the postings. */
    put_u32(file + 96 + 8u * SEARCH_SECTION_DOCS, read_u32(file + 96 + 8u * SEARCH_SECTION_DOCS) + 4u);
    seal(file, *length);
}

static void zero_avgdl(uint8_t *file, size_t *length) {
    put_u32(file + 20, 0u);
    seal(file, *length);
}

static void bad_crc(uint8_t *file, size_t *length) {
    file[*length - 8u] ^= 0x20u;
}

static void postings_past_end(uint8_t *file, size_t *length) {
    size_t at = term_entry(file, "radio");
    put_u32(file + at + 4u, read_u32(file + 96 + 8u * SEARCH_SECTION_POSTINGS + 4u) - 6u);
    seal(file, *length);
}

static void chunk_past_end(uint8_t *file, size_t *length) {
    size_t at = term_entry(file, "pwm");
    size_t posting = read_u32(file + 96 + 8u * SEARCH_SECTION_POSTINGS) + read_u32(file + at + 4u);
    put_u32(file + posting, (uint32_t)CHUNK_COUNT);
    seal(file, *length);
}

static void text_off_past_end(uint8_t *file, size_t *length) {
    size_t docs = read_u32(file + 96 + 8u * SEARCH_SECTION_DOCS);
    put_u32(file + docs + 12u, read_u32(file + 100 + 8u * SEARCH_SECTION_TEXT));
    seal(file, *length);
}

static void check_damaged(void) {
    search_hit_t hits[SEARCH_MAX_HITS];
    search_status_t status;
    char buf[32];

    CHECK(open_edited(&index_, cut_header) == SEARCH_ERR_FORMAT);
    CHECK(index_.file == NULL);
    CHECK(open_edited(&index_, cut_tail) == SEARCH_ERR_FORMAT);
    CHECK(open_edited(&index_, bad_magic) == SEARCH_ERR_FORMAT);
    CHECK(open_edited(&index_, bad_version) == SEARCH_ERR_FORMAT);
    CHECK(open_edited(&index_, text_past_end) == SEARCH_ERR_FORMAT);
    CHECK(open_edited(&index_, section_gap) == SEARCH_ERR_FORMAT);
    CHECK(open_edited(&index_, zero_avgdl) == SEARCH_ERR_FORMAT);
    CHECK(open_edited(&index_, bad_crc) == SEARCH_ERR_FORMAT);

    /* A header that checks out but an entry that points outside its section. */
    CHECK(open_edited(&index_, postings_past_end) == SEARCH_OK);
    CHECK(run_query("radio", SEARCH_MAX_HITS, hits, &status) == 0u);
    CHECK(status == SEARCH_ERR_FORMAT);
    CHECK(run_query("pwm", SEARCH_MAX_HITS, hits, &status) == 1u);
    CHECK(status == SEARCH_OK);
    search_index_close(&index_);

    CHECK(open_edited(&index_, chunk_past_end) == SEARCH_OK);
    run_query("pwm", SEARCH_MAX_HITS, hits, &status);
    CHECK(status == SEARCH_ERR_FORMAT);
    search_index_close(&index_);

    CHECK(open_edited(&index_, text_off_past_end) == SEARCH_OK);
    CHECK(search_index_read_title(&index_, 1u, buf, sizeof(buf)) == SEARCH_ERR_FORMAT);
    CHECK_STR_EQ(buf, "");
    CHECK(search_index_read_title(&index_, 2u, buf, sizeof(buf)) == SEARCH_OK);
    search_index_close(&index_);
}

static void check_tokenizer_pieces(void) {
    CHECK(search_index_hash("radio", 5u) == fnv1a("radio"));
    CHECK(search_index_hash("", 0u) == 2166136261u);
    CHECK(search_index_is_stopword("the", 3u));
    CHECK(search_index_is_stopword("yourselves", 10u));
    CHECK(search_index_is_stopword("about", 5u));
    CHECK(!search_index_is_stopword("radio", 5u));
    CHECK(!search_index_is_stopword("th", 2u));
    CHECK(!search_index_is_stopword("theirsx", 7u));
}

void test_search_index(void) {
    build_fixture();
    write_file(fixture, fixture_length);

    check_open_reads_header();
    check_ranking();
    check_ties_and_truncation();
    check_empty_queries();
    check_query_reads_little();
    check_text();
    check_arguments();
    check_damaged();
    check_tokenizer_pieces();

    remove(FIXTURE_FILE);
}
