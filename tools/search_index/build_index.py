#!/usr/bin/env python3
"""Build a CYX1 keyword search index for the PicoCalc, or query one.

    build_index.py --corpus <dir> --out <file> [--limit N]
    build_index.py --index <file> --query "text" [--top N]

docs/design/on-device-search.md pins the file format, the tokenizer and the
BM25 scoring.  The firmware reader (ai/search_index.c) implements the same
three; --query is the reference its ranking is checked against.

Standard library only.
"""

import argparse
import math
import os
import re
import struct
import sys
import zlib

MAGIC = b"CYX1"
VERSION = 1
HEADER_SIZE = 160
SECTION_COUNT = 6
TERMS, POSTINGS, DOCS, TEXT, VECTORS, RESERVED = range(SECTION_COUNT)
TERM_ENTRY = struct.Struct("<IIHH")
POSTING = struct.Struct("<IH")
DOC_ENTRY = struct.Struct("<IIHH")

TOKEN_MIN = 2
TOKEN_MAX = 32
CHUNK_CHARS = 600
MAX_HITS = 8  # SEARCH_MAX_HITS in ai/search_index.h
K1 = 1.2
B = 0.75

# Mirrored as SEARCH_STOPWORDS in ai/search_index.h, which must stay sorted.
STOPWORDS = frozenset("""
about above after again against all am an and any are as at be because been
before being below between both but by can did do does doing down during each
few for from further had has have having he her here hers herself him himself
his how if in into is it its itself just me more most my myself no nor not now
of off on once only or other our ours ourselves out over own same she should so
some such than that the their theirs them themselves then there these they this
those through to too under until up very was we were what when where which
while who whom why will with you your yours yourself yourselves
""".split())

_RUN = re.compile(rb"[a-z0-9_]+")
_HEADING = re.compile(r"^#[ \t]+(\S.*?)\s*$")
_HIGH_BYTES = bytes(range(0x80, 0x100))


class FormatError(ValueError):
    """The file is not a valid CYX1 index."""


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def tokenize(text):
    """The tokens of text (str or bytes), in order, as bytes.

    Bytes >= 0x80 are dropped without ending a token, A-Z is folded, runs of
    [a-z0-9_] are cut into 32-byte pieces, and pieces shorter than 2 bytes or
    in STOPWORDS are dropped.
    """
    if isinstance(text, str):
        text = text.encode("utf-8")
    text = text.translate(None, _HIGH_BYTES).lower()
    tokens = []
    for run in _RUN.findall(text):
        for start in range(0, len(run), TOKEN_MAX):
            piece = run[start:start + TOKEN_MAX]
            if len(piece) >= TOKEN_MIN and piece.decode("ascii") not in STOPWORDS:
                tokens.append(piece)
    return tokens


def query_hashes(query):
    """The distinct term hashes of a query, first occurrence first."""
    seen = []
    for token in tokenize(query):
        h = fnv1a32(token)
        if h not in seen:
            seen.append(h)
    return seen


def bm25_idf(n, df):
    return math.log(1.0 + (n - df + 0.5) / (df + 0.5))


def bm25_q16(idf, tf, dl, avgdl_q8):
    """One term's contribution in 1/65536ths; written as in ai/search_index.c."""
    avgdl = avgdl_q8 / 256.0
    norm = K1 * (1.0 - B + B * dl / avgdl)
    part = idf * (tf * (K1 + 1.0)) / (tf + norm)
    return int(math.floor(part * 65536.0 + 0.5))


# ---- corpus ---------------------------------------------------------------

def corpus_files(root):
    """*.md and *.txt under root, recursively, sorted by relative path."""
    found = []
    for directory, _dirs, names in os.walk(root):
        for name in names:
            if name.endswith(".md") or name.endswith(".txt"):
                path = os.path.join(directory, name)
                found.append((os.path.relpath(path, root).replace(os.sep, "/"), path))
    found.sort()
    return [path for _rel, path in found]


def file_title(text, path):
    for line in text.split("\n"):
        match = _HEADING.match(line)
        if match:
            return match.group(1)
    return os.path.basename(path)


def paragraphs(text):
    """Blocks of text separated by blank lines, trailing spaces trimmed."""
    result = []
    current = []
    for line in text.split("\n"):
        line = line.rstrip()
        if line:
            current.append(line)
        elif current:
            result.append("\n".join(current))
            current = []
    if current:
        result.append("\n".join(current))
    return result


def split_long(paragraph, limit=CHUNK_CHARS):
    """Cut a paragraph longer than limit at whitespace where it can."""
    pieces = []
    while len(paragraph) > limit:
        cut = max(paragraph.rfind(" ", 0, limit + 1), paragraph.rfind("\n", 0, limit + 1))
        if cut <= 0:
            cut = limit
        pieces.append(paragraph[:cut].rstrip())
        paragraph = paragraph[cut:].lstrip()
    if paragraph:
        pieces.append(paragraph)
    return pieces


def pack(paras, limit=CHUNK_CHARS):
    """Pack consecutive paragraphs, joined by a blank line, into chunks of at
    most limit characters.  Only a paragraph longer than limit is split."""
    chunks = []
    current = ""
    for para in paras:
        for piece in split_long(para, limit):
            if not current:
                current = piece
            elif len(current) + 2 + len(piece) <= limit:
                current += "\n\n" + piece
            else:
                chunks.append(current)
                current = piece
    if current:
        chunks.append(current)
    return chunks


def chunk_file(path):
    """(title, body) chunks of one file."""
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", errors="replace")
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\0", "")
    title = file_title(text, path).replace("\n", " ")
    return [(title, body) for body in pack(paragraphs(text))]


def chunk_corpus(root, limit=None):
    chunks = []
    for path in corpus_files(root):
        for chunk in chunk_file(path):
            if limit is not None and len(chunks) >= limit:
                return chunks
            chunks.append(chunk)
    return chunks


# ---- writing --------------------------------------------------------------

def _align4(n):
    return (n + 3) & ~3


def build(chunks):
    """The bytes of a CYX1 index over (title, body) chunks.

    A chunk's stored text is "title\\nbody"; its tokens, and so its length dl,
    are those of that whole text.
    """
    postings = {}  # hash -> {chunk_id: tf}
    docs = []
    text = bytearray()
    total_dl = 0
    for chunk_id, (title, body) in enumerate(chunks):
        if "\n" in title:
            raise ValueError("a title may not contain a newline")
        stored = (title + "\n" + body).encode("utf-8")
        if b"\0" in stored:
            raise ValueError("chunk text may not contain NUL")
        tokens = tokenize(stored)
        if len(tokens) > 0xFFFF:
            raise ValueError("chunk %d has too many tokens" % chunk_id)
        for token in tokens:
            tfs = postings.setdefault(fnv1a32(token), {})
            tfs[chunk_id] = tfs.get(chunk_id, 0) + 1
        docs.append((len(text), len(stored), len(tokens)))
        text += stored + b"\0"
        total_dl += len(tokens)

    n = len(chunks)
    avgdl_q8 = (total_dl * 256 * 2 + n) // (2 * n) if n else 0

    terms_bytes = bytearray()
    postings_bytes = bytearray()
    for h in sorted(postings):
        tfs = postings[h]
        if len(tfs) > 0xFFFF:
            raise ValueError("term %08x is in too many chunks" % h)
        terms_bytes += TERM_ENTRY.pack(h, len(postings_bytes), len(tfs), 0)
        for chunk_id in sorted(tfs):
            postings_bytes += POSTING.pack(chunk_id, min(tfs[chunk_id], 0xFFFF))
    docs_bytes = b"".join(DOC_ENTRY.pack(off, length, dl, 0) for off, length, dl in docs)

    bodies = [bytes(terms_bytes), bytes(postings_bytes), docs_bytes, bytes(text), b"", b""]
    sections = []
    payload = bytearray()
    offset = HEADER_SIZE
    for body in bodies:
        start = _align4(offset)
        payload += b"\0" * (start - offset)
        sections.append((start, len(body)))
        payload += body
        offset = start + len(body)
    if offset > 0xFFFFFFFF:
        raise ValueError("index too large")

    def header(crc):
        h = bytearray(HEADER_SIZE)
        struct.pack_into("<4sHHHHIIIII", h, 0, MAGIC, VERSION, 0, 0, 0, n, len(postings), avgdl_q8, crc, 0)
        for i, (off, length) in enumerate(sections):
            struct.pack_into("<II", h, 96 + 8 * i, off, length)
        return h

    data = header(0) + payload
    crc = zlib.crc32(bytes(data[28:])) & 0xFFFFFFFF
    return bytes(header(crc) + payload)


# ---- reading --------------------------------------------------------------

class Index:
    """A parsed CYX1 index, checked as the firmware checks it."""

    def __init__(self, data):
        if len(data) < HEADER_SIZE:
            raise FormatError("truncated header")
        magic, version, self.flags, self.dim, self.dtype, self.chunk_count, self.term_count, \
            self.avgdl_q8, crc, _reserved = struct.unpack_from("<4sHHHHIIIII", data, 0)
        if magic != MAGIC or version != VERSION:
            raise FormatError("bad magic or version")
        self.model_id = data[32:96].rstrip(b"\0")
        self.sections = [struct.unpack_from("<II", data, 96 + 8 * i) for i in range(SECTION_COUNT)]
        expected = HEADER_SIZE
        for off, length in self.sections:
            if off != expected or off + length > len(data):
                raise FormatError("bad section bounds")
            expected = _align4(off + length)
        if self.sections[-1][0] + self.sections[-1][1] != len(data):
            raise FormatError("bad section bounds")
        if self.sections[TERMS][1] != self.term_count * TERM_ENTRY.size or \
                self.sections[DOCS][1] != self.chunk_count * DOC_ENTRY.size:
            raise FormatError("bad section lengths")
        if zlib.crc32(data[28:]) & 0xFFFFFFFF != crc:
            raise FormatError("bad crc")
        self.data = data

    def section(self, i):
        off, length = self.sections[i]
        return self.data[off:off + length]

    def terms(self):
        terms = self.section(TERMS)
        return [TERM_ENTRY.unpack_from(terms, i * TERM_ENTRY.size) for i in range(self.term_count)]

    def postings(self, postings_off, df):
        section = self.section(POSTINGS)
        return [POSTING.unpack_from(section, postings_off + i * POSTING.size) for i in range(df)]

    def doc(self, chunk_id):
        return DOC_ENTRY.unpack_from(self.section(DOCS), chunk_id * DOC_ENTRY.size)

    def text(self, chunk_id):
        off, length, _dl, _flags = self.doc(chunk_id)
        return bytes(self.section(TEXT)[off:off + length])

    def title(self, chunk_id):
        return self.text(chunk_id).split(b"\n", 1)[0].decode("utf-8", errors="replace")

    def query(self, text, top=MAX_HITS):
        """[(chunk_id, score_q16)], best first, ties by lower chunk id."""
        by_hash = {h: (off, df) for h, off, df, _r in self.terms()}
        scores = {}
        for h in query_hashes(text):
            if h not in by_hash:
                continue
            off, df = by_hash[h]
            idf = bm25_idf(self.chunk_count, df)
            for chunk_id, tf in self.postings(off, df):
                dl = self.doc(chunk_id)[2]
                scores[chunk_id] = scores.get(chunk_id, 0) + bm25_q16(idf, tf, dl, self.avgdl_q8)
        ranked = sorted(((c, s) for c, s in scores.items() if s > 0), key=lambda cs: (-cs[1], cs[0]))
        return ranked[:top]


def load(path):
    with open(path, "rb") as f:
        return Index(f.read())


# ---- command line ---------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--corpus", help="directory of *.md and *.txt files to index")
    parser.add_argument("--out", help="index file to write")
    parser.add_argument("--limit", type=int, help="index at most N chunks")
    parser.add_argument("--index", help="index file to query")
    parser.add_argument("--query", help="query text")
    parser.add_argument("--top", type=int, default=MAX_HITS, help="results to print (default %(default)s)")
    args = parser.parse_args(argv)

    if args.corpus is not None and args.out is not None:
        chunks = chunk_corpus(args.corpus, args.limit)
        data = build(chunks)
        with open(args.out, "wb") as f:
            f.write(data)
        print("%d chunks, %d bytes -> %s" % (len(chunks), len(data), args.out), file=sys.stderr)
        return 0
    if args.index is not None and args.query is not None:
        try:
            index = load(args.index)
        except FormatError as e:
            print("%s: %s" % (args.index, e), file=sys.stderr)
            return 1
        for chunk_id, score in index.query(args.query, args.top):
            print("%d\t%.6f\t%s" % (chunk_id, score / 65536.0, index.title(chunk_id)))
        return 0
    parser.error("give --corpus and --out, or --index and --query")
    return 2


if __name__ == "__main__":
    sys.exit(main())
