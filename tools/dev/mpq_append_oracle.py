"""独立 classic MPQ v0 append 证明；只解索引，成员只比较原始 slice。

不调用产品 MPQ/verify/CLI，不解密、解压或导出任何成员正文。
只读输入；显式 receipt 不覆盖既有文件；不证明编译/native-save/游戏运行。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys

sys.dont_write_bytecode = True

U32 = 0xFFFFFFFF
EMPTY, DELETED = U32, U32 - 1
EXISTS, SINGLE = 0x80000000, 0x01000000
IMPLODE, COMPRESS, ENCRYPTED, FIX_KEY = 0x100, 0x200, 0x10000, 0x20000
PATCH, SECTOR_CRC = 0x100000, 0x4000000
KNOWN_FLAGS = EXISTS | SINGLE | IMPLODE | COMPRESS | ENCRYPTED | FIX_KEY | PATCH | SECTOR_CRC
HASH_KEY, BLOCK_KEY = 0xC3AF3770, 0xEC83B3A3
MAX_PHYSICAL, MAX_SCAN, CHUNK = 512 * 1024 * 1024, 1024 * 1024, 1024 * 1024
MAX_TABLE_COUNT, MAX_NAMES = 1 << 20, 65536
MAX_PAYLOAD, MAX_APPEND, MAX_REPLACEMENTS = 64 * 1024 * 1024, 128 * 1024 * 1024, 9
MAX_LOOKUP_STEPS, MAX_JSON_BYTES = 4 * (1 << 20), 8 * 1024 * 1024
SHA_PATTERN = re.compile(r"[0-9a-f]{64}")
DOO = "WAR3MAP.DOO"
JASS_NAMES = {"WAR3MAP.J", "SCRIPTS\\WAR3MAP.J"}
NATIVE_NAMES = {"WAR3MAP." + suffix for suffix in ("W3U", "W3A", "W3T", "W3H", "W3Q", "W3D", "W3B")}
SPECIAL_NAMES = ("(listfile)", "(attributes)", "(signature)")


class OracleError(ValueError):
    def __init__(self, code, message, **location):
        super().__init__(message)
        self.code, self.message, self.location = code, message, location

    def receipt(self):
        return {"code": self.code, "message": self.message, "location": self.location}


def check(condition, code, message, **location):
    if not condition:
        raise OracleError(code, message, **location)


def exact_keys(value, keys, label):
    check(isinstance(value, dict) and set(value) == set(keys), "PLAN_SCHEMA", "字段或对象类型不符合合同", field=label)


def sha_value(value, label):
    check(isinstance(value, str) and SHA_PATTERN.fullmatch(value) is not None, "PLAN_SHA", "需要 lowercase SHA-256", field=label)
    return value


def size_value(value, label, limit=MAX_PAYLOAD):
    check(type(value) is int and 0 < value <= limit, "PLAN_SIZE", "payload 大小无效或超预算", field=label)
    return value


def canonical_name(value):
    check(isinstance(value, str), "NAME_INVALID", "成员名必须为字符串")
    try:
        encoded = value.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise OracleError("NAME_INVALID", "成员名不是有效 UTF-8") from error
    check(0 < len(encoded) <= 1024 and all(byte >= 32 and byte != 127 for byte in encoded), "NAME_INVALID", "成员名为空、包含控制字节或超预算")
    normalized = bytes(92 if byte == 47 else byte - 32 if 97 <= byte <= 122 else byte for byte in encoded)
    segments = normalized.split(b"\\")
    check(b":" not in normalized and all(part and part not in (b".", b"..") for part in segments), "NAME_INVALID", "成员名必须为安全相对路径")
    return normalized.decode("utf-8", errors="strict")


def _make_crypt_table():
    result, seed = [0] * 0x500, 0x100001
    for column in range(256):
        for group in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            high = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            result[column + group * 256] = high | (seed & 0xFFFF)
    return tuple(result)


_CRYPT = _make_crypt_table()


def hash_name(value, kind):
    check(type(kind) is int and 0 <= kind <= 3, "HASH_TYPE", "名称 hash 类型无效")
    name, first, second = canonical_name(value).encode("utf-8"), 0x7FED7FED, 0xEEEEEEEE
    for byte in name:
        first = (_CRYPT[(kind << 8) + byte] ^ ((first + second) & U32)) & U32
        second = (byte + first + second + (second << 5) + 3) & U32
    return first


def _decrypt_index(ciphertext, key):
    check(len(ciphertext) % 16 == 0, "INDEX_LENGTH", "索引长度不是完整 entry")
    output, second = bytearray(len(ciphertext)), 0xEEEEEEEE
    for offset in range(0, len(ciphertext), 4):
        second = (second + _CRYPT[0x400 + (key & 0xFF)]) & U32
        value = struct.unpack_from("<I", ciphertext, offset)[0] ^ ((key + second) & U32)
        struct.pack_into("<I", output, offset, value)
        key = ((((~key & U32) << 21) + 0x11111111) | (key >> 11)) & U32
        second = (value + second + (second << 5) + 3) & U32
    return bytes(output)


def canonical_digest(value):
    wire = json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(wire).hexdigest()


def strict_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            check(key not in result, "JSON_DUPLICATE", "JSON 字段重复", field=key)
            result[key] = value
        return result

    def constant(value):
        raise OracleError("JSON_NONFINITE", "JSON 不接受非有限常量", constant=value)

    def floating(value):
        result = float(value)
        check(result == result and result not in (float("inf"), float("-inf")), "JSON_NONFINITE", "JSON 数值溢出")
        return result

    check(isinstance(text, str) and len(text.encode("utf-8")) <= MAX_JSON_BYTES, "JSON_BUDGET", "JSON 字节超预算")
    return json.loads(text, object_pairs_hook=pairs, parse_constant=constant, parse_float=floating)


def _fingerprint(info):
    # Windows Python 3.13 的 fstat/path stat 对 ctime 可能返回不同含义。
    # 使用两接口一致的 birth time；非 Windows 仍检查 inode change time。
    if os.name == "nt":
        birth = getattr(info, "st_birthtime_ns", None)
        check(type(birth) is int, "FINGERPRINT_UNSUPPORTED", "Windows 身份检查需要 st_birthtime_ns")
        change_or_birth = birth
    else:
        change_or_birth = info.st_ctime_ns
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, change_or_birth)


class _Archive:
    def __init__(self, path, expected_sha256, names):
        self.path = Path(path).resolve(strict=True)
        self.expected_sha256 = sha_value(expected_sha256, "archive_sha256")
        self.file = None
        try:
            self.file = self.path.open("rb")
            before = os.fstat(self.file.fileno())
            check(stat.S_ISREG(before.st_mode) and 32 <= before.st_size <= MAX_PHYSICAL, "FILE_BUDGET", "输入不是预算内普通文件")
            self.before = _fingerprint(before)
            self.physical_size = before.st_size
            check(self.sha_range(0, self.physical_size) == expected_sha256, "INPUT_IDENTITY", "输入整文件 SHA 不匹配")
            check(_fingerprint(self.path.stat()) == self.before, "INPUT_DRIFT", "输入路径与打开句柄身份不同")
            self._read_header()
            self._read_tables()
            self._validate_structure()
            self._bind_names(names)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.file is not None:
            self.file.close()
            self.file = None

    def read(self, offset, size):
        check(type(offset) is int and type(size) is int and 0 <= offset <= self.physical_size
              and 0 <= size <= self.physical_size - offset, "READ_BOUNDS", "只读范围越界", offset=offset, bytes=size)
        self.file.seek(offset)
        data = self.file.read(size)
        check(len(data) == size, "INPUT_TRUNCATED", "输入在读取期间截断", offset=offset, bytes=size)
        return data

    def sha_range(self, offset, size):
        digest, cursor = hashlib.sha256(), 0
        while cursor < size:
            amount = min(CHUNK, size - cursor)
            digest.update(self.read(offset + cursor, amount))
            cursor += amount
        return digest.hexdigest()

    def _read_header(self):
        scan = self.read(0, min(self.physical_size, MAX_SCAN))
        positions, userdata = [], []
        for offset in range(0, len(scan) - 3, 512):
            signature = scan[offset:offset + 4]
            if signature == b"MPQ\x1a":
                positions.append(offset)
            elif signature == b"MPQ\x1b":
                userdata.append(offset)
        check(not userdata, "HEADER_VARIANT", "本批不支持 userdata MPQ", offsets=userdata[:8])
        check(len(positions) == 1, "HEADER_UNIQUE", "扫描范围必须只有一个 512 对齐 MPQ 头", count=len(positions))
        self.base = positions[0]
        check(self.base + 32 <= len(scan), "HEADER_VARIANT", "完整 classic header 不在扫描范围")
        self.header_bytes = scan[self.base:self.base + 32]
        signature, header_size, archive_size, version, shift, hash_pos, block_pos, hash_count, block_count = struct.unpack("<4sIIHHIIII", self.header_bytes)
        check(signature == b"MPQ\x1a" and header_size == 32 and version == 0 and 0 <= shift <= 15,
              "HEADER_VARIANT", "只支持 32-byte formatVersion=0 classic MPQ", header_size=header_size, version=version, sector_shift=shift)
        check(self.base + archive_size == self.physical_size, "ARCHIVE_EOF", "声明长度必须精确到物理 EOF")
        check(0 < hash_count <= MAX_TABLE_COUNT and hash_count & (hash_count - 1) == 0
              and 0 < block_count <= MAX_TABLE_COUNT, "TABLE_BUDGET", "索引 count 无效或超预算")
        self.header = {"header_size": header_size, "archive_size": archive_size, "format_version": version,
            "sector_shift": shift, "hash_table_pos": hash_pos, "block_table_pos": block_pos,
            "hash_count": hash_count, "block_count": block_count}
        self.archive_size, self.hash_pos, self.block_pos = archive_size, hash_pos, block_pos
        self.hash_count, self.block_count = hash_count, block_count
        self.hash_size, self.block_size = hash_count * 16, block_count * 16
        for label, offset, size in (("hash", hash_pos, self.hash_size), ("block", block_pos, self.block_size)):
            check(32 <= offset <= archive_size and size <= archive_size - offset, "TABLE_BOUNDS", "索引范围越界", table=label)
        check(hash_pos + self.hash_size <= block_pos or block_pos + self.block_size <= hash_pos,
              "TABLE_OVERLAP", "两张索引表重叠")

    def _read_tables(self):
        self.hash_cipher = self.read(self.base + self.hash_pos, self.hash_size)
        self.block_cipher = self.read(self.base + self.block_pos, self.block_size)
        self.hash_entries = tuple(struct.iter_unpack("<IIHHI", _decrypt_index(self.hash_cipher, HASH_KEY)))
        self.blocks = tuple(struct.iter_unpack("<IIII", _decrypt_index(self.block_cipher, BLOCK_KEY)))

    def _validate_structure(self):
        live_ranges, existing, active, pairs, referenced = [], set(), {}, set(), set()
        metadata = ((0, 32), (self.hash_pos, self.hash_pos + self.hash_size), (self.block_pos, self.block_pos + self.block_size))
        for index, (offset, compressed, size, flags) in enumerate(self.blocks):
            if not flags & EXISTS:
                continue
            existing.add(index)
            check(not flags & ~KNOWN_FLAGS and not flags & (PATCH | SECTOR_CRC)
                  and not (flags & IMPLODE and flags & COMPRESS)
                  and (not flags & FIX_KEY or flags & ENCRYPTED), "BLOCK_FLAGS", "现存块 flags 不支持", block_index=index)
            check(offset <= self.archive_size and compressed <= self.archive_size - offset,
                  "BLOCK_BOUNDS", "现存块 raw slice 越界", block_index=index)
            if compressed:
                end = offset + compressed
                check(all(end <= start or offset >= finish for start, finish in metadata),
                      "BLOCK_METADATA_OVERLAP", "成员 raw slice 与 header/索引重叠", block_index=index)
                live_ranges.append((offset, end, index))
        live_ranges.sort()
        for first, second in zip(live_ranges, live_ranges[1:]):
            check(first[1] <= second[0], "BLOCK_OVERLAP", "现存成员 raw slices 重叠", block_index=second[2])
        for index, (name_a, name_b, locale, platform, block_index) in enumerate(self.hash_entries):
            if block_index in (EMPTY, DELETED):
                continue
            check(locale == 0 and platform & 0xFF == 0, "HASH_LOCALE_PLATFORM", "active hash locale/platform 必须为零", hash_index=index)
            check(block_index in existing, "HASH_BLOCK_INDEX", "active hash 未指向界内 Exists 块", hash_index=index)
            check((name_a, name_b) not in pairs, "HASH_PAIR_DUPLICATE", "active hashpair 重复", hash_index=index)
            check(block_index not in referenced, "HASH_BLOCK_ALIAS", "两个 active hash 共用 block", hash_index=index)
            pairs.add((name_a, name_b))
            referenced.add(block_index)
            active[block_index] = index
        check(referenced == existing, "HASH_BLOCK_BIJECTION", "现存块与 active hash 不是一一对应")
        self.active, self.existing = active, existing
        self.hash_pairs = {entry[:2]: index for index, entry in enumerate(self.hash_entries) if entry[4] not in (EMPTY, DELETED)}
        self.lookup_steps, self.lookup_cache = 0, {}

    def lookup(self, name, required=True):
        name = canonical_name(name)
        if name in self.lookup_cache:
            return self.lookup_cache[name]
        pair = hash_name(name, 1), hash_name(name, 2)
        found = self.hash_pairs.get(pair)
        if found is None:
            check(not required, "NAME_MISSING", "声明的具名成员不存在", name=name)
            return None
        cursor = hash_name(name, 0) & (self.hash_count - 1)
        for _ in range(self.hash_count):
            self.lookup_steps += 1
            check(self.lookup_steps <= MAX_LOOKUP_STEPS, "LOOKUP_BUDGET", "累计名称 probe 超预算")
            if self.hash_entries[cursor][4] == EMPTY:
                break
            if cursor == found:
                block_index = self.hash_entries[cursor][4]
                self.lookup_cache[name] = block_index
                return block_index
            cursor = (cursor + 1) & (self.hash_count - 1)
        raise OracleError("NAME_UNREACHABLE", "具名 hash 在 bucket probe 中不可达", name=name, hash_index=found)

    def _bind_names(self, names):
        check(isinstance(names, list) and 0 < len(names) <= MAX_NAMES, "INVENTORY_BUDGET", "具名 inventory 为空或超预算")
        self.named, seen, block_names = {}, set(), {}
        for name in names:
            key = canonical_name(name)
            check(key not in seen, "NAME_DUPLICATE", "inventory 规范成员名重复", name=key)
            seen.add(key)
            block_index = self.lookup(key)
            check(block_index not in block_names, "NAME_BLOCK_ALIAS", "inventory 多名字共用同 block", name=key)
            self.named[key], block_names[block_index] = block_index, key
        for name in SPECIAL_NAMES:
            key = canonical_name(name)
            if key in self.named:
                continue
            block_index = self.lookup(key, required=False)
            if block_index is not None:
                check(block_index not in block_names, "NAME_BLOCK_ALIAS", "特殊成员名共用 block", name=key)
                self.named[key], block_names[block_index] = block_index, key
        self.anonymous = self.existing - set(self.named.values())

    def unchanged(self):
        try:
            stable = (_fingerprint(os.fstat(self.file.fileno())) == self.before
                and _fingerprint(self.path.stat()) == self.before
                and self.sha_range(0, self.physical_size) == self.expected_sha256)
        except OSError as error:
            raise OracleError("INPUT_DRIFT", "输入路径/句柄在证明期间不可回读") from error
        check(stable, "INPUT_DRIFT", "输入在独立证明期间发生变化")

    def identity(self):
        return {"path": str(self.path), "bytes": self.physical_size, "sha256": self.expected_sha256,
            "device": self.before[0], "file_id": self.before[1], "mtime_ns": self.before[3],
            "birth_or_change_time_ns": self.before[4], "time_field": "st_birthtime_ns" if os.name == "nt" else "st_ctime_ns"}

    def summary(self):
        return {"identity": self.identity(), "archive_base": self.base, "header": self.header,
            "encrypted_hash_table_sha256": hashlib.sha256(self.hash_cipher).hexdigest(),
            "encrypted_block_table_sha256": hashlib.sha256(self.block_cipher).hexdigest(),
            "existing_block_count": len(self.existing), "active_hash_count": len(self.active),
            "named_block_count": len(self.named), "anonymous_block_count": len(self.anonymous),
            "anonymous_blocks": [{"hash_index": self.active[index], "block_index": index,
                "hash_entry": list(self.hash_entries[self.active[index]]), "block_entry": list(self.blocks[index])}
                for index in sorted(self.anonymous)],
            "anonymous_classification": "active hashes absent from supplied inventory plus existing special names"}


def _inventory(value, source_sha256):
    exact_keys(value, ("schema", "source_sha256", "names"), "inventory")
    check(value["schema"] == "mpq-append-inventory/v1" and value["source_sha256"] == source_sha256,
          "INVENTORY_BINDING", "inventory 未绑定同一 source SHA/schema")
    names = value["names"]
    check(isinstance(names, list) and 0 < len(names) <= MAX_NAMES, "INVENTORY_BUDGET", "inventory 数量超预算")
    keys = [canonical_name(name) for name in names]
    check(len(set(keys)) == len(keys), "NAME_DUPLICATE", "inventory 规范名字重复")
    return names


def _intent(value):
    exact_keys(value, ("schema", "source_sha256", "inventory", "business_names", "source_doo", "expected_doo", "optional_source_equal"), "intent")
    check(value["schema"] == "mpq-append-intent/v1", "PLAN_SCHEMA", "intent schema 不支持")
    source_sha = sha_value(value["source_sha256"], "source_sha256")
    names = _inventory(value["inventory"], source_sha)
    business = value["business_names"]
    check(isinstance(business, list) and len(business) == 2, "PLAN_AUTHORITY", "本批业务允许且仅允许 DOO 和唯一 JASS")
    business = [canonical_name(name) for name in business]
    check(len(set(business)) == 2 and DOO in business and len(JASS_NAMES & set(business)) == 1,
          "PLAN_AUTHORITY", "本批业务允许且仅允许 DOO 和唯一 JASS")
    inventory_keys = {canonical_name(name) for name in names}
    check(set(business) <= inventory_keys and len(JASS_NAMES & inventory_keys) == 1
          and not ({"WAR3MAP.LUA", "SCRIPTS\\WAR3MAP.LUA"} & inventory_keys), "PLAN_AUTHORITY", "源库存不是唯一经典 JASS")
    payloads = {}
    for label in ("source_doo", "expected_doo"):
        row = value[label]
        exact_keys(row, ("bytes", "sha256"), label)
        size_value(row["bytes"], label)
        sha_value(row["sha256"], label)
    check(value["source_doo"]["sha256"] != value["expected_doo"]["sha256"], "PLAN_DOO_NOOP", "独立 DOO expected 没有净变化")
    payloads[DOO] = {**value["expected_doo"], "origin": "independent-doo-full-bytes", "required": True}
    optional = value["optional_source_equal"]
    check(isinstance(optional, list) and len(optional) <= 7, "PLAN_AUTHORITY", "可选 source-equal 仅支持七类已存在物编")
    for row in optional:
        exact_keys(row, ("name", "bytes", "sha256", "source_sha256"), "optional_source_equal")
        key = canonical_name(row["name"])
        check(key in NATIVE_NAMES and key in inventory_keys and key not in payloads
              and row["source_sha256"] == source_sha, "PLAN_AUTHORITY", "可选重写未绑定源物编，或是禁止/重复名字")
        size_value(row["bytes"], key)
        sha_value(row["sha256"], key)
        payloads[key] = {"bytes": row["bytes"], "sha256": row["sha256"],
            "origin": "product-source-decoded-identity", "required": False}
    return source_sha, names, next(iter(JASS_NAMES & set(business))), payloads


def inspect_archive(path, expected_sha256, inventory):
    names = _inventory(inventory, sha_value(expected_sha256, "expected_sha256"))
    archive = _Archive(path, expected_sha256, names)
    try:
        result = archive.summary()
        archive.unchanged()
        return {"schema": "mpq-append-inspect/v1", "passed": True, **result,
            "index_metadata_decrypted": True, "member_decrypted": False, "member_decompressed": False,
            "tiara_content_parsed": False, "inputs_unchanged": True}
    finally:
        archive.close()


def preflight_intent(path, intent):
    source_sha, names, script, payloads = _intent(intent)
    archive = _Archive(path, source_sha, names)
    try:
        check(archive.blocks[archive.lookup(DOO)][2] == intent["source_doo"]["bytes"], "PLAN_SOURCE_SIZE", "DOO 源身份 fileSize 不匹配")
        archive.lookup(script)
        for name, row in payloads.items():
            if not row["required"]:
                check(archive.blocks[archive.lookup(name)][2] == row["bytes"], "PLAN_SOURCE_SIZE", "source-equal 物编 fileSize 不匹配", name=name)
        archive.unchanged()
        return {"schema": "mpq-append-preflight/v1", "passed": True, "intent_sha256": canonical_digest(intent),
            "source": archive.summary(), "allowed_physical_names": sorted([script, *payloads]),
            "index_metadata_decrypted": True, "member_decrypted": False, "member_decompressed": False,
            "tiara_content_parsed": False, "inputs_unchanged": True}
    finally:
        archive.close()


def _compare_source_region(source, candidate):
    windows = [(source.base + 8, source.base + 12), (source.base + 20, source.base + 24)]
    cursor, compared, ranges = 0, 0, []
    for start, end in [*windows, (source.physical_size, source.physical_size)]:
        ranges.append({"offset": cursor, "bytes": start - cursor})
        while cursor < start:
            amount = min(CHUNK, start - cursor)
            before, after = source.read(cursor, amount), candidate.read(cursor, amount)
            if before != after:
                first = next(index for index, pair in enumerate(zip(before, after)) if pair[0] != pair[1])
                raise OracleError("SOURCE_REGION_CHANGED", "原完整物理区域出现非头窗口变化", physical_offset=cursor + first)
            cursor += amount
            compared += amount
        cursor = end
    return {"bytes_compared": compared, "equal_ranges": ranges,
        "excluded_header_windows": [{"offset": start, "bytes": end - start} for start, end in windows]}


def prove_append(source_path, candidate_path, plan):
    exact_keys(plan, ("schema", "intent", "intent_sha256", "candidate_sha256", "jass_witness"), "bound_plan")
    check(plan["schema"] == "mpq-append-bound-plan/v1", "PLAN_SCHEMA", "bound plan schema 不支持")
    source_sha, names, script, payloads = _intent(plan["intent"])
    check(sha_value(plan["intent_sha256"], "intent_sha256") == canonical_digest(plan["intent"]), "PLAN_INTENT_BINDING", "intent 冻结 SHA 不匹配")
    candidate_sha = sha_value(plan["candidate_sha256"], "candidate_sha256")
    witness = plan["jass_witness"]
    exact_keys(witness, ("name", "bytes", "sha256", "candidate_sha256", "origin"), "jass_witness")
    check(canonical_name(witness["name"]) == script and witness["candidate_sha256"] == candidate_sha
          and witness["origin"] in ("product-candidate-decoded-identity", "product-compiled-artifact-identity"),
          "PLAN_SCRIPT_BINDING", "JASS payload 见证未绑定固定名字/候选/来源")
    size_value(witness["bytes"], "jass_witness")
    sha_value(witness["sha256"], "jass_witness")
    payloads[script] = {"bytes": witness["bytes"], "sha256": witness["sha256"], "origin": witness["origin"], "required": True}
    check(len(payloads) <= MAX_REPLACEMENTS, "PLAN_BUDGET", "replacement 数量超预算")
    check(not os.path.samefile(source_path, candidate_path), "INPUT_IDENTITY", "source/candidate 必须不同实体")
    source, candidate = None, None
    try:
        source = _Archive(source_path, source_sha, names)
        candidate = _Archive(candidate_path, candidate_sha, names)
        check(candidate.physical_size > source.physical_size and source.base == candidate.base,
              "APPEND_IDENTITY", "候选必须保留 base 且实际追加")
        check(source.hash_count == candidate.hash_count and source.block_count == candidate.block_count,
              "INDEX_COUNT_CHANGED", "索引 count 改变")
        header_before, header_after = bytearray(source.header_bytes), bytearray(candidate.header_bytes)
        for start in (8, 20):
            header_before[start:start + 4] = b"\0" * 4
            header_after[start:start + 4] = b"\0" * 4
        check(header_before == header_after, "HEADER_CHANGE", "出现第三个 header 字段变化")
        check(source.hash_cipher == candidate.hash_cipher and source.hash_entries == candidate.hash_entries,
              "HASH_TABLE_CHANGED", "原 encrypted hash table/所有索引字段没有保持")
        check(source.named == candidate.named and source.anonymous == candidate.anonymous,
              "INDEX_IDENTITY_CHANGED", "具名/匿名 lookup 身份改变")
        check(source.blocks[source.lookup(DOO)][2] == plan["intent"]["source_doo"]["bytes"],
              "PLAN_SOURCE_SIZE", "source DOO fileSize 不匹配来源见证")
        bindings = {source.lookup(name): (name, row) for name, row in payloads.items()}
        changed = {index for index, (before, after) in enumerate(zip(source.blocks, candidate.blocks)) if before != after}
        required = {index for index, (_, row) in bindings.items() if row["required"]}
        unauthorized = changed - set(bindings)
        check(not unauthorized and required <= changed, "REPLACEMENT_SET", "存在未授权重写或遗漏必需 replacement",
              unauthorized_blocks=sorted(unauthorized), missing_required_blocks=sorted(required - changed))
        replacements, protected = [], []
        for index in sorted(source.existing - changed):
            offset, compressed, size, flags = source.blocks[index]
            before_sha = source.sha_range(source.base + offset, compressed)
            after_sha = candidate.sha_range(candidate.base + offset, compressed)
            check(before_sha == after_sha, "PROTECTED_RAW_SLICE_CHANGED", "原保护块完整 raw slice 变化", block_index=index)
            protected.append({"block_index": index, "hash_index": source.active[index],
                "hash_entry": list(source.hash_entries[source.active[index]]),
                "block_entry": [offset, compressed, size, flags], "anonymous": index in source.anonymous,
                "physical_offset": source.base + offset, "raw_bytes": compressed,
                "source_raw_sha256": before_sha, "candidate_raw_sha256": after_sha})
        for index in sorted(changed):
            name, row = bindings[index]
            offset, compressed, size, flags = candidate.blocks[index]
            if not row["required"]:
                check(source.blocks[index][2] == row["bytes"], "PLAN_SOURCE_SIZE", "source-equal 物编原 fileSize 不匹配", block_index=index)
            check(flags == EXISTS | SINGLE and compressed == size == row["bytes"],
                  "REPLACEMENT_FLAGS_SIZE", "新 replacement 必须为完整未压缩/未加密 single-unit", block_index=index)
            check(offset >= source.archive_size, "REPLACEMENT_NOT_APPENDED", "新 replacement 复用了原区域", block_index=index)
            actual_sha = candidate.sha_range(candidate.base + offset, compressed)
            check(actual_sha == row["sha256"], "REPLACEMENT_PAYLOAD", "完整 appended payload SHA 未匹配冻结见证", block_index=index)
            replacements.append({"name": name, "block_index": index, "file_pos": offset, "physical_offset": candidate.base + offset,
                "bytes": compressed, "sha256": actual_sha, "origin": row["origin"], "required": row["required"]})
        replacements.sort(key=lambda row: (row["file_pos"], row["block_index"]))
        check(sum(row["bytes"] for row in replacements) <= MAX_APPEND, "APPEND_BUDGET", "实际追加 payload 超预算")
        cursor = source.archive_size
        for row in replacements:
            check(row["file_pos"] == cursor, "APPEND_LAYOUT", "追加 replacement 不连续或重叠", block_index=row["block_index"], expected_offset=cursor)
            cursor += row["bytes"]
            check(cursor <= U32, "APPEND_OVERFLOW", "追加超过 classic U32 范围")
        check(candidate.block_pos == cursor and cursor + candidate.block_size <= U32
              and candidate.archive_size == cursor + candidate.block_size,
              "NEW_BLOCK_TABLE_EOF", "最后 replacement/新块表/声明EOF未精确连接")
        region = _compare_source_region(source, candidate)
        source.unchanged()
        candidate.unchanged()
        return {"schema": "mpq-append-proof/v1", "passed": True, "oracle_file_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "intent_sha256": plan["intent_sha256"], "bound_plan_sha256": canonical_digest(plan),
            "source": source.summary(), "candidate": candidate.summary(), "original_region": region,
            "protected_blocks": protected, "unmodified_block_metadata_exact": True,
            "unchanged_nonexistent_block_count": len(source.blocks) - len(source.existing),
            "actual_replacements": replacements, "allowed_physical_names": sorted(payloads),
            "business_change_names": sorted([DOO, script]), "new_block_table_eof_exact": True,
            "index_metadata_decrypted": True, "member_decrypted": False, "member_decompressed": False,
            "tiara_content_parsed": False, "inputs_unchanged": True,
            "source_equal_witness_boundary": "source decoded size/SHA supplied by product read chain; original members not decoded by oracle",
            "jass_witness_boundary": "product compiled/candidate identity bound to appended bytes; script semantics not independently verified",
            "native_editor_verified": False, "game_verified": False}
    finally:
        if candidate is not None:
            candidate.close()
        if source is not None:
            source.close()


def _write_receipt(path, result, inputs):
    output = Path(path).resolve()
    check(not output.exists() and all(output != Path(value).resolve(strict=True) for value in inputs),
          "RECEIPT_PATH", "receipt 必须是不同的新文件")
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def _read_json_path(path):
    with Path(path).open("rb") as stream:
        data = stream.read(MAX_JSON_BYTES + 1)
    check(len(data) <= MAX_JSON_BYTES, "JSON_BUDGET", "JSON 文件字节超预算")
    return strict_json(data.decode("utf-8", errors="strict"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    inspect = commands.add_parser("inspect")
    inspect.add_argument("--archive", required=True, type=Path)
    inspect.add_argument("--expected-sha256", required=True)
    inspect.add_argument("--inventory", required=True, type=Path)
    proof = commands.add_parser("prove")
    proof.add_argument("--source", required=True, type=Path)
    proof.add_argument("--candidate", required=True, type=Path)
    proof.add_argument("--plan", required=True, type=Path)
    for command in (inspect, proof):
        command.add_argument("--receipt", required=True, type=Path)
    args = parser.parse_args()
    inputs = [args.archive, args.inventory] if args.mode == "inspect" else [args.source, args.candidate, args.plan]
    result = {"schema": "mpq-append-failure/v1", "passed": False, "member_decrypted": False, "member_decompressed": False,
        "tiara_content_parsed": False, "native_editor_verified": False, "game_verified": False}
    try:
        if args.mode == "inspect":
            result = inspect_archive(args.archive, args.expected_sha256, _read_json_path(args.inventory))
        else:
            result = prove_append(args.source, args.candidate, _read_json_path(args.plan))
    except OracleError as error:
        result["error"] = error.receipt()
    except (OSError, ValueError, TypeError, KeyError, UnicodeError) as error:
        result["error"] = {"code": "INPUT_UNREADABLE", "message": str(error)}
    try:
        _write_receipt(args.receipt, result, inputs)
    except (OracleError, OSError) as error:
        print(json.dumps({"passed": False, "receipt_written": False, "error": str(error)}, ensure_ascii=False))
        return 2
    print(json.dumps({"passed": result["passed"], "receipt": str(args.receipt.resolve()),
        "error_code": result.get("error", {}).get("code")}, ensure_ascii=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
