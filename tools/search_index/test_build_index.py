"""Tests for build_index.py.  Run from the repository root:

    python3 -m unittest discover -s tools/search_index -p 'test_*.py' -v
"""

import contextlib
import io
import os
import re
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build_index as bi  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def write_tree(root, files):
    for rel, text in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


class TokenizerTest(unittest.TestCase):
    def test_case_folding(self):
        self.assertEqual(bi.tokenize("Radio RADIO rAdIo"), [b"radio"] * 3)

    def test_stopwords_dropped(self):
        self.assertEqual(bi.tokenize("The radio and the antenna"), [b"radio", b"antenna"])
        self.assertEqual(bi.tokenize("the and of"), [])

    def test_length_bounds(self):
        self.assertEqual(bi.tokenize("x y zz"), [b"zz"])
        self.assertEqual(bi.tokenize("q" * 32), [b"q" * 32])
        # A longer run is cut into 32-byte pieces; a 1-byte remainder is dropped.
        self.assertEqual(bi.tokenize("q" * 33), [b"q" * 32])
        self.assertEqual(bi.tokenize("q" * 34), [b"q" * 32, b"qq"])
        self.assertEqual(bi.tokenize("q" * 70), [b"q" * 32, b"q" * 32, b"q" * 6])

    def test_underscore_and_digits_kept(self):
        self.assertEqual(bi.tokenize("snake_case i2c 2026 _x"), [b"snake_case", b"i2c", b"2026", b"_x"])

    def test_punctuation_separates(self):
        self.assertEqual(bi.tokenize("wifi-setup,radio.cfg"), [b"wifi", b"setup", b"radio", b"cfg"])

    def test_high_bytes_skipped(self):
        # Bytes >= 0x80 are dropped from the stream; they do not end a token.
        self.assertEqual(bi.tokenize("cafés Zürich"), [b"cafs", b"zrich"])
        self.assertEqual(bi.tokenize("éé é"), [])

    def test_fnv1a(self):
        self.assertEqual(bi.fnv1a32(b""), 0x811C9DC5)
        self.assertEqual(bi.fnv1a32(b"a"), 0xE40C292C)
        self.assertEqual(bi.fnv1a32(b"foobar"), 0xBF9CF968)

    def test_query_terms_distinct(self):
        self.assertEqual(bi.query_hashes("radio Radio RADIO antenna"),
                         [bi.fnv1a32(b"radio"), bi.fnv1a32(b"antenna")])

    def test_stopwords_match_c_header(self):
        with open(os.path.join(REPO, "ai", "search_index.h"), encoding="utf-8") as f:
            header = f.read()
        block = re.search(r"#define SEARCH_STOPWORDS(.*?)\n\n", header, re.S).group(1)
        words = re.findall(r'"([^"]*)"', block)
        # Sorted bytewise, for the reader's binary search.
        self.assertEqual(words, sorted(words, key=lambda w: w.encode("ascii")))
        self.assertEqual(len(words), len(set(words)))
        self.assertEqual(set(words), set(bi.STOPWORDS))
        for word in words:
            # Every stopword is itself a token, or it could never match.
            self.assertEqual(bi._RUN.fullmatch(word.encode("ascii")).group(0), word.encode("ascii"))
            self.assertTrue(bi.TOKEN_MIN <= len(word) <= bi.TOKEN_MAX)


class ChunkingTest(unittest.TestCase):
    def test_paragraphs_packed(self):
        paras = ["a" * 200, "b" * 200, "c" * 200, "d" * 10]
        chunks = bi.pack(paras)
        self.assertEqual(chunks, ["a" * 200 + "\n\n" + "b" * 200, "c" * 200 + "\n\n" + "d" * 10])
        self.assertTrue(all(len(c) <= bi.CHUNK_CHARS for c in chunks))

    def test_long_paragraph_split(self):
        para = " ".join(["word%03d" % i for i in range(200)])  # 1599 chars
        chunks = bi.pack(["short", para, "tail"])
        self.assertTrue(all(len(c) <= bi.CHUNK_CHARS for c in chunks))
        self.assertEqual(chunks[0], "short")
        self.assertEqual(" ".join(" ".join(chunks[1:]).split()), para + " tail")
        unbroken = bi.pack(["x" * 1300])
        self.assertEqual(unbroken, ["x" * 600, "x" * 600, "x" * 100])

    def test_title_from_heading_or_name(self):
        with tempfile.TemporaryDirectory() as root:
            write_tree(root, {"a.md": "intro\n\n# Wifi Setup\n\nbody", "b.txt": "no heading\n## sub"})
            self.assertEqual(bi.chunk_file(os.path.join(root, "a.md"))[0][0], "Wifi Setup")
            self.assertEqual(bi.chunk_file(os.path.join(root, "b.txt"))[0][0], "b.txt")

    def test_corpus_walk_sorted_and_filtered(self):
        with tempfile.TemporaryDirectory() as root:
            write_tree(root, {"z.md": "z", "sub/b.txt": "b", "a.md": "a", "c.rst": "c", "sub/a/x.md": "x"})
            rels = [os.path.relpath(p, root).replace(os.sep, "/") for p in bi.corpus_files(root)]
            self.assertEqual(rels, ["a.md", "sub/a/x.md", "sub/b.txt", "z.md"])
            self.assertEqual(len(bi.chunk_corpus(root, limit=2)), 2)


FIXTURE_CHUNKS = [
    ("Notes", "The keyboard reads keys over i2c and the display draws text over spi; the radio is "
              "mentioned here, and the radio again, among many other words about matrix scanning "
              "debounce timing layers modifiers repeat rates and power"),
    ("Wireless", "radio antenna tuning"),
    ("Battery", "battery charge gauge battery cells"),
    ("Battery", "battery charge gauge battery cells"),
    ("Sound é", "pwm audio speaker tones"),
    ("Radio antenna", "antenna placement near the radio shield"),
]


class FormatTest(unittest.TestCase):
    def setUp(self):
        self.data = bi.build(FIXTURE_CHUNKS)
        self.index = bi.Index(self.data)

    def test_header_fields(self):
        d = self.data
        self.assertEqual(d[0:4], b"CYX1")
        self.assertEqual(d[0:4], bytes([0x43, 0x59, 0x58, 0x31]))
        version, flags, dim, dtype, chunks, terms, avgdl_q8, crc, reserved = \
            struct.unpack_from("<HHHHIIIII", d, 4)
        self.assertEqual((version, flags, dim, dtype, reserved), (1, 0, 0, 0, 0))
        self.assertEqual(chunks, 6)
        self.assertEqual(terms, 38)
        total_dl = sum(self.index.doc(i)[2] for i in range(chunks))
        self.assertEqual(total_dl, 52)
        self.assertEqual(avgdl_q8, round(52 * 256 / 6))
        self.assertEqual(d[32:96], bytes(64))
        self.assertEqual(d[144:160], bytes(16))

    def test_sections_contiguous_and_aligned(self):
        sections = self.index.sections
        self.assertEqual(sections[0][0], bi.HEADER_SIZE)
        for (off, length), (next_off, _) in zip(sections, sections[1:]):
            self.assertEqual(off % 4, 0)
            self.assertEqual(next_off, (off + length + 3) & ~3)
        self.assertEqual(sections[-1][0] + sections[-1][1], len(self.data))
        self.assertEqual(sections[bi.VECTORS][1], 0)
        self.assertEqual(sections[bi.RESERVED][1], 0)

    def test_terms_sorted_by_hash(self):
        hashes = [h for h, _off, _df, _r in self.index.terms()]
        self.assertEqual(hashes, sorted(set(hashes)))

    def test_postings(self):
        expected = {}
        for chunk_id, (title, body) in enumerate(FIXTURE_CHUNKS):
            for token in bi.tokenize(title + "\n" + body):
                tfs = expected.setdefault(bi.fnv1a32(token), {})
                tfs[chunk_id] = tfs.get(chunk_id, 0) + 1
        next_off = 0
        for h, off, df, reserved in self.index.terms():
            self.assertEqual(reserved, 0)
            self.assertEqual(off, next_off)  # in term order, no padding
            postings = self.index.postings(off, df)
            self.assertEqual(dict(postings), expected[h])
            self.assertEqual([c for c, _tf in postings], sorted(expected[h]))
            next_off += df * bi.POSTING.size
        self.assertEqual(next_off, self.index.sections[bi.POSTINGS][1])
        radio = [t for t in self.index.terms() if t[0] == bi.fnv1a32(b"radio")][0]
        self.assertEqual(self.index.postings(radio[1], radio[2]), [(0, 2), (1, 1), (5, 2)])

    def test_docs_and_text(self):
        text = self.index.section(bi.TEXT)
        for chunk_id, (title, body) in enumerate(FIXTURE_CHUNKS):
            off, length, dl, flags = self.index.doc(chunk_id)
            stored = (title + "\n" + body).encode("utf-8")
            self.assertEqual(text[off:off + length], stored)
            self.assertEqual(text[off + length], 0)
            self.assertEqual(dl, len(bi.tokenize(stored)))
            self.assertEqual(flags, 0)
        self.assertEqual(self.index.title(4), "Sound é")

    def test_crc(self):
        crc = struct.unpack_from("<I", self.data, 24)[0]
        self.assertEqual(crc, zlib.crc32(self.data[28:]) & 0xFFFFFFFF)
        damaged = bytearray(self.data)
        damaged[-8] ^= 0x20
        with self.assertRaisesRegex(bi.FormatError, "crc"):
            bi.Index(bytes(damaged))

    def test_rejects_damage(self):
        with self.assertRaises(bi.FormatError):
            bi.Index(self.data[:100])
        with self.assertRaises(bi.FormatError):
            bi.Index(self.data[:-4])
        with self.assertRaises(bi.FormatError):
            bi.Index(b"1YXC" + self.data[4:])
        with self.assertRaises(bi.FormatError):
            bi.Index(self.data[:4] + b"\x02\x00" + self.data[6:])

    def test_scores_match_c_fixture(self):
        # tests/test_search_index.c expects these exact values from the C reader.
        self.assertEqual(self.index.query("radio"), [(5, 66035), (1, 58262), (0, 41711)])
        self.assertEqual(self.index.query("Antenna, RADIO?"), [(5, 164125), (1, 144805), (0, 41711)])
        self.assertEqual(self.index.query("pwm"), [(4, 122090)])
        self.assertEqual(self.index.query("battery"), [(2, 113523), (3, 113523)])
        self.assertEqual(self.index.query("radio radio RADIO"), self.index.query("radio"))
        self.assertEqual(self.index.query("radio", top=1), [(5, 66035)])
        self.assertEqual(self.index.query("the and of"), [])

    def test_empty_corpus(self):
        index = bi.Index(bi.build([]))
        self.assertEqual((index.chunk_count, index.term_count, index.avgdl_q8), (0, 0, 0))
        self.assertEqual(index.query("anything"), [])

    def test_deterministic(self):
        self.assertEqual(bi.build(FIXTURE_CHUNKS), self.data)


class CorpusRankingTest(unittest.TestCase):
    """Four documents where the right order is plain from the text."""

    FILES = {
        "battery.md": "# Battery\n\nThe battery charges over USB-C. A full battery lasts a day; "
                      "the battery gauge reads the cells over i2c.",
        "wifi.md": "# Wi-Fi\n\nJoin a network from the connection settings. The radio scans for "
                   "networks and the password is stored on the card.",
        "keyboard.txt": "Keyboard notes\n\nThe keyboard talks i2c to the Pico. Keys repeat when held.",
        "sound/speaker.md": "# Sound\n\nThe speaker plays PWM tones. Battery level does not change the volume.",
    }

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.corpus = os.path.join(self.tmp.name, "corpus")
        write_tree(self.corpus, self.FILES)
        self.out = os.path.join(self.tmp.name, "a.idx")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(bi.main(["--corpus", self.corpus, "--out", self.out]), 0)
        self.index = bi.load(self.out)
        # Sorted path order: battery.md, keyboard.txt, sound/speaker.md, wifi.md.
        self.titles = [self.index.title(i) for i in range(self.index.chunk_count)]

    def tearDown(self):
        self.tmp.cleanup()

    def ranked_titles(self, query):
        return [self.titles[c] for c, _s in self.index.query(query)]

    def test_chunk_order_and_titles(self):
        self.assertEqual(self.titles, ["Battery", "keyboard.txt", "Sound", "Wi-Fi"])

    def test_rankings(self):
        self.assertEqual(self.ranked_titles("battery"), ["Battery", "Sound"])
        self.assertEqual(self.ranked_titles("i2c"), ["keyboard.txt", "Battery"])
        self.assertEqual(self.ranked_titles("wifi password"), ["Wi-Fi"])
        self.assertEqual(self.ranked_titles("pwm speaker volume"), ["Sound"])
        self.assertEqual(self.ranked_titles("the of and"), [])

    def test_query_command(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(bi.main(["--index", self.out, "--query", "battery", "--top", "1"]), 0)
        lines = out.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        chunk_id, score, title = lines[0].split("\t")
        self.assertEqual((chunk_id, title), ("0", "Battery"))
        self.assertGreater(float(score), 0.0)

    def test_query_command_rejects_bad_index(self):
        with open(self.out, "r+b") as f:
            f.write(b"XXXX")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(bi.main(["--index", self.out, "--query", "battery"]), 1)


if __name__ == "__main__":
    unittest.main()
