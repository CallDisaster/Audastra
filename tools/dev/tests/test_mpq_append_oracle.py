"""原创 synthetic MPQ append 夹具；不读取作者地图或调用产品 writer。

F01 encrypted tables / known-answer 常量于 2026-10-05 由独立 stdlib 算术
生成：1280-step PRNG 线性生成后转置；以模 2^32 的独立 encryption 实现。
生成器未 import/执行 oracle、tests 或产品代码。公开 StormLib 的两表 key
与首个 crypt word 作外部锚点。测试侧 reference 仅构造变体和负向输入；
F01 与两个 decrypt known-answer 的 expected 使用下方预先冻结 literal。
"""
from __future__ import annotations

import ast
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mpq_append_oracle as oracle

MASK, EXISTS, SINGLE, RAW_FLAGS = 0xFFFFFFFF, 0x80000000, 0x01000000, 0x81010200
DOO_OLD, DOO_NEW = b"DOO-old!", b"DOO-new!"
JASS_OLD, JASS_NEW = b"J-old", b"J-new!"
OBJECT = b"\x01\x00\x11\xff"
UNKNOWN_RAW = bytes.fromhex("f1378c00d42abe19378f06c9ee52810b")
NAMED_HASHES = {
    "war3map.doo": (3507244074, 4151898854, 3452709199),
    "war3map.j": (214580198, 3382118375, 2511869006),
    "war3map.w3u": (2403077103, 4115557215, 375997887),
}
HASH_ENTRIES = [
    (MASK, MASK, 65535, 65535, MASK),
    (MASK, MASK, 65535, 65535, MASK),
    (4151898854, 3452709199, 0, 65280, 0),
    (MASK, MASK, 65535, 65535, MASK),
    (MASK, MASK, 65535, 65535, MASK - 1),
    (19088743, 2309737967, 0, 65280, 3),
    (3382118375, 2511869006, 0, 0, 1),
    (4115557215, 375997887, 0, 0, 2),
]
SOURCE_BLOCKS = [(32, 8, 8, 0x81000000), (40, 5, 5, 0x81000000),
    (45, 4, 4, 0x81000000), (49, 16, 4096, RAW_FLAGS)]
CANDIDATE_BLOCKS = [(257, 8, 8, 0x81000000), (265, 6, 6, 0x81000000),
    (271, 4, 4, 0x81000000), SOURCE_BLOCKS[3]]
HASH_CIPHER = bytes.fromhex(
    "3330c37928d93298bc736f9fb2884ee917a20484c440bfca63bf7b980ce7197e"
    "34a63441a8e11486120c6163a6325c6197def1e8c0ec53dd61672e90b36c72c"
    "138061de80529e4b1c4af3d648ce319c2578d6ff369373a983116202aa680fcb1a"
    "d282fb207f1967337c0663907aec73a9c002e6e69b6fc20574ee9a90ddeec10")
SOURCE_BLOCK_CIPHER = bytes.fromhex(
    "ab67483da9d308cad3ba35f8d97233e9d8b5a2b311919c0e8d7a37c70b65d2b6"
    "6eab78edd6cd39168d6f7cb2a97d2d7889f9e58477ca50ab2facc34f110ee640")
CANDIDATE_BLOCK_CIPHER = bytes.fromhex(
    "8a66483d8ad408cad4d735f8fa2f37e938161eb4501b841e0d4d10d4ef87c55e"
    "a82bd0939c3a81870b75b14cef33035b8f7872803d2a6d18b5046c6057779b66")
VECTORS = (
    (0xC3AF3770, "cccf3c8627d9329803c9b361190d7f9e", "00000000ffffffff67452301efcdab89"),
    (0xEC83B3A3, "ab67483db1d308cae3aa35f8d98132e9", "20000000100000000010000000020181"),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def independent_json_sha(value):
    return sha(json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8"))


def reference_words():
    result, seed = [0] * 1280, 1048577
    for step in range(1280):
        halves = []
        for _ in range(2):
            seed = (125 * seed + 3) % 2796203
            halves.append(seed % 65536)
        column, group = divmod(step, 5)
        result[group * 256 + column] = halves[0] * 65536 + halves[1]
    return result


def reference_encrypt(data, key):
    words, carry, output = reference_words(), 4008636142, []
    for (plain,) in struct.iter_unpack("<I", data):
        carry = (carry + words[1024 + key % 256]) % (1 << 32)
        output.append(plain ^ ((key + carry) % (1 << 32)))
        rotated = (((MASK ^ key) * (1 << 21)) + 286331153) % (1 << 32)
        key = rotated | (key // (1 << 11))
        carry = (plain + 33 * carry + 3) % (1 << 32)
    return struct.pack("<" + "I" * len(output), *output)


def reference_hash(name, kind):
    words, first, second = reference_words(), 2146271213, 4008636142
    for byte in name.encode("utf-8").replace(b"/", b"\\"):
        if 97 <= byte <= 122:
            byte -= 32
        first = words[kind * 256 + byte] ^ ((first + second) % (1 << 32))
        second = (byte + first + 33 * second + 3) % (1 << 32)
    return first


def index_cipher(entries, format_string, key):
    return reference_encrypt(b"".join(struct.pack(format_string, *entry) for entry in entries), key)


def header(archive_size, block_pos, block_count=4, hash_count=8, version=0, shift=3, size=32, hash_pos=65):
    return struct.pack("<4sIIHHIIII", b"MPQ\x1a", size, archive_size, version, shift, hash_pos, block_pos, hash_count, block_count)


class Fixture:
    def __init__(self, base=512, rewrite_object=True):
        self.base, self.rewrite_object = base, rewrite_object
        self.names = ["war3map.doo", "war3map.j", "war3map.w3u"]
        self.hashes, self.blocks = list(HASH_ENTRIES), list(SOURCE_BLOCKS)
        self.prefix = b"" if base == 0 else b"HM3W" + b"\x5a" * (base - 4)

    def source(self):
        hash_cipher = HASH_CIPHER if self.hashes == HASH_ENTRIES else index_cipher(self.hashes, "<IIHHI", 0xC3AF3770)
        block_cipher = SOURCE_BLOCK_CIPHER if self.blocks == SOURCE_BLOCKS else index_cipher(self.blocks, "<IIII", 0xEC83B3A3)
        source_size = 193 + len(block_cipher)
        return self.prefix + header(source_size, 193, len(self.blocks)) + DOO_OLD + JASS_OLD + OBJECT + UNKNOWN_RAW + hash_cipher + block_cipher

    def candidate_parts(self, source):
        cursor = len(source) - self.base
        blocks, payload = list(self.blocks), bytearray()
        indexes = [0, 1] + ([2] if self.rewrite_object else [])
        for index, data in zip(indexes, [DOO_NEW, JASS_NEW, OBJECT]):
            blocks[index] = (cursor, len(data), len(data), 0x81000000)
            payload.extend(data)
            cursor += len(data)
        return blocks, bytes(payload), cursor

    def candidate(self, source, override_blocks=None):
        blocks, payload, table_pos = self.candidate_parts(source)
        if override_blocks is not None:
            blocks = override_blocks
        cipher = CANDIDATE_BLOCK_CIPHER if blocks == CANDIDATE_BLOCKS else index_cipher(blocks, "<IIII", 0xEC83B3A3)
        result = bytearray(source)
        result[self.base + 8:self.base + 12] = struct.pack("<I", table_pos + len(cipher))
        result[self.base + 20:self.base + 24] = struct.pack("<I", table_pos)
        return bytes(result) + payload + cipher

    def plan(self, source, candidate, optional=True):
        source_sha, candidate_sha = sha(source), sha(candidate)
        intent = {"schema": "mpq-append-intent/v1", "source_sha256": source_sha,
            "inventory": {"schema": "mpq-append-inventory/v1", "source_sha256": source_sha, "names": list(self.names)},
            "business_names": ["war3map.doo", "war3map.j"], "source_doo": {"bytes": 8, "sha256": sha(DOO_OLD)},
            "expected_doo": {"bytes": 8, "sha256": sha(DOO_NEW)}, "optional_source_equal": []}
        if optional:
            intent["optional_source_equal"].append({"name": "war3map.w3u", "bytes": 4, "sha256": sha(OBJECT), "source_sha256": source_sha})
        return {"schema": "mpq-append-bound-plan/v1", "intent": intent, "intent_sha256": independent_json_sha(intent),
            "candidate_sha256": candidate_sha, "jass_witness": {"name": "war3map.j", "bytes": 6,
                "sha256": sha(JASS_NEW), "candidate_sha256": candidate_sha, "origin": "product-candidate-decoded-identity"}}


class AppendOracleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="original-mpq-append-")
        self.addCleanup(self.temp.cleanup)
        self.source_path = Path(self.temp.name) / "source.mpq"
        self.candidate_path = Path(self.temp.name) / "candidate.mpq"

    def write_inputs(self, source, candidate):
        self.source_path.write_bytes(source)
        self.candidate_path.write_bytes(candidate)

    def proof(self, fixture=None, source=None, candidate=None, plan=None):
        fixture = fixture or Fixture()
        source = fixture.source() if source is None else source
        candidate = fixture.candidate(source) if candidate is None else candidate
        plan = fixture.plan(source, candidate) if plan is None else plan
        self.write_inputs(source, candidate)
        return oracle.prove_append(self.source_path, self.candidate_path, plan)

    def rejected(self, codes, fixture=None, source=None, candidate=None, plan=None):
        codes = {codes} if isinstance(codes, str) else set(codes)
        with self.assertRaises(oracle.OracleError) as result:
            self.proof(fixture, source, candidate, plan)
        self.assertIn(result.exception.code, codes)
        return result.exception

    def test_public_keys_and_fixed_decrypt_answers(self):
        self.assertEqual(oracle._CRYPT[0], 0x55C636E2)
        self.assertEqual(oracle.hash_name("(hash table)", 3), 0xC3AF3770)
        self.assertEqual(oracle.hash_name("(block table)", 3), 0xEC83B3A3)
        for key, cipher, plain in VECTORS:
            with self.subTest(key=key):
                self.assertEqual(oracle._decrypt_index(bytes.fromhex(cipher), key), bytes.fromhex(plain))
        for name, triple in NAMED_HASHES.items():
            for kind, expected in enumerate(triple):
                self.assertEqual(oracle.hash_name(name, kind), expected)
                self.assertEqual(oracle.hash_name(name.upper(), kind), expected)
        self.assertEqual(oracle.hash_name("scripts/war3map.j", 1), oracle.hash_name("SCRIPTS\\WAR3MAP.J", 1))

    def test_fixed_f01_layout_and_all_protected_slice_bytes(self):
        fixture, result = Fixture(), self.proof()
        self.assertEqual(len(fixture.source()), 769)
        self.assertEqual(len(fixture.candidate(fixture.source())), 851)
        self.assertTrue(result["passed"])
        self.assertEqual(result["source"]["anonymous_block_count"], 1)
        self.assertEqual(result["candidate"]["header"]["block_table_pos"], 275)
        self.assertEqual(result["original_region"]["bytes_compared"], 761)
        protected = result["protected_blocks"]
        self.assertEqual(len(protected), 1)
        self.assertEqual(protected[0]["raw_bytes"], 16)
        self.assertEqual(protected[0]["block_entry"], list(SOURCE_BLOCKS[3]))
        self.assertEqual(protected[0]["source_raw_sha256"], sha(UNKNOWN_RAW))
        self.assertEqual(protected[0]["source_raw_sha256"], protected[0]["candidate_raw_sha256"])
        self.assertEqual({row["name"] for row in result["actual_replacements"]}, {"WAR3MAP.DOO", "WAR3MAP.J", "WAR3MAP.W3U"})
        self.assertFalse(result["member_decrypted"])
        self.assertFalse(result["member_decompressed"])
        self.assertFalse(result["tiara_content_parsed"])

    def test_no_prefix_optional_not_rewritten(self):
        fixture = Fixture(base=0, rewrite_object=False)
        result = self.proof(fixture)
        self.assertEqual(result["source"]["archive_base"], 0)
        self.assertEqual(len(result["actual_replacements"]), 2)
        self.assertEqual(len(result["protected_blocks"]), 2)
        self.assertEqual(result["candidate"]["header"]["block_table_pos"], 271)

    def test_multiple_unknowns_zero_length_and_nonexistent_entries(self):
        fixture = Fixture()
        fixture.blocks += [(65, 0, 0, 0x80030000), (0, 0, 0, 0)]
        fixture.hashes[0] = (0x99887766, 0x11223344, 0, 0, 4)
        result = self.proof(fixture)
        self.assertEqual(result["source"]["anonymous_block_count"], 2)
        empty = next(row for row in result["protected_blocks"] if row["block_index"] == 4)
        self.assertEqual(empty["source_raw_sha256"], sha(b""))
        self.assertEqual(result["unchanged_nonexistent_block_count"], 1)

    def test_append_preserves_an_already_appended_source(self):
        fixture = Fixture()
        first_source = fixture.source()
        first_candidate = fixture.candidate(first_source)
        current = list(CANDIDATE_BLOCKS)
        # 新源的 old DOO 与 JASS 不再是原 fixture bytes；业务意图独立固定下一次期望。
        source = first_candidate
        payloads = [(1, b"J-next"), (0, b"DOO-next")]
        new = list(current)
        cursor, tail = len(source) - fixture.base, bytearray()
        for index, payload in payloads:
            new[index] = (cursor, len(payload), len(payload), 0x81000000)
            cursor += len(payload)
            tail.extend(payload)
        candidate = bytearray(source)
        struct.pack_into("<I", candidate, fixture.base + 8, cursor + 64)
        struct.pack_into("<I", candidate, fixture.base + 20, cursor)
        candidate = bytes(candidate) + tail + index_cipher(new, "<IIII", 0xEC83B3A3)
        plan = fixture.plan(source, candidate)
        plan["intent"]["source_doo"] = {"bytes": 8, "sha256": sha(DOO_NEW)}
        plan["intent"]["expected_doo"] = {"bytes": 8, "sha256": sha(b"DOO-next")}
        plan["jass_witness"].update(bytes=6, sha256=sha(b"J-next"))
        plan["intent_sha256"] = independent_json_sha(plan["intent"])
        result = self.proof(fixture, source, candidate, plan)
        self.assertEqual(result["original_region"]["bytes_compared"], len(source) - 8)
        self.assertEqual([row["name"] for row in result["actual_replacements"]], ["WAR3MAP.J", "WAR3MAP.DOO"])

    def test_preflight_and_inspect_do_not_modify_inputs(self):
        fixture = Fixture()
        source, candidate = fixture.source(), fixture.candidate(fixture.source())
        plan = fixture.plan(source, candidate)
        self.write_inputs(source, candidate)
        before = (self.source_path.read_bytes(), self.candidate_path.read_bytes())
        inspection = oracle.inspect_archive(self.source_path, sha(source), plan["intent"]["inventory"])
        preflight = oracle.preflight_intent(self.source_path, plan["intent"])
        self.assertTrue(inspection["passed"] and preflight["passed"])
        self.assertEqual(preflight["intent_sha256"], independent_json_sha(plan["intent"]))
        self.assertEqual(before, (self.source_path.read_bytes(), self.candidate_path.read_bytes()))

    @unittest.skipUnless(os.name == "nt", "Windows fstat/path stat 的 birth time 回归")
    def test_windows_rewritten_file_and_open_handle_identity(self):
        fixture = Fixture()
        source = fixture.source()
        candidate = fixture.candidate(source)
        self.write_inputs(source, candidate)
        for rewrite in range(32):
            # 同一实体重新写入，模拟多个subTest复用路径；ctime不能当跨接口字段。
            self.source_path.write_bytes(source + bytes([rewrite]))
            self.source_path.write_bytes(source)
            with self.source_path.open("rb") as stream:
                handle, path = os.fstat(stream.fileno()), self.source_path.stat()
                self.assertEqual(handle.st_birthtime_ns, path.st_birthtime_ns)
                self.assertEqual(oracle._fingerprint(handle), oracle._fingerprint(path))
        result = oracle.prove_append(self.source_path, self.candidate_path, fixture.plan(source, candidate))
        self.assertTrue(result["passed"])
        self.assertEqual(result["source"]["identity"]["time_field"], "st_birthtime_ns")
        self.assertEqual(self.source_path.read_bytes(), source)
        self.assertEqual(self.candidate_path.read_bytes(), candidate)

    def test_protected_raw_slice_changes_have_precise_block_location(self):
        fixture, source = Fixture(), Fixture().source()
        for index in (0, 7, 15):
            with self.subTest(raw_byte=index):
                candidate = bytearray(fixture.candidate(source))
                candidate[fixture.base + 49 + index] ^= 1
                error = self.rejected("PROTECTED_RAW_SLICE_CHANGED", fixture, source, bytes(candidate))
                self.assertEqual(error.location["block_index"], 3)

    def test_full_source_region_covers_prefix_old_payload_and_old_table(self):
        fixture, source = Fixture(), Fixture().source()
        for physical in (0, 511, 512 + 32, 512 + 40, 512 + 193, 512 + 256):
            with self.subTest(offset=physical):
                candidate = bytearray(fixture.candidate(source))
                candidate[physical] ^= 1
                error = self.rejected("SOURCE_REGION_CHANGED", fixture, source, bytes(candidate))
                self.assertEqual(error.location["physical_offset"], physical)

    def test_third_header_field_change_is_rejected(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = bytearray(fixture.candidate(source))
        struct.pack_into("<H", candidate, fixture.base + 14, 4)
        self.rejected("HEADER_CHANGE", fixture, source, bytes(candidate))

    def test_reserved_byte_may_be_ff_but_must_not_change(self):
        fixture, source = Fixture(), Fixture().source()
        hashes = list(HASH_ENTRIES)
        hashes[2] = (*hashes[2][:3], 0, hashes[2][4])
        candidate = bytearray(fixture.candidate(source))
        candidate[fixture.base + 65:fixture.base + 193] = index_cipher(hashes, "<IIHHI", 0xC3AF3770)
        self.rejected("HASH_TABLE_CHANGED", fixture, source, bytes(candidate))

    def test_header_window_cannot_overlap_a_payload(self):
        for offset, compressed in ((8, 4), (20, 4), (0, 32)):
            with self.subTest(offset=offset):
                fixture = Fixture()
                fixture.blocks[3] = (offset, compressed, 4096, RAW_FLAGS)
                self.rejected("BLOCK_METADATA_OVERLAP", fixture)

    def test_hash_alias_duplicate_locale_and_orphan_rejected(self):
        mutations = (
            (5, (19088743, 2309737967, 0, 65280, 0), "HASH_BLOCK_ALIAS"),
            (5, (4151898854, 3452709199, 0, 65280, 3), "HASH_PAIR_DUPLICATE"),
            (5, (4151898854, 3452709199, 1, 65280, 3), "HASH_LOCALE_PLATFORM"),
            (5, (19088743, 2309737967, 0, 65281, 3), "HASH_LOCALE_PLATFORM"),
            (5, (19088743, 2309737967, 0, 65280, 7), "HASH_BLOCK_INDEX"),
            (5, (MASK, MASK, 65535, 65535, MASK - 1), "HASH_BLOCK_BIJECTION"),
        )
        for slot, value, code in mutations:
            with self.subTest(code=code):
                fixture = Fixture()
                fixture.hashes[slot] = value
                self.rejected(code, fixture)

    def test_named_lookup_stops_at_empty_slot(self):
        fixture = Fixture()
        fixture.hashes[3], fixture.hashes[2] = fixture.hashes[2], fixture.hashes[3]
        self.rejected("NAME_UNREACHABLE", fixture)

    def test_named_hash_change_does_not_rebind_authority(self):
        fixture, source = Fixture(), Fixture().source()
        hashes = list(HASH_ENTRIES)
        old = hashes[5]
        hashes[5] = (old[0] ^ 1, *old[1:])
        candidate = bytearray(fixture.candidate(source))
        candidate[fixture.base + 65:fixture.base + 193] = index_cipher(hashes, "<IIHHI", 0xC3AF3770)
        self.rejected("HASH_TABLE_CHANGED", fixture, source, bytes(candidate))

    def test_every_protected_block_field_is_immutable(self):
        fixture, source = Fixture(), Fixture().source()
        baseline = fixture.candidate_parts(source)[0]
        for field, value in ((0, 48), (1, 15), (2, 4095), (3, 0x81000200)):
            with self.subTest(field=field):
                blocks = list(baseline)
                changed = list(blocks[3])
                changed[field] = value
                blocks[3] = tuple(changed)
                self.rejected("REPLACEMENT_SET", fixture, source, fixture.candidate(source, blocks))

    def test_nonexistent_block_metadata_is_immutable(self):
        fixture = Fixture()
        fixture.blocks.append((0, 0, 0, 0))
        source = fixture.source()
        blocks = fixture.candidate_parts(source)[0]
        blocks[4] = (1, 0, 0, 0)
        self.rejected("REPLACEMENT_SET", fixture, source, fixture.candidate(source, blocks))

    def test_no_unfrozen_object_rewrite_even_with_identical_payload(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = fixture.candidate(source)
        self.rejected("REPLACEMENT_SET", fixture, source, candidate, fixture.plan(source, candidate, optional=False))

    def test_source_equal_object_payload_must_match_the_source_witness(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = bytearray(fixture.candidate(source))
        candidate[fixture.base + 271] ^= 1
        error = self.rejected("REPLACEMENT_PAYLOAD", fixture, source, bytes(candidate))
        self.assertEqual(error.location["block_index"], 2)

    def test_doo_and_jass_payloads_must_match_their_bound_witnesses(self):
        fixture, source = Fixture(), Fixture().source()
        for offset, expected_block in ((257, 0), (265, 1)):
            with self.subTest(block=expected_block):
                candidate = bytearray(fixture.candidate(source))
                candidate[fixture.base + offset] ^= 1
                error = self.rejected("REPLACEMENT_PAYLOAD", fixture, source, bytes(candidate))
                self.assertEqual(error.location["block_index"], expected_block)

    def test_required_replacements_cannot_be_omitted(self):
        fixture, source = Fixture(), Fixture().source()
        for index in (0, 1):
            with self.subTest(index=index):
                blocks = fixture.candidate_parts(source)[0]
                blocks[index] = SOURCE_BLOCKS[index]
                self.rejected("REPLACEMENT_SET", fixture, source, fixture.candidate(source, blocks))

    def test_forbidden_names_cannot_become_optional_via_a_plan(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = fixture.candidate(source)
        for name in ("war3map.wtg", "war3map.wct", "tiara.blp", "(listfile)", "(attributes)", "new.member"):
            with self.subTest(name=name):
                plan = fixture.plan(source, candidate)
                plan["intent"]["inventory"]["names"].append(name)
                plan["intent"]["optional_source_equal"][0]["name"] = name
                plan["intent_sha256"] = independent_json_sha(plan["intent"])
                self.rejected("PLAN_AUTHORITY", fixture, source, candidate, plan)

    def test_real_wtg_block_rewrite_is_rejected_before_payload_comparison(self):
        fixture = Fixture()
        fixture.names[2] = "war3map.wtg"
        hashes = [(MASK, MASK, 65535, 65535, MASK) for _ in range(8)]
        for index, name in enumerate(fixture.names):
            slot = reference_hash(name, 0) % 8
            while hashes[slot][4] != MASK:
                slot = (slot + 1) % 8
            hashes[slot] = (reference_hash(name, 1), reference_hash(name, 2), 0, 0, index)
        slot = next(index for index, entry in enumerate(hashes) if entry[4] == MASK)
        hashes[slot] = HASH_ENTRIES[5]
        fixture.hashes = hashes
        source = fixture.source()
        candidate = fixture.candidate(source)
        plan = fixture.plan(source, candidate, optional=False)
        error = self.rejected("REPLACEMENT_SET", fixture, source, candidate, plan)
        self.assertEqual(error.location["unauthorized_blocks"], [2])

    def test_replacement_flags_size_and_zero_payload_rejected(self):
        fixture, source = Fixture(), Fixture().source()
        for block in ((257, 8, 8, 0x81010000), (257, 8, 8, 0x81000200), (257, 7, 8, 0x81000000), (257, 0, 0, 0x81000000)):
            with self.subTest(block=block):
                blocks = fixture.candidate_parts(source)[0]
                blocks[0] = block
                self.rejected("REPLACEMENT_FLAGS_SIZE", fixture, source, fixture.candidate(source, blocks))

    def test_replacement_cannot_reuse_original_offset(self):
        fixture, source = Fixture(), Fixture().source()
        blocks = fixture.candidate_parts(source)[0]
        blocks[0] = (32, 8, 8, 0x81000000)
        # metadata完全等于源，意味着遗漏必需DOO replacement，先拒权限集合正确。
        self.rejected("REPLACEMENT_SET", fixture, source, fixture.candidate(source, blocks))
        # 源允许非Single原块；候选改为Single但复用原offset，集合/size/flags均合法。
        fixture.blocks[0] = (32, 8, 8, 0x80000000)
        source = fixture.source()
        blocks = fixture.candidate_parts(source)[0]
        blocks[0] = (32, 8, 8, 0x81000000)
        self.rejected("REPLACEMENT_NOT_APPENDED", fixture, source, fixture.candidate(source, blocks))

    def test_gap_and_overlap_and_table_padding(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = fixture.candidate(source)
        # 插入一个字节但修正 filePos/header，使其仍是可解析候选；布局必须拒绝。
        for location, code in ((257, "APPEND_LAYOUT"), (275, "NEW_BLOCK_TABLE_EOF")):
            with self.subTest(location=location):
                altered = bytearray(candidate[:fixture.base + location] + b"\0" + candidate[fixture.base + location:])
                blocks = list(CANDIDATE_BLOCKS)
                if location == 257:
                    blocks[:3] = [(pos + 1, compressed, size, flags) for pos, compressed, size, flags in blocks[:3]]
                struct.pack_into("<I", altered, fixture.base + 8, 340)
                struct.pack_into("<I", altered, fixture.base + 20, 276)
                altered[fixture.base + 276:] = index_cipher(blocks, "<IIII", 0xEC83B3A3)
                self.rejected(code, fixture, source, bytes(altered))
        blocks = list(CANDIDATE_BLOCKS)
        blocks[1] = (264, 6, 6, 0x81000000)
        self.rejected("BLOCK_OVERLAP", fixture, source, fixture.candidate(source, blocks))

    def test_eof_trailing_and_truncated_table(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = fixture.candidate(source)
        for altered in (candidate + b"x", candidate[:-1]):
            self.rejected("ARCHIVE_EOF", fixture, source, altered)
        altered = bytearray(candidate[:-1])
        struct.pack_into("<I", altered, fixture.base + 8, len(altered) - fixture.base)
        self.rejected("TABLE_BOUNDS", fixture, source, bytes(altered))
        self.rejected("ARCHIVE_EOF", fixture, source + b"x", candidate)

    def test_table_key_and_byte_order_corruption_rejected(self):
        fixture, source = Fixture(), Fixture().source()
        plain = b"".join(struct.pack("<IIII", *entry) for entry in CANDIDATE_BLOCKS)
        for cipher in (reference_encrypt(plain, 0xC3AF3770), b"".join(CANDIDATE_BLOCK_CIPHER[index:index + 4][::-1] for index in range(0, 64, 4))):
            candidate = fixture.candidate(source)[:-64] + cipher
            self.rejected({"BLOCK_FLAGS", "BLOCK_BOUNDS", "HASH_BLOCK_INDEX", "HASH_BLOCK_BIJECTION"}, fixture, source, candidate)

    def test_unknown_patch_crc_and_conflicting_source_flags(self):
        for flags in (0x81000001, 0x81110200, 0x85010200, 0x81000300, 0x81020200):
            with self.subTest(flags=flags):
                fixture = Fixture()
                fixture.blocks[3] = (49, 16, 4096, flags)
                self.rejected("BLOCK_FLAGS", fixture)

    def test_metadata_and_data_overlap_and_u32_bounds(self):
        for block, code in (((64, 16, 4096, RAW_FLAGS), "BLOCK_METADATA_OVERLAP"),
                            ((48, 16, 4096, RAW_FLAGS), "BLOCK_OVERLAP"),
                            ((0xFFFFFFF0, 32, 4096, RAW_FLAGS), "BLOCK_BOUNDS")):
            with self.subTest(block=block):
                fixture = Fixture()
                fixture.blocks[3] = block
                self.rejected(code, fixture)
        fixture, source = Fixture(), Fixture().source()
        candidate = bytearray(fixture.candidate(source))
        struct.pack_into("<I", candidate, fixture.base + 20, 100)
        self.rejected("TABLE_OVERLAP", fixture, source, bytes(candidate))

    def test_unsupported_header_variants_and_ambiguous_base(self):
        fixture, source = Fixture(), Fixture().source()
        baseline = fixture.candidate(source)
        for offset, format_string, value in ((4, "<I", 44), (12, "<H", 1), (14, "<H", 16)):
            with self.subTest(offset=offset):
                candidate = bytearray(baseline)
                struct.pack_into(format_string, candidate, fixture.base + offset, value)
                self.rejected("HEADER_VARIANT", fixture, source, bytes(candidate))
        for marker, code in ((b"MPQ\x1a", "HEADER_UNIQUE"), (b"MPQ\x1b", "HEADER_VARIANT")):
            candidate = bytearray(baseline)
            candidate[:4] = marker
            self.rejected(code, fixture, source, bytes(candidate))
        candidate = bytearray(baseline)
        candidate[fixture.base:fixture.base + 4] = b"NONE"
        self.rejected("HEADER_UNIQUE", fixture, source, bytes(candidate))

    def test_counts_and_runtime_budgets_fail_closed(self):
        fixture, source = Fixture(), Fixture().source()
        for offset, value in ((24, 0), (24, 7), (24, 1 << 21), (28, 0), (28, 1 << 21)):
            with self.subTest(offset=offset, value=value):
                candidate = bytearray(fixture.candidate(source))
                struct.pack_into("<I", candidate, fixture.base + offset, value)
                self.rejected("TABLE_BUDGET", fixture, source, bytes(candidate))
        for symbol, value, code in (("MAX_PHYSICAL", 768, "FILE_BUDGET"), ("MAX_LOOKUP_STEPS", 1, "LOOKUP_BUDGET"),
                                    ("MAX_APPEND", 17, "APPEND_BUDGET"), ("MAX_SCAN", 512, "HEADER_UNIQUE")):
            with self.subTest(budget=symbol), mock.patch.object(oracle, symbol, value):
                self.rejected(code, fixture)

    def test_plan_cannot_change_binding_authority_or_payload_budget(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = fixture.candidate(source)
        mutations = (
            (lambda plan: plan.update(intent_sha256="0" * 64), "PLAN_INTENT_BINDING"),
            (lambda plan: plan.update(schema="unknown"), "PLAN_SCHEMA"),
            (lambda plan: plan["jass_witness"].update(candidate_sha256="0" * 64), "PLAN_SCRIPT_BINDING"),
            (lambda plan: plan["jass_witness"].update(bytes=0), "PLAN_SIZE"),
            (lambda plan: plan["jass_witness"].update(bytes=64 * 1024 * 1024 + 1), "PLAN_SIZE"),
            (lambda plan: plan["intent"]["optional_source_equal"][0].update(source_sha256="0" * 64), "PLAN_AUTHORITY"),
            (lambda plan: plan["intent"]["inventory"]["names"].append("WAR3MAP.DOO"), "NAME_DUPLICATE"),
            (lambda plan: plan["intent"]["optional_source_equal"].append(plan["intent"]["optional_source_equal"][0].copy()), "PLAN_AUTHORITY"),
        )
        for mutation, code in mutations:
            with self.subTest(code=code):
                plan = fixture.plan(source, candidate)
                mutation(plan)
                if code != "PLAN_INTENT_BINDING":
                    plan["intent_sha256"] = independent_json_sha(plan["intent"])
                self.rejected(code, fixture, source, candidate, plan)

    def test_noop_doo_and_source_equal_size_mismatch_rejected(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = fixture.candidate(source)
        plan = fixture.plan(source, candidate)
        plan["intent"]["expected_doo"] = dict(plan["intent"]["source_doo"])
        plan["intent_sha256"] = independent_json_sha(plan["intent"])
        self.rejected("PLAN_DOO_NOOP", fixture, source, candidate, plan)
        plan = fixture.plan(source, candidate)
        plan["intent"]["optional_source_equal"][0]["bytes"] = 5
        plan["intent_sha256"] = independent_json_sha(plan["intent"])
        self.rejected("PLAN_SOURCE_SIZE", fixture, source, candidate, plan)

    def test_same_entity_and_wrong_whole_file_sha(self):
        fixture, source = Fixture(), Fixture().source()
        candidate = fixture.candidate(source)
        plan = fixture.plan(source, candidate)
        self.write_inputs(source, candidate)
        with self.assertRaises(oracle.OracleError) as error:
            oracle.prove_append(self.source_path, self.source_path, plan)
        self.assertEqual(error.exception.code, "INPUT_IDENTITY")
        plan["candidate_sha256"] = "0" * 64
        plan["jass_witness"]["candidate_sha256"] = "0" * 64
        self.rejected("INPUT_IDENTITY", fixture, source, candidate, plan)

    def test_external_source_change_during_proof_is_detected(self):
        fixture, source = Fixture(), Fixture().source()
        compare = oracle._compare_source_region
        def changed_after_compare(open_source, open_candidate):
            result = compare(open_source, open_candidate)
            altered = bytearray(source)
            altered[4] ^= 1
            self.source_path.write_bytes(altered)
            return result
        with mock.patch.object(oracle, "_compare_source_region", side_effect=changed_after_compare):
            self.rejected("INPUT_DRIFT", fixture)

    def test_json_receipt_and_safe_path_contracts(self):
        for text, code in (("{\"a\":1,\"a\":2}", "JSON_DUPLICATE"), ("{\"a\":NaN}", "JSON_NONFINITE"),
                           ("{\"a\":1e9999}", "JSON_NONFINITE")):
            with self.subTest(text=text), self.assertRaises(oracle.OracleError) as error:
                oracle.strict_json(text)
            self.assertEqual(error.exception.code, code)
        for name in ("../tiara.blp", "C:/tiara.blp", "/tiara.blp", "x//y", "x\0y"):
            with self.assertRaises(oracle.OracleError) as error:
                oracle.canonical_name(name)
            self.assertEqual(error.exception.code, "NAME_INVALID")
        fixture = Fixture()
        source = fixture.source()
        self.write_inputs(source, fixture.candidate(source))
        for target in (self.source_path, self.candidate_path):
            with self.assertRaises(oracle.OracleError) as error:
                oracle._write_receipt(target, {"passed": False}, [self.source_path, self.candidate_path])
            self.assertEqual(error.exception.code, "RECEIPT_PATH")

    def test_reader_has_no_product_or_member_decoder_execution_paths(self):
        tree = ast.parse(Path(oracle.__file__).read_text(encoding="utf-8"))
        imported = {alias.name.split(".")[0] for node in ast.walk(tree) if isinstance(node, ast.Import) for alias in node.names}
        imported |= {node.module.split(".")[0] for node in ast.walk(tree) if isinstance(node, ast.ImportFrom) and node.module}
        self.assertFalse(imported & {"subprocess", "ctypes", "zlib", "bz2", "lzma", "stormlib"})
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.ClassDef)):
                for function in ([node] if isinstance(node, ast.FunctionDef) else node.body):
                    if not isinstance(function, ast.FunctionDef):
                        continue
                    decrypt_calls = [call for call in ast.walk(function) if isinstance(call, ast.Call)
                        and isinstance(call.func, ast.Name) and call.func.id == "_decrypt_index"]
                    if decrypt_calls:
                        self.assertEqual(function.name, "_read_tables")
                        self.assertEqual(len(decrypt_calls), 2)


if __name__ == "__main__":
    unittest.main()
