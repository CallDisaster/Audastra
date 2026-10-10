#!/usr/bin/env python3
"""Generate the static WarVK JASS and editor hand-off bundle from one manifest."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Iterable, Sequence


MINIMUM_PYTHON = (3, 10)
GENERATOR_VERSION = "1.0.0"
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
SHA256_RE = re.compile(r"[0-9A-Fa-f]{64}\Z")
HEX_VALUE_RE = re.compile(r"0x[0-9A-Fa-f]{1,16}\Z")
HEX_BYTES_RE = re.compile(r"[0-9A-Fa-f]{2}(?: [0-9A-Fa-f]{2})*\Z")
JASS_NAME_RE = re.compile(r"WarVK[A-Za-z0-9]+\Z")
COMMAND_NAME_RE = re.compile(r"[a-z][A-Za-z0-9]*(?:\.[a-z][A-Za-z0-9]*)+\Z")
ARGUMENT_NAME_RE = re.compile(r"[A-Za-z][A-Za-z0-9]*\Z")
PLACEHOLDER_RE = re.compile(r"\$\{([A-Za-z][A-Za-z0-9]*)\}")

INPUT_TYPES: dict[str, tuple[str, str, str]] = {
    "bool": ("boolean", "b", "WarVKEncodeBool"),
    "i32": ("integer", "i", "WarVKEncodeInteger"),
    "id": ("integer", "d", "WarVKEncodeId"),
    "real": ("real", "r", "WarVKEncodeReal"),
}
RESULT_TYPES: dict[str, str] = {
    "void": "nothing",
    "bool": "boolean",
    "i32": "integer",
    "id": "integer",
    "real": "real",
    "string": "string",
}
RESULTS_BY_CARRIER = {
    "preloader": frozenset({"void"}),
    "hotkey": frozenset({"bool", "i32", "id"}),
    "localized_string": frozenset({"real", "string"}),
}
STOCK_NATIVE_BY_CARRIER = {
    "preloader": "Preloader",
    "hotkey": "GetLocalizedHotkey",
    "localized_string": "GetLocalizedString",
}
SAMPLE_ARGUMENTS = {
    "bool": "b:1",
    "i32": "i:-17",
    "id": "d:7",
    "real": "r:1.25",
}
HELPER_NAMES = frozenset(value[2] for value in INPUT_TYPES.values())
HELPER_NAMES_CASEFOLD = frozenset(value.casefold() for value in HELPER_NAMES)
JASS_RESERVED = frozenset({
    "and", "array", "call", "constant", "debug", "else", "elseif", "endfunction",
    "endglobals", "endif", "endloop", "exitwhen", "extends", "false",
    "function", "globals", "if", "local", "loop", "native", "not",
    "nothing", "null", "or", "return", "returns", "set", "takes", "then",
    "true", "type", "boolean", "integer", "real", "string", "code", "handle",
})

OUTPUT_PATHS = (
    "jass/WarVK.j",
    "catalog/mapui/ui/config",
    "catalog/mapui/ui/warvk/define.txt",
    "catalog/mapui/ui/warvk/action.txt",
    "catalog/mapui/ui/warvk/call.txt",
    "catalog/legacy/config",
    "catalog/legacy/warvk/ui/TriggerData.txt",
    "catalog/legacy/warvk/ui/TriggerStrings.txt",
    "protocol/warvk-v1-vectors.json",
    "warvk-handoff.json",
)

TOP_LEVEL_KEYS = frozenset({
    "schema", "apiVersion", "protocolVersion", "provenance", "distribution",
    "protocol", "publicApiPolicy", "featureStatus", "categories", "errorCodes",
    "gameDllProfile", "commands",
})
PROVENANCE_KEYS = frozenset({
    "method", "allowedEvidence", "restrictedImplementationConsulted",
    "runtimeEffectClaimed",
})
DISTRIBUTION_KEYS = frozenset({
    "status", "reason", "deploymentAuthorized", "mapArchiveIntegrationAuthorized",
})
PROTOCOL_KEYS = frozenset({
    "prefix", "canonicalPrefix", "canonicalGrammar", "legacyGrammar",
    "legacyParseOnly", "separator", "typeSeparator", "encoding", "maxBytes",
    "maxArgs", "integerPolicy", "realPolicy", "nonWarvkPolicy",
    "profileMismatchPolicy", "typedArgs", "carriers",
})
PUBLIC_POLICY_KEYS = frozenset({
    "jassNamePattern", "allowedInputTypes", "allowedResultTypes",
    "prohibitedPublicTypes", "localVisualWarningZh", "managedIdPolicy",
})
CATEGORY_KEYS = frozenset({"name", "status", "featureBit"})
COMMAND_KEYS = frozenset({
    "id", "name", "jassName", "category", "carrier", "args", "result",
    "backendRequired", "localVisual", "querySyncWarning", "titleZh",
    "descriptionZh",
})
ARGUMENT_KEYS = frozenset({"name", "type"})
PROHIBITED_PUBLIC_TYPES = frozenset({
    "raw_pointer", "address", "vulkan_handle", "framebuffer", "native_callback",
    "native_event", "c_ui_callback", "raw_image_handle", "war3_handle",
})
TYPED_ARGUMENT_GRAMMARS = {
    "b": ("bool", "b:0|b:1"),
    "i": ("i32", "i:<strict-int32-decimal>"),
    "d": ("id", "d:<positive-int32-decimal>"),
    "r": ("real", "r:<finite-decimal>"),
}
CARRIER_CONTRACTS = {
    "preloader": ("Preloader", "(S)V", "setter_or_command"),
    "hotkey": ("GetLocalizedHotkey", "(S)I", "bool_i32_or_id_return"),
    "localized_string": (
        "GetLocalizedString", "(S)S", "string_or_textual_real_return"),
}
GAME_PROFILE_KEYS = frozenset({
    "profileId", "identity", "verificationPolicy", "registrationTargetRva",
    "carriers", "helpers", "registrationEvidence",
})
GAME_IDENTITY_KEYS = frozenset({
    "sha256", "fileVersion", "productVersion", "fileLength", "machine",
    "optionalHeaderMagic", "timeDateStamp", "sizeOfImage", "sectionCount",
    "entryPointRva", "preferredImageBase", "checksum",
})
GAME_CARRIER_KEYS = frozenset({
    "name", "implementationRva", "implementationSize", "registrationRva",
    "nameRva", "signatureRva", "abi", "prologueBytes", "prologueMask",
    "relocations",
})
GAME_HELPER_KEYS = frozenset({
    "name", "rva", "size", "bytes", "mask", "relocations",
})
GAME_RELOCATION_KEYS = frozenset({"offset", "targetRva"})
GAME_REGISTRATION_EVIDENCE_KEYS = frozenset({"addressing", "layout", "scanAllowed"})


class CodegenError(ValueError):
    """A stable, user-actionable fail-closed generation error."""


@dataclasses.dataclass(frozen=True)
class Argument:
    name: str
    manifest_type: str

    @property
    def jass_type(self) -> str:
        return INPUT_TYPES[self.manifest_type][0]

    @property
    def protocol_tag(self) -> str:
        return INPUT_TYPES[self.manifest_type][1]

    @property
    def encoder(self) -> str:
        return INPUT_TYPES[self.manifest_type][2]


@dataclasses.dataclass(frozen=True)
class Command:
    command_id: int
    name: str
    jass_name: str
    category: str
    carrier: str
    arguments: tuple[Argument, ...]
    result: str
    backend_required: bool
    title_zh: str
    description_zh: str


@dataclasses.dataclass(frozen=True)
class ManifestModel:
    api_version: str
    protocol_version: int
    canonical_prefix: str
    legacy_prefix: str
    max_bytes: int
    max_args: int
    local_warning_zh: str
    distribution_status: str
    deployment_authorized: bool
    map_archive_integration_authorized: bool
    categories: tuple[str, ...]
    commands: tuple[Command, ...]


def _duplicate_rejecting_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        try:
            key.encode("utf-8", errors="strict")
        except UnicodeEncodeError as error:
            raise CodegenError("manifest contains invalid Unicode in a JSON key") from error
        if key in result:
            raise CodegenError(f"manifest contains duplicate JSON key: {key}")
        result[key] = value
    return result


def _expect_object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CodegenError(f"{path} must be an object")
    return value


def _expect_array(value: Any, path: str) -> list[Any]:
    if not isinstance(value, list):
        raise CodegenError(f"{path} must be an array")
    return value


def _expect_string(value: Any, path: str, *, nonempty: bool = True) -> str:
    if not isinstance(value, str) or (nonempty and not value):
        raise CodegenError(f"{path} must be a{' non-empty' if nonempty else ''} string")
    if any(
            ord(character) < 0x20 or 0x7F <= ord(character) <= 0x9F or
            ord(character) in {0x2028, 0x2029}
            for character in value):
        raise CodegenError(f"{path} contains a control character")
    try:
        value.encode("utf-8", errors="strict")
    except UnicodeEncodeError as error:
        raise CodegenError(f"{path} contains invalid Unicode") from error
    return value


def _expect_bool(value: Any, path: str) -> bool:
    if type(value) is not bool:
        raise CodegenError(f"{path} must be a boolean")
    return value


def _expect_int(value: Any, path: str) -> int:
    if type(value) is not int:
        raise CodegenError(f"{path} must be an integer")
    return value


def _expect_keys(mapping: dict[str, Any], expected: frozenset[str], path: str) -> None:
    actual = frozenset(mapping)
    missing = sorted(expected - actual)
    unknown = sorted(actual - expected)
    if missing or unknown:
        details: list[str] = []
        if missing:
            details.append("missing=" + ",".join(missing))
        if unknown:
            details.append("unknown=" + ",".join(unknown))
        raise CodegenError(f"{path} fields do not match schema ({'; '.join(details)})")


def _expect_bounded_string(value: Any, path: str, maximum: int) -> str:
    result = _expect_string(value, path)
    if len(result) > maximum:
        raise CodegenError(f"{path} exceeds the {maximum}-character safety limit")
    return result


def _expect_string_set(value: Any, path: str) -> set[str]:
    values = _expect_array(value, path)
    if any(not isinstance(item, str) or not item for item in values):
        raise CodegenError(f"{path} must contain non-empty strings")
    result = set(values)
    if len(result) != len(values):
        raise CodegenError(f"{path} must not contain duplicates")
    return result


def _required(mapping: dict[str, Any], key: str, path: str) -> Any:
    if key not in mapping:
        raise CodegenError(f"{path}.{key} is required")
    return mapping[key]


def _reject_json_constant(value: str) -> None:
    raise CodegenError(f"manifest contains non-standard JSON constant: {value}")


def _bounded_json_int(value: str) -> int:
    if len(value) > 64:
        raise CodegenError("manifest contains an overlong JSON integer token")
    return int(value, 10)


def _reject_json_float(_value: str) -> None:
    raise CodegenError("manifest schema does not permit JSON floating-point literals")


def _normalize_sha256(value: str, option: str) -> str:
    if not SHA256_RE.fullmatch(value):
        raise CodegenError(f"{option} must be exactly 64 hexadecimal characters")
    return value.lower()


def _expect_hex_value(value: Any, path: str) -> str:
    result = _expect_bounded_string(value, path, 32)
    if not HEX_VALUE_RE.fullmatch(result):
        raise CodegenError(f"{path} must be a hexadecimal 0x value")
    return result


def _expect_hex_bytes(value: Any, path: str) -> int:
    result = _expect_bounded_string(value, path, 4096)
    if not HEX_BYTES_RE.fullmatch(result):
        raise CodegenError(f"{path} must be space-separated hexadecimal bytes")
    return len(result.split(" "))


def _validate_profile_relocations(value: Any, path: str, byte_count: int) -> None:
    relocations = _expect_array(value, path)
    if len(relocations) > 64:
        raise CodegenError(f"{path} exceeds the 64-entry safety limit")
    offsets: set[int] = set()
    for index, raw_relocation in enumerate(relocations):
        relocation_path = f"{path}[{index}]"
        relocation = _expect_object(raw_relocation, relocation_path)
        _expect_keys(relocation, GAME_RELOCATION_KEYS, relocation_path)
        offset = _expect_int(
            _required(relocation, "offset", relocation_path),
            f"{relocation_path}.offset")
        _expect_hex_value(
            _required(relocation, "targetRva", relocation_path),
            f"{relocation_path}.targetRva")
        if offset < 0 or offset + 4 > byte_count or offset in offsets:
            raise CodegenError(f"{relocation_path}.offset is duplicated or outside byte evidence")
        offsets.add(offset)


def _validate_ascii(value: str, path: str) -> None:
    try:
        value.encode("ascii")
    except UnicodeEncodeError as error:
        raise CodegenError(f"{path} must be ASCII") from error


def _validate_placeholders(description: str, argument_names: Sequence[str], path: str) -> None:
    placeholders = PLACEHOLDER_RE.findall(description)
    if "${" in PLACEHOLDER_RE.sub("", description):
        raise CodegenError(f"{path} contains a malformed placeholder")
    if placeholders != list(argument_names):
        raise CodegenError(
            f"{path} placeholders must occur exactly once in argument order: "
            f"{list(argument_names)!r}")


def _parse_manifest(document: dict[str, Any]) -> ManifestModel:
    _expect_keys(document, TOP_LEVEL_KEYS, "manifest")
    schema = _expect_object(_required(document, "schema", "manifest"), "manifest.schema")
    _expect_keys(schema, frozenset({"name", "version"}), "manifest.schema")
    if _expect_string(_required(schema, "name", "manifest.schema"), "manifest.schema.name") != \
            "warvk-japi-manifest":
        raise CodegenError("manifest.schema.name is unsupported")
    if _expect_int(_required(schema, "version", "manifest.schema"), "manifest.schema.version") != 1:
        raise CodegenError("manifest.schema.version is unsupported")

    api_version = _expect_string(
        _required(document, "apiVersion", "manifest"), "manifest.apiVersion")
    protocol_version = _expect_int(
        _required(document, "protocolVersion", "manifest"), "manifest.protocolVersion")
    if protocol_version != 1:
        raise CodegenError("only protocolVersion 1 can be generated")

    provenance = _expect_object(
        _required(document, "provenance", "manifest"), "manifest.provenance")
    _expect_keys(provenance, PROVENANCE_KEYS, "manifest.provenance")
    if _expect_string(
            _required(provenance, "method", "manifest.provenance"),
            "manifest.provenance.method") != "clean-room":
        raise CodegenError("manifest provenance method must be clean-room")
    allowed_evidence = _expect_array(
        _required(provenance, "allowedEvidence", "manifest.provenance"),
        "manifest.provenance.allowedEvidence")
    if not allowed_evidence or len(allowed_evidence) > 64:
        raise CodegenError(
            "manifest.provenance.allowedEvidence must contain between 1 and 64 entries")
    normalized_evidence = [
        _expect_bounded_string(
            value, f"manifest.provenance.allowedEvidence[{index}]", 1024)
        for index, value in enumerate(allowed_evidence)
    ]
    if len(set(normalized_evidence)) != len(normalized_evidence):
        raise CodegenError("manifest.provenance.allowedEvidence must not contain duplicates")
    if _expect_bool(
            _required(provenance, "restrictedImplementationConsulted", "manifest.provenance"),
            "manifest.provenance.restrictedImplementationConsulted"):
        raise CodegenError("manifest provenance is not eligible for clean-room generation")
    if _expect_bool(
            _required(provenance, "runtimeEffectClaimed", "manifest.provenance"),
            "manifest.provenance.runtimeEffectClaimed"):
        raise CodegenError("static codegen refuses a manifest that claims runtime effects")

    distribution = _expect_object(
        _required(document, "distribution", "manifest"), "manifest.distribution")
    _expect_keys(distribution, DISTRIBUTION_KEYS, "manifest.distribution")
    distribution_status = _expect_string(
        _required(distribution, "status", "manifest.distribution"),
        "manifest.distribution.status")
    deployment_authorized = _expect_bool(
        _required(distribution, "deploymentAuthorized", "manifest.distribution"),
        "manifest.distribution.deploymentAuthorized")
    map_archive_integration_authorized = _expect_bool(
        _required(distribution, "mapArchiveIntegrationAuthorized", "manifest.distribution"),
        "manifest.distribution.mapArchiveIntegrationAuthorized")
    _expect_bounded_string(
        _required(distribution, "reason", "manifest.distribution"),
        "manifest.distribution.reason", 1024)

    protocol = _expect_object(_required(document, "protocol", "manifest"), "manifest.protocol")
    _expect_keys(protocol, PROTOCOL_KEYS, "manifest.protocol")
    legacy_prefix = _expect_string(
        _required(protocol, "prefix", "manifest.protocol"), "manifest.protocol.prefix")
    canonical_prefix = _expect_string(
        _required(protocol, "canonicalPrefix", "manifest.protocol"),
        "manifest.protocol.canonicalPrefix")
    if legacy_prefix != "warvk:" or canonical_prefix != "warvk:v1;":
        raise CodegenError("protocol prefixes do not describe the fixed warvk:v1 contract")
    if _expect_string(
            _required(protocol, "canonicalGrammar", "manifest.protocol"),
            "manifest.protocol.canonicalGrammar") != \
            "warvk:v1;<command>[;<typed-arg>...]":
        raise CodegenError("canonical protocol grammar is not the fixed v1 grammar")
    if _expect_string(
            _required(protocol, "legacyGrammar", "manifest.protocol"),
            "manifest.protocol.legacyGrammar") != \
            "warvk:<command>[;<typed-arg>...]":
        raise CodegenError("legacy protocol grammar is not the parse-only grammar")
    if not _expect_bool(
            _required(protocol, "legacyParseOnly", "manifest.protocol"),
            "manifest.protocol.legacyParseOnly"):
        raise CodegenError("the unversioned protocol must be parse-only")
    if _expect_string(_required(protocol, "separator", "manifest.protocol"),
                      "manifest.protocol.separator") != ";":
        raise CodegenError("protocol separator must be ';'")
    if _expect_string(_required(protocol, "typeSeparator", "manifest.protocol"),
                      "manifest.protocol.typeSeparator") != ":":
        raise CodegenError("protocol type separator must be ':'")
    if _expect_string(_required(protocol, "encoding", "manifest.protocol"),
                      "manifest.protocol.encoding") != "ASCII":
        raise CodegenError("protocol encoding must be ASCII")
    max_bytes = _expect_int(
        _required(protocol, "maxBytes", "manifest.protocol"), "manifest.protocol.maxBytes")
    max_args = _expect_int(
        _required(protocol, "maxArgs", "manifest.protocol"), "manifest.protocol.maxArgs")
    if max_bytes != 512:
        raise CodegenError("manifest.protocol.maxBytes must be the fixed 512-byte limit")
    if max_args != 16:
        raise CodegenError("manifest.protocol.maxArgs must be the fixed 16-argument limit")
    if _expect_string(
            _required(protocol, "integerPolicy", "manifest.protocol"),
            "manifest.protocol.integerPolicy") != \
            "strict signed int32 decimal; no whitespace, sign-only token, or overflow":
        raise CodegenError("integer policy is not the strict int32 contract")
    if _expect_string(
            _required(protocol, "realPolicy", "manifest.protocol"),
            "manifest.protocol.realPolicy") != \
            ("strict decimal, optional exponent, finite and representable; reject NaN, Inf, "
             "whitespace, and overflow"):
        raise CodegenError("real policy is not the finite decimal contract")
    if _expect_string(
            _required(protocol, "nonWarvkPolicy", "manifest.protocol"),
            "manifest.protocol.nonWarvkPolicy") != "forward_original_unconditionally":
        raise CodegenError("non-WarVK calls must unconditionally forward to the original native")
    if _expect_string(
            _required(protocol, "profileMismatchPolicy", "manifest.protocol"),
            "manifest.protocol.profileMismatchPolicy") != "fail_closed_no_address_scan":
        raise CodegenError("profile mismatch policy must fail closed without address scanning")

    typed_args = _expect_object(
        _required(protocol, "typedArgs", "manifest.protocol"), "manifest.protocol.typedArgs")
    _expect_keys(typed_args, frozenset(TYPED_ARGUMENT_GRAMMARS),
                 "manifest.protocol.typedArgs")
    for tag, (manifest_type, grammar) in TYPED_ARGUMENT_GRAMMARS.items():
        entry = _expect_object(
            _required(typed_args, tag, "manifest.protocol.typedArgs"),
            f"manifest.protocol.typedArgs.{tag}")
        _expect_keys(entry, frozenset({"type", "grammar"}),
                     f"manifest.protocol.typedArgs.{tag}")
        if _expect_string(_required(entry, "type", f"manifest.protocol.typedArgs.{tag}"),
                          f"manifest.protocol.typedArgs.{tag}.type") != manifest_type:
            raise CodegenError(f"typed argument tag {tag!r} has the wrong manifest type")
        if _expect_string(_required(entry, "grammar", f"manifest.protocol.typedArgs.{tag}"),
                          f"manifest.protocol.typedArgs.{tag}.grammar") != grammar:
            raise CodegenError(f"typed argument tag {tag!r} has the wrong grammar")

    carriers = _expect_object(
        _required(protocol, "carriers", "manifest.protocol"), "manifest.protocol.carriers")
    _expect_keys(carriers, frozenset(CARRIER_CONTRACTS), "manifest.protocol.carriers")
    for carrier, (stock_native, abi, role) in CARRIER_CONTRACTS.items():
        entry = _expect_object(
            _required(carriers, carrier, "manifest.protocol.carriers"),
            f"manifest.protocol.carriers.{carrier}")
        _expect_keys(entry, frozenset({"stockNative", "abi", "role"}),
                     f"manifest.protocol.carriers.{carrier}")
        if _expect_string(_required(entry, "stockNative", f"manifest.protocol.carriers.{carrier}"),
                          f"manifest.protocol.carriers.{carrier}.stockNative") != stock_native:
            raise CodegenError(f"carrier {carrier!r} does not use its required stock native")
        if _expect_string(_required(entry, "abi", f"manifest.protocol.carriers.{carrier}"),
                          f"manifest.protocol.carriers.{carrier}.abi") != abi:
            raise CodegenError(f"carrier {carrier!r} does not use its required stock ABI")
        if _expect_string(_required(entry, "role", f"manifest.protocol.carriers.{carrier}"),
                          f"manifest.protocol.carriers.{carrier}.role") != role:
            raise CodegenError(f"carrier {carrier!r} does not use its required role")

    policy = _expect_object(
        _required(document, "publicApiPolicy", "manifest"), "manifest.publicApiPolicy")
    _expect_keys(policy, PUBLIC_POLICY_KEYS, "manifest.publicApiPolicy")
    if _expect_string(_required(policy, "jassNamePattern", "manifest.publicApiPolicy"),
                      "manifest.publicApiPolicy.jassNamePattern") != "^WarVK[A-Za-z0-9]+$":
        raise CodegenError("public JASS name policy is not the fixed WarVK policy")
    allowed_inputs = _expect_string_set(
        _required(policy, "allowedInputTypes", "manifest.publicApiPolicy"),
        "manifest.publicApiPolicy.allowedInputTypes")
    if set(allowed_inputs) != set(INPUT_TYPES):
        raise CodegenError("allowed input types do not match the safe public type set")
    allowed_results = _expect_string_set(
        _required(policy, "allowedResultTypes", "manifest.publicApiPolicy"),
        "manifest.publicApiPolicy.allowedResultTypes")
    if set(allowed_results) != set(RESULT_TYPES):
        raise CodegenError("allowed result types do not match the safe public result set")
    prohibited_types = _expect_string_set(
        _required(policy, "prohibitedPublicTypes", "manifest.publicApiPolicy"),
        "manifest.publicApiPolicy.prohibitedPublicTypes")
    if set(prohibited_types) != PROHIBITED_PUBLIC_TYPES:
        raise CodegenError("prohibited public types do not match the fixed safe boundary")
    managed_id_policy = _expect_string(
        _required(policy, "managedIdPolicy", "manifest.publicApiPolicy"),
        "manifest.publicApiPolicy.managedIdPolicy")
    if "positive" not in managed_id_policy.casefold() or \
            "handles and addresses are forbidden" not in managed_id_policy.casefold():
        raise CodegenError("managed id policy must forbid Warcraft handles and addresses")
    local_warning = _expect_string(
        _required(policy, "localVisualWarningZh", "manifest.publicApiPolicy"),
        "manifest.publicApiPolicy.localVisualWarningZh")

    _expect_string(_required(document, "featureStatus", "manifest"),
                   "manifest.featureStatus")
    error_codes = _expect_array(
        _required(document, "errorCodes", "manifest"), "manifest.errorCodes")
    if not error_codes or len(error_codes) > 256:
        raise CodegenError("manifest.errorCodes must contain between 1 and 256 entries")
    seen_error_values: set[int] = set()
    seen_error_names: set[str] = set()
    for index, raw_error in enumerate(error_codes):
        error_path = f"manifest.errorCodes[{index}]"
        entry = _expect_object(raw_error, error_path)
        _expect_keys(entry, frozenset({"value", "name", "stableText"}), error_path)
        value = _expect_int(_required(entry, "value", error_path), f"{error_path}.value")
        name = _expect_bounded_string(
            _required(entry, "name", error_path), f"{error_path}.name", 128)
        _expect_bounded_string(
            _required(entry, "stableText", error_path), f"{error_path}.stableText", 1024)
        if value < 0 or value > 0x7FFFFFFF or value in seen_error_values or \
                name.casefold() in seen_error_names:
            raise CodegenError(f"{error_path} is duplicated or outside non-negative int32")
        seen_error_values.add(value)
        seen_error_names.add(name.casefold())
    if seen_error_values != set(range(len(error_codes))):
        raise CodegenError("manifest.errorCodes values must be contiguous from zero")

    game_profile = _expect_object(
        _required(document, "gameDllProfile", "manifest"), "manifest.gameDllProfile")
    _expect_keys(game_profile, GAME_PROFILE_KEYS, "manifest.gameDllProfile")
    _expect_bounded_string(
        _required(game_profile, "profileId", "manifest.gameDllProfile"),
        "manifest.gameDllProfile.profileId", 256)
    identity = _expect_object(
        _required(game_profile, "identity", "manifest.gameDllProfile"),
        "manifest.gameDllProfile.identity")
    _expect_keys(identity, GAME_IDENTITY_KEYS, "manifest.gameDllProfile.identity")
    _normalize_sha256(
        _expect_string(
            _required(identity, "sha256", "manifest.gameDllProfile.identity"),
            "manifest.gameDllProfile.identity.sha256"),
        "manifest.gameDllProfile.identity.sha256")
    for field in ("fileVersion", "productVersion"):
        _expect_bounded_string(
            _required(identity, field, "manifest.gameDllProfile.identity"),
            f"manifest.gameDllProfile.identity.{field}", 256)
    file_length = _expect_int(
        _required(identity, "fileLength", "manifest.gameDllProfile.identity"),
        "manifest.gameDllProfile.identity.fileLength")
    section_count = _expect_int(
        _required(identity, "sectionCount", "manifest.gameDllProfile.identity"),
        "manifest.gameDllProfile.identity.sectionCount")
    if file_length <= 0 or file_length > (1 << 40) or not 1 <= section_count <= 96:
        raise CodegenError("Game.dll identity numeric bounds are invalid")
    for field in (
            "machine", "optionalHeaderMagic", "timeDateStamp", "sizeOfImage",
            "entryPointRva", "preferredImageBase", "checksum"):
        _expect_hex_value(
            _required(identity, field, "manifest.gameDllProfile.identity"),
            f"manifest.gameDllProfile.identity.{field}")
    _expect_bounded_string(
        _required(game_profile, "verificationPolicy", "manifest.gameDllProfile"),
        "manifest.gameDllProfile.verificationPolicy", 2048)
    _expect_hex_value(
        _required(game_profile, "registrationTargetRva", "manifest.gameDllProfile"),
        "manifest.gameDllProfile.registrationTargetRva")
    registration_evidence = _expect_object(
        _required(game_profile, "registrationEvidence", "manifest.gameDllProfile"),
        "manifest.gameDllProfile.registrationEvidence")
    _expect_keys(
        registration_evidence, GAME_REGISTRATION_EVIDENCE_KEYS,
        "manifest.gameDllProfile.registrationEvidence")
    for field in ("addressing", "layout"):
        _expect_bounded_string(
            _required(registration_evidence, field,
                      "manifest.gameDllProfile.registrationEvidence"),
            f"manifest.gameDllProfile.registrationEvidence.{field}", 2048)
    if _expect_bool(
            _required(registration_evidence, "scanAllowed",
                      "manifest.gameDllProfile.registrationEvidence"),
            "manifest.gameDllProfile.registrationEvidence.scanAllowed"):
        raise CodegenError("Game.dll profile must forbid replacement address scanning")
    profile_carriers = _expect_array(
        _required(game_profile, "carriers", "manifest.gameDllProfile"),
        "manifest.gameDllProfile.carriers")
    if len(profile_carriers) != len(CARRIER_CONTRACTS):
        raise CodegenError("Game.dll profile must contain exactly the three stock carriers")
    profile_abis: dict[str, str] = {}
    for index, raw_carrier in enumerate(profile_carriers):
        carrier_path = f"manifest.gameDllProfile.carriers[{index}]"
        entry = _expect_object(raw_carrier, carrier_path)
        _expect_keys(entry, GAME_CARRIER_KEYS, carrier_path)
        name = _expect_string(_required(entry, "name", carrier_path), f"{carrier_path}.name")
        abi = _expect_string(_required(entry, "abi", carrier_path), f"{carrier_path}.abi")
        for field in (
                "implementationRva", "registrationRva", "nameRva", "signatureRva"):
            _expect_hex_value(_required(entry, field, carrier_path), f"{carrier_path}.{field}")
        implementation_size = int(_expect_hex_value(
            _required(entry, "implementationSize", carrier_path),
            f"{carrier_path}.implementationSize"), 16)
        prologue_bytes = _expect_hex_bytes(
            _required(entry, "prologueBytes", carrier_path),
            f"{carrier_path}.prologueBytes")
        if implementation_size < prologue_bytes:
            raise CodegenError(
                f"{carrier_path}.implementationSize is smaller than its prologue evidence")
        prologue_mask_text = _expect_string(
            _required(entry, "prologueMask", carrier_path),
            f"{carrier_path}.prologueMask")
        prologue_mask = _expect_hex_bytes(prologue_mask_text, f"{carrier_path}.prologueMask")
        if prologue_mask != prologue_bytes or any(
                token not in {"00", "FF"} for token in prologue_mask_text.upper().split(" ")):
            raise CodegenError(f"{carrier_path}.prologueMask does not match byte evidence")
        _validate_profile_relocations(
            _required(entry, "relocations", carrier_path),
            f"{carrier_path}.relocations", prologue_bytes)
        if name in profile_abis:
            raise CodegenError(f"{carrier_path}.name is duplicated")
        profile_abis[name] = abi
    expected_profile_abis = {
        stock_native: abi for stock_native, abi, _ in CARRIER_CONTRACTS.values()
    }
    if profile_abis != expected_profile_abis:
        raise CodegenError("Game.dll carrier ABI evidence does not match the stock carrier contract")
    profile_helpers = _expect_array(
        _required(game_profile, "helpers", "manifest.gameDllProfile"),
        "manifest.gameDllProfile.helpers")
    if len(profile_helpers) > 64:
        raise CodegenError("manifest.gameDllProfile.helpers exceeds the 64-entry safety limit")
    helper_names: set[str] = set()
    for index, raw_helper in enumerate(profile_helpers):
        helper_path = f"manifest.gameDllProfile.helpers[{index}]"
        helper = _expect_object(raw_helper, helper_path)
        _expect_keys(helper, GAME_HELPER_KEYS, helper_path)
        name = _expect_bounded_string(
            _required(helper, "name", helper_path), f"{helper_path}.name", 128)
        if not ARGUMENT_NAME_RE.fullmatch(name) or name.casefold() in helper_names:
            raise CodegenError(f"{helper_path}.name is unsafe or duplicated")
        helper_names.add(name.casefold())
        _expect_hex_value(_required(helper, "rva", helper_path), f"{helper_path}.rva")
        size_text = _expect_hex_value(
            _required(helper, "size", helper_path), f"{helper_path}.size")
        byte_count = _expect_hex_bytes(
            _required(helper, "bytes", helper_path), f"{helper_path}.bytes")
        mask_text = _expect_string(
            _required(helper, "mask", helper_path), f"{helper_path}.mask")
        mask_count = _expect_hex_bytes(mask_text, f"{helper_path}.mask")
        if byte_count != int(size_text, 16) or mask_count != byte_count or any(
                token not in {"00", "FF"} for token in mask_text.upper().split(" ")):
            raise CodegenError(f"{helper_path} byte/mask evidence is inconsistent")
        _validate_profile_relocations(
            _required(helper, "relocations", helper_path),
            f"{helper_path}.relocations", byte_count)

    categories_raw = _expect_array(
        _required(document, "categories", "manifest"), "manifest.categories")
    if not categories_raw or len(categories_raw) > 256:
        raise CodegenError("manifest.categories must contain between 1 and 256 categories")
    categories: set[str] = set()
    category_names_casefold: set[str] = set()
    category_order: list[str] = []
    generated_category_ids: set[str] = set()
    feature_bits: set[int] = set()
    for index, raw_category in enumerate(categories_raw):
        category = _expect_object(raw_category, f"manifest.categories[{index}]")
        _expect_keys(category, CATEGORY_KEYS, f"manifest.categories[{index}]")
        name = _expect_bounded_string(
            _required(category, "name", f"manifest.categories[{index}]"),
            f"manifest.categories[{index}].name", 64)
        if not ARGUMENT_NAME_RE.fullmatch(name):
            raise CodegenError(f"manifest.categories[{index}].name is not a safe identifier")
        category_status = _expect_bounded_string(
            _required(category, "status", f"manifest.categories[{index}]"),
            f"manifest.categories[{index}].status", 128)
        if category_status not in {"internal_dispatch", "declared_backend_required"}:
            raise CodegenError(f"manifest.categories[{index}].status is unsupported")
        feature_bit = _expect_int(
            _required(category, "featureBit", f"manifest.categories[{index}]"),
            f"manifest.categories[{index}].featureBit")
        if feature_bit < -1 or feature_bit > 30:
            raise CodegenError(
                f"manifest.categories[{index}].featureBit is outside the safe range")
        if feature_bit >= 0 and feature_bit in feature_bits:
            raise CodegenError(f"manifest.categories[{index}].featureBit is duplicated")
        if feature_bit >= 0:
            feature_bits.add(feature_bit)
        generated_category_id = "TC_WARVK_" + name.upper()
        if name.casefold() in category_names_casefold:
            raise CodegenError(f"duplicate category name: {name}")
        if generated_category_id in generated_category_ids:
            raise CodegenError(f"category names collide after namespace normalization: {name}")
        categories.add(name)
        category_names_casefold.add(name.casefold())
        category_order.append(name)
        generated_category_ids.add(generated_category_id)
    commands_raw = _expect_array(
        _required(document, "commands", "manifest"), "manifest.commands")
    if not commands_raw or len(commands_raw) > 4096:
        raise CodegenError("manifest.commands must contain between 1 and 4096 commands")

    commands: list[Command] = []
    command_ids: set[int] = set()
    command_names: set[str] = set()
    jass_names: set[str] = set()
    previous_id = -1
    for index, raw_command in enumerate(commands_raw):
        path = f"manifest.commands[{index}]"
        command = _expect_object(raw_command, path)
        _expect_keys(command, COMMAND_KEYS, path)
        command_id = _expect_int(_required(command, "id", path), f"{path}.id")
        if command_id < 0 or command_id > 0x7FFFFFFF:
            raise CodegenError(f"{path}.id is outside non-negative int32")
        if command_id != index or command_id <= previous_id or command_id in command_ids:
            raise CodegenError("command ids must be contiguous from zero in manifest order")
        previous_id = command_id
        command_ids.add(command_id)

        name = _expect_bounded_string(
            _required(command, "name", path), f"{path}.name", 128)
        _validate_ascii(name, f"{path}.name")
        if not COMMAND_NAME_RE.fullmatch(name) or name.casefold() in command_names:
            raise CodegenError(f"{path}.name is invalid or duplicated")
        command_names.add(name.casefold())

        jass_name = _expect_bounded_string(
            _required(command, "jassName", path), f"{path}.jassName", 128)
        if (not JASS_NAME_RE.fullmatch(jass_name) or
                jass_name.casefold() in jass_names or
                jass_name.casefold() in HELPER_NAMES_CASEFOLD):
            raise CodegenError(f"{path}.jassName is invalid, duplicated, or reserved")
        jass_names.add(jass_name.casefold())

        category = _expect_string(_required(command, "category", path), f"{path}.category")
        if category not in categories:
            raise CodegenError(f"{path}.category is not declared")
        carrier = _expect_string(_required(command, "carrier", path), f"{path}.carrier")
        result = _expect_string(_required(command, "result", path), f"{path}.result")
        if carrier not in RESULTS_BY_CARRIER or result not in RESULTS_BY_CARRIER[carrier]:
            raise CodegenError(f"{path} has an unsafe carrier/result pairing")

        arguments_raw = _expect_array(_required(command, "args", path), f"{path}.args")
        if len(arguments_raw) > max_args or len(arguments_raw) > 16:
            raise CodegenError(f"{path}.args exceeds the declared safety limit")
        arguments: list[Argument] = []
        argument_names: set[str] = set()
        for argument_index, raw_argument in enumerate(arguments_raw):
            argument_path = f"{path}.args[{argument_index}]"
            argument = _expect_object(raw_argument, argument_path)
            _expect_keys(argument, ARGUMENT_KEYS, argument_path)
            argument_name = _expect_bounded_string(
                _required(argument, "name", argument_path), f"{argument_path}.name", 64)
            manifest_type = _expect_string(
                _required(argument, "type", argument_path), f"{argument_path}.type")
            if (not ARGUMENT_NAME_RE.fullmatch(argument_name) or
                    argument_name.casefold() in argument_names or
                    argument_name.casefold() in JASS_RESERVED):
                raise CodegenError(f"{argument_path}.name is unsafe or duplicated")
            if manifest_type not in INPUT_TYPES:
                raise CodegenError(f"{argument_path}.type is not allowed")
            argument_names.add(argument_name.casefold())
            arguments.append(Argument(argument_name, manifest_type))

        backend_required = _expect_bool(
            _required(command, "backendRequired", path), f"{path}.backendRequired")
        if not _expect_bool(_required(command, "localVisual", path), f"{path}.localVisual"):
            raise CodegenError(f"{path} must have localVisual=true")
        if not _expect_bool(
                _required(command, "querySyncWarning", path), f"{path}.querySyncWarning"):
            raise CodegenError(f"{path} must have querySyncWarning=true")
        title_zh = _expect_bounded_string(
            _required(command, "titleZh", path), f"{path}.titleZh", 256)
        if any(character in title_zh for character in (",", "=", "\"", "'", "\\")):
            raise CodegenError(f"{path}.titleZh is unsafe for both Catalog formats")
        description_zh = _expect_bounded_string(
            _required(command, "descriptionZh", path), f"{path}.descriptionZh", 4096)
        if not description_zh.startswith(local_warning):
            raise CodegenError(
                f"{path}.descriptionZh must start with the local-visual sync warning")
        _validate_placeholders(
            description_zh, [argument.name for argument in arguments], f"{path}.descriptionZh")

        typed_payload = canonical_prefix + name
        for argument in arguments:
            typed_payload += ";" + SAMPLE_ARGUMENTS[argument.manifest_type]
        _validate_ascii(typed_payload, f"{path} representative payload")
        if len(typed_payload.encode("ascii")) > max_bytes:
            raise CodegenError(f"{path} representative payload exceeds maxBytes")

        commands.append(Command(
            command_id=command_id,
            name=name,
            jass_name=jass_name,
            category=category,
            carrier=carrier,
            arguments=tuple(arguments),
            result=result,
            backend_required=backend_required,
            title_zh=title_zh,
            description_zh=description_zh,
        ))

    return ManifestModel(
        api_version=api_version,
        protocol_version=protocol_version,
        canonical_prefix=canonical_prefix,
        legacy_prefix=legacy_prefix,
        max_bytes=max_bytes,
        max_args=max_args,
        local_warning_zh=local_warning,
        distribution_status=distribution_status,
        deployment_authorized=deployment_authorized,
        map_archive_integration_authorized=map_archive_integration_authorized,
        categories=tuple(category_order),
        commands=tuple(commands),
    )


def load_manifest(path: Path, expected_sha256: str) -> tuple[ManifestModel, str, int]:
    expected = _normalize_sha256(expected_sha256, "--expected-manifest-sha256")
    try:
        with path.open("rb") as stream:
            raw = stream.read(MAX_MANIFEST_BYTES + 1)
    except OSError as error:
        raise CodegenError(f"cannot read manifest: {error}") from error
    if len(raw) > MAX_MANIFEST_BYTES:
        raise CodegenError(
            f"manifest exceeds the {MAX_MANIFEST_BYTES}-byte input safety limit")
    actual = hashlib.sha256(raw).hexdigest()
    if actual != expected:
        raise CodegenError(
            f"manifest SHA-256 mismatch: expected {expected}, actual {actual}")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise CodegenError("manifest is not strict UTF-8") from error
    try:
        document = json.loads(
            text,
            object_pairs_hook=_duplicate_rejecting_object,
            parse_constant=_reject_json_constant,
            parse_int=_bounded_json_int,
            parse_float=_reject_json_float)
    except (json.JSONDecodeError, CodegenError, RecursionError, ValueError) as error:
        raise CodegenError(f"manifest JSON is invalid: {error}") from error
    return _parse_manifest(_expect_object(document, "manifest")), actual, len(raw)


def _json_text(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=False) + "\n"


def _lni_quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def _jass_string(value: str) -> str:
    if '"' in value or "\\" in value or "\n" in value or "\r" in value:
        raise CodegenError("internal error: unsafe JASS string literal")
    return f'"{value}"'


def _payload_expression(model: ManifestModel, command: Command) -> str:
    expression = _jass_string(model.canonical_prefix + command.name)
    for argument in command.arguments:
        expression += f' + ";" + {argument.encoder}({argument.name})'
    return expression


def _render_jass(model: ManifestModel, manifest_sha256: str) -> str:
    lines = [
        "// Generated file. Source manifest SHA-256: " + manifest_sha256,
        "// All functions are local-visual only. Never use returned values in multiplayer sync branches.",
        "",
        "function WarVKEncodeBool takes boolean value returns string",
        "    // Local-visual protocol encoding helper; do not use it for synchronized gameplay state.",
        "    if value then",
        '        return "b:1"',
        "    endif",
        '    return "b:0"',
        "endfunction",
        "",
        "function WarVKEncodeInteger takes integer value returns string",
        "    // Local-visual protocol encoding helper; do not use it for synchronized gameplay state.",
        '    return "i:" + I2S(value)',
        "endfunction",
        "",
        "function WarVKEncodeId takes integer value returns string",
        "    // Local-visual protocol encoding helper; only positive managed ids are accepted by the runtime.",
        '    return "d:" + I2S(value)',
        "endfunction",
        "",
        "function WarVKEncodeReal takes real value returns string",
        "    // Local-visual protocol encoding helper; non-finite or out-of-range values are rejected by the runtime.",
        '    return "r:" + R2S(value)',
        "endfunction",
        "",
    ]
    for command in model.commands:
        takes = "nothing" if not command.arguments else ", ".join(
            f"{argument.jass_type} {argument.name}" for argument in command.arguments)
        lines.append(
            f"function {command.jass_name} takes {takes} returns {RESULT_TYPES[command.result]}")
        lines.append("    // " + command.description_zh)
        expression = _payload_expression(model, command)
        if command.carrier == "preloader":
            lines.append(f"    call Preloader({expression})")
        elif command.carrier == "hotkey":
            call = f"GetLocalizedHotkey({expression})"
            lines.append(f"    return {call} != 0" if command.result == "bool" else f"    return {call}")
        elif command.carrier == "localized_string":
            call = f"GetLocalizedString({expression})"
            lines.append(f"    return S2R({call})" if command.result == "real" else f"    return {call}")
        else:
            raise CodegenError(f"internal error: unsupported carrier {command.carrier}")
        lines.extend(("endfunction", ""))
    return "\n".join(lines)


def _mapui_definition(command: Command) -> str:
    lines = [
        f"[{command.jass_name}]",
        f"title={_lni_quote(command.title_zh)}",
        f"description={_lni_quote(command.description_zh)}",
        f"category={_category_id(command.category)}",
        f"script_name={command.jass_name}",
    ]
    if command.result != "void":
        lines.append(f"returns={RESULT_TYPES[command.result]}")
    if not command.arguments:
        lines.extend(("[[.args]]", "type=nothing"))
    else:
        for argument in command.arguments:
            lines.extend(("[[.args]]", f"type={argument.jass_type}"))
    return "\r\n".join(lines) + "\r\n"


def _render_mapui(model: ManifestModel) -> tuple[str, str, str]:
    actions = "\r\n".join(
        _mapui_definition(command).rstrip("\r\n")
        for command in model.commands if command.result == "void") + "\r\n"
    calls = "\r\n".join(
        _mapui_definition(command).rstrip("\r\n")
        for command in model.commands if command.result != "void") + "\r\n"
    define_lines = ["[TriggerCategories]"]
    define_lines.extend(
        f"{_category_id(category)}=WarVK {category} 本地视觉,"
        for category in model.categories)
    define = "\r\n".join(define_lines) + "\r\n"
    if "//" in define or "//" in actions or "//" in calls:
        raise CodegenError("internal error: MapUI LNI output contains a forbidden comment token")
    return define, actions, calls


def _legacy_signature(command: Command) -> str:
    arguments = [argument.jass_type for argument in command.arguments] or ["nothing"]
    if command.result == "void":
        return "0," + ",".join(arguments)
    return "0,0," + RESULT_TYPES[command.result] + "," + ",".join(arguments)


def _category_id(category: str) -> str:
    return "TC_WARVK_" + category.upper()


def _legacy_description(command: Command) -> str:
    # The leading empty field prevents an outer quote pair from being stripped by
    # readers before CSV fragments are interpreted.
    parts: list[str] = []
    cursor = 0
    for match in PLACEHOLDER_RE.finditer(command.description_zh):
        literal = command.description_zh[cursor:match.start()]
        if literal:
            parts.append(_lni_quote(literal))
        parts.append("~" + match.group(1))
        cursor = match.end()
    trailing = command.description_zh[cursor:]
    if trailing:
        parts.append(_lni_quote(trailing))
    return "," + ",".join(parts)


def _render_legacy(model: ManifestModel) -> tuple[str, str]:
    action_commands = [command for command in model.commands if command.result == "void"]
    call_commands = [command for command in model.commands if command.result != "void"]
    data = ["[TriggerCategories]"]
    data.extend(
        f"{_category_id(category)}=WarVK {category} 本地视觉,"
        for category in model.categories)
    data.append("[TriggerActions]")
    for command in action_commands:
        data.extend((
            f"{command.jass_name}={_legacy_signature(command)}",
            f"_{command.jass_name}_Category={_category_id(command.category)}",
            f"_{command.jass_name}_ScriptName={command.jass_name}",
        ))
    data.append("[TriggerCalls]")
    for command in call_commands:
        data.extend((
            f"{command.jass_name}={_legacy_signature(command)}",
            f"_{command.jass_name}_Category={_category_id(command.category)}",
            f"_{command.jass_name}_ScriptName={command.jass_name}",
        ))

    strings = ["[TriggerCategoryStrings]"]
    strings.extend(
        f"{_category_id(category)}=WarVK {category} 本地视觉"
        for category in model.categories)
    strings.append("[TriggerActionStrings]")
    for command in action_commands:
        strings.extend((
            f"{command.jass_name}={command.title_zh}",
            f"{command.jass_name}={_legacy_description(command)}",
        ))
    strings.append("[TriggerCallStrings]")
    for command in call_commands:
        strings.extend((
            f"{command.jass_name}={command.title_zh}",
            f"{command.jass_name}={_legacy_description(command)}",
        ))
    return "\r\n".join(data) + "\r\n", "\r\n".join(strings) + "\r\n"


def _sample_payload(model: ManifestModel, command: Command, *, legacy: bool = False) -> str:
    payload = (model.legacy_prefix if legacy else model.canonical_prefix) + command.name
    for argument in command.arguments:
        payload += ";" + SAMPLE_ARGUMENTS[argument.manifest_type]
    return payload


def _render_vectors(model: ManifestModel, manifest_sha256: str) -> str:
    legacy_command = next(
        (command for command in model.commands if command.arguments), model.commands[0])
    command_vectors = []
    for command in model.commands:
        payload = _sample_payload(model, command)
        command_vectors.append({
            "id": command.command_id,
            "command": command.name,
            "jassName": command.jass_name,
            "carrier": command.carrier,
            "result": command.result,
            "payload": payload,
            "payloadBytes": len(payload.encode("ascii")),
            "argumentTags": [argument.protocol_tag for argument in command.arguments],
            "expected": "accept",
            "validationScope": "dispatch",
            "generationAllowed": True,
        })
    envelope_command = model.commands[0]
    max_argument_payload = (
        model.canonical_prefix + envelope_command.name + ";b:1" * model.max_args)
    too_many = max_argument_payload + ";b:1"
    max_length_base = model.canonical_prefix + "boundary."
    max_length_payload = max_length_base + "x" * (model.max_bytes - len(max_length_base))
    too_long = max_length_payload + "x"
    carrier_command = next(
        (command for command in model.commands if command.carrier == "preloader"),
        model.commands[0])
    document = {
        "schema": {"name": "warvk-v1-protocol-vectors", "version": 1},
        "manifestSha256": manifest_sha256,
        "protocolVersion": model.protocol_version,
        "limits": {"maxBytes": model.max_bytes, "maxArgs": model.max_args},
        "canonicalGenerationOnly": True,
        "legacyParseOnly": True,
        "commandVectors": command_vectors,
        "typedArgumentCases": [
            {"token": "b:0", "expected": "accept"},
            {"token": "b:1", "expected": "accept"},
            {"token": "b:true", "expected": "reject", "reason": "strict-bool"},
            {"token": "i:-2147483648", "expected": "accept"},
            {"token": "i:2147483647", "expected": "accept"},
            {"token": "i:2147483648", "expected": "reject", "reason": "int32-overflow"},
            {"token": "i:-", "expected": "reject", "reason": "sign-only"},
            {"token": "i: 1", "expected": "reject", "reason": "whitespace"},
            {"token": "d:1", "expected": "accept"},
            {"token": "d:0", "expected": "reject", "reason": "non-positive-id"},
            {"token": "d:-1", "expected": "reject", "reason": "non-positive-id"},
            {"token": "d:2147483648", "expected": "reject", "reason": "id-overflow"},
            {"token": "x:1", "expected": "reject", "reason": "unknown-type-tag"},
            {"token": "r:-1.25e+2", "expected": "accept"},
            {"token": "r:NaN", "expected": "reject", "reason": "non-finite"},
            {"token": "r:Inf", "expected": "reject", "reason": "non-finite"},
            {"token": "r:1e9999", "expected": "reject", "reason": "overflow"},
            {"token": "r: 1", "expected": "reject", "reason": "whitespace"},
        ],
        "payloadCases": [
            {
                "name": "legacy-compatible-parse-only",
                "payload": _sample_payload(model, legacy_command, legacy=True),
                "expected": "accept-compatible-parse",
                "generationAllowed": False,
            },
            {
                "name": "non-warvk-forward",
                "payload": "stock:unrelated",
                "expected": "forward-original-unconditionally",
                "generationAllowed": False,
            },
            {
                "name": "maximum-argument-envelope",
                "payload": max_argument_payload,
                "payloadArguments": model.max_args,
                "expected": "not-too-many-arguments",
                "validationScope": "envelope",
                "generationAllowed": False,
            },
            {
                "name": "too-many-arguments",
                "payload": too_many,
                "expected": "reject",
                "reason": "argument-limit",
                "validationScope": "envelope",
                "generationAllowed": False,
            },
            {
                "name": "maximum-byte-envelope",
                "payload": max_length_payload,
                "payloadBytes": len(max_length_payload.encode("ascii")),
                "expected": "not-payload-too-long",
                "validationScope": "envelope",
                "generationAllowed": False,
            },
            {
                "name": "payload-too-long",
                "payload": too_long,
                "payloadBytes": len(too_long.encode("ascii")),
                "expected": "reject",
                "reason": "byte-limit",
                "validationScope": "envelope",
                "generationAllowed": False,
            },
            {
                "name": "carrier-mismatch",
                "payload": _sample_payload(model, carrier_command),
                "declaredCarrier": carrier_command.carrier,
                "providedCarrier": "hotkey" if carrier_command.carrier != "hotkey" else "preloader",
                "expected": "reject",
                "reason": "carrier-mismatch",
                "generationAllowed": False,
            },
            {
                "name": "non-ascii",
                "payload": model.canonical_prefix + "boundary.é",
                "expected": "reject",
                "reason": "non-ascii",
                "generationAllowed": False,
            },
            {
                "name": "control-character",
                "payload": model.canonical_prefix + "boundary.x\n",
                "expected": "reject",
                "reason": "control-character",
                "generationAllowed": False,
            },
        ],
    }
    return _json_text(document)


def _render_handoff(
        model: ManifestModel,
        manifest_name: str,
        manifest_sha256: str,
        manifest_bytes: int,
        catalog_sha256: str | None,
        artifacts: dict[str, str]) -> str:
    blockers = [
        "static_generation_only",
        "map_archive_integration_not_performed",
        "warcraft_runtime_acceptance_not_performed",
    ]
    if catalog_sha256 is None:
        blockers.insert(0, "target_catalog_sha256_not_supplied")
    if model.distribution_status.casefold() == "blocked":
        blockers.append("manifest_distribution_blocked")
    document = {
        "schema": {"name": "warvk-editor-handoff", "version": 1},
        "generator": {
            "name": "War3MapEditor64 WarVK manifest codegen",
            "version": GENERATOR_VERSION,
            "deterministic": True,
        },
        "manifest": {
            "sourceFileName": manifest_name,
            "sha256": manifest_sha256,
            "bytes": manifest_bytes,
            "apiVersion": model.api_version,
            "protocolVersion": model.protocol_version,
        },
        "catalog": {
            "targetCatalogBound": catalog_sha256 is not None,
            "targetCatalogBaselineSha256": catalog_sha256,
            "mergedCatalogSha256": None,
            "mergedCatalogValidated": False,
        },
        "integration": {"status": "blocked", "blockers": blockers},
        "claims": {
            "distributionAuthorized": False,
            "deploymentAuthorized": False,
            "mapArchiveIntegrationAuthorized": False,
            "runnableMapClaimed": False,
            "editorTrueSave": False,
            "runtimeValidated": False,
            "warcraftExecuted": False,
            "queryResultsSafeForSynchronizedGameplay": False,
            "runtimeEffectVerified": False,
            "mapArchiveIntegrated": False,
            "deploymentPerformed": False,
        },
        "constraints": {
            "localVisualOnly": True,
            "queryResultsForbiddenInMultiplayerSyncBranches": True,
            "catalogFragmentsMustNotOverwriteTargetConfig": True,
            "candidateCompleteOnlyWhenHandoffPresentAndVerified": True,
            "unexpectedFilesMustBeRejected": True,
        },
        "publication": {
            "mode": "windows_handle_bound_commit_marker",
            "targetNoReplace": True,
            "directoryIdentityLeased": True,
            "payloadsReadBackBeforeCommit": True,
            "fixedOutputSetVerifiedBeforeCommit": True,
            "completionMarker": "warvk-handoff.json",
            "completionMarkerReservedExclusiveAtBegin": True,
        },
        "manifestDistribution": {
            "status": model.distribution_status,
            "deploymentAuthorized": model.deployment_authorized,
            "mapArchiveIntegrationAuthorized": model.map_archive_integration_authorized,
        },
        "artifacts": [
            {
                "path": path,
                "sha256": hashlib.sha256(content.encode("utf-8")).hexdigest(),
                "bytes": len(content.encode("utf-8")),
            }
            for path, content in artifacts.items()
        ],
    }
    return _json_text(document)


def build_outputs(
        model: ManifestModel,
        manifest_name: str,
        manifest_sha256: str,
        manifest_bytes: int,
        catalog_sha256: str | None) -> dict[str, str]:
    define, actions, calls = _render_mapui(model)
    trigger_data, trigger_strings = _render_legacy(model)
    artifacts = {
        "jass/WarVK.j": _render_jass(model, manifest_sha256),
        "catalog/mapui/ui/config": "warvk\n",
        "catalog/mapui/ui/warvk/define.txt": define,
        "catalog/mapui/ui/warvk/action.txt": actions,
        "catalog/mapui/ui/warvk/call.txt": calls,
        "catalog/legacy/config": "warvk\n",
        "catalog/legacy/warvk/ui/TriggerData.txt": trigger_data,
        "catalog/legacy/warvk/ui/TriggerStrings.txt": trigger_strings,
        "protocol/warvk-v1-vectors.json": _render_vectors(model, manifest_sha256),
    }
    artifacts["warvk-handoff.json"] = _render_handoff(
        model, manifest_name, manifest_sha256, manifest_bytes, catalog_sha256, artifacts)
    if tuple(artifacts) != OUTPUT_PATHS:
        raise CodegenError("internal error: generated output set is not the fixed contract")
    return artifacts


class _UnicodeString(ctypes.Structure):
    _fields_ = [
        ("Length", wintypes.USHORT),
        ("MaximumLength", wintypes.USHORT),
        ("Buffer", wintypes.LPWSTR),
    ]


class _ObjectAttributes(ctypes.Structure):
    _fields_ = [
        ("Length", wintypes.ULONG),
        ("RootDirectory", wintypes.HANDLE),
        ("ObjectName", ctypes.POINTER(_UnicodeString)),
        ("Attributes", wintypes.ULONG),
        ("SecurityDescriptor", wintypes.LPVOID),
        ("SecurityQualityOfService", wintypes.LPVOID),
    ]


class _IoStatusUnion(ctypes.Union):
    _fields_ = [("Status", ctypes.c_long), ("Pointer", wintypes.LPVOID)]


class _IoStatusBlock(ctypes.Structure):
    _anonymous_ = ("value",)
    _fields_ = [("value", _IoStatusUnion), ("Information", ctypes.c_size_t)]


class _ByHandleFileInformation(ctypes.Structure):
    _fields_ = [
        ("FileAttributes", wintypes.DWORD),
        ("CreationTime", wintypes.FILETIME),
        ("LastAccessTime", wintypes.FILETIME),
        ("LastWriteTime", wintypes.FILETIME),
        ("VolumeSerialNumber", wintypes.DWORD),
        ("FileSizeHigh", wintypes.DWORD),
        ("FileSizeLow", wintypes.DWORD),
        ("NumberOfLinks", wintypes.DWORD),
        ("FileIndexHigh", wintypes.DWORD),
        ("FileIndexLow", wintypes.DWORD),
    ]


class _WinHandle:
    def __init__(self, api: "_WindowsApi", value: int) -> None:
        self._api = api
        self.value = value

    def close(self) -> None:
        if self.value:
            self._api.close_handle(self.value)
            self.value = 0

    def __del__(self) -> None:
        if self.value:
            try:
                self.close()
            except Exception:
                pass


class _WindowsApi:
    FILE_LIST_DIRECTORY = 0x0001
    FILE_ADD_FILE = 0x0002
    FILE_ADD_SUBDIRECTORY = 0x0004
    FILE_TRAVERSE = 0x0020
    FILE_READ_ATTRIBUTES = 0x0080
    DELETE = 0x00010000
    SYNCHRONIZE = 0x00100000
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 0x00000001
    FILE_SHARE_WRITE = 0x00000002
    OPEN_EXISTING = 3
    FILE_ATTRIBUTE_DIRECTORY = 0x00000010
    FILE_ATTRIBUTE_NORMAL = 0x00000080
    FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
    FILE_CREATE = 2
    FILE_DIRECTORY_FILE = 0x00000001
    FILE_NON_DIRECTORY_FILE = 0x00000040
    FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020
    OBJ_CASE_INSENSITIVE = 0x00000040
    FILE_BEGIN = 0

    def __init__(self) -> None:
        if os.name != "nt":
            raise CodegenError("handle-bound publication is supported only on Windows")
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.ntdll = ctypes.WinDLL("ntdll")
        self.invalid_handle = ctypes.c_void_p(-1).value

        self.kernel32.CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
        ]
        self.kernel32.CreateFileW.restype = wintypes.HANDLE
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        self.kernel32.GetFinalPathNameByHandleW.argtypes = [
            wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD,
        ]
        self.kernel32.GetFinalPathNameByHandleW.restype = wintypes.DWORD
        self.kernel32.GetFileInformationByHandle.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(_ByHandleFileInformation),
        ]
        self.kernel32.GetFileInformationByHandle.restype = wintypes.BOOL
        self.kernel32.WriteFile.argtypes = [
            wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        ]
        self.kernel32.WriteFile.restype = wintypes.BOOL
        self.kernel32.ReadFile.argtypes = [
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        ]
        self.kernel32.ReadFile.restype = wintypes.BOOL
        self.kernel32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
        self.kernel32.FlushFileBuffers.restype = wintypes.BOOL
        self.kernel32.SetFilePointerEx.argtypes = [
            wintypes.HANDLE, ctypes.c_longlong, ctypes.POINTER(ctypes.c_longlong),
            wintypes.DWORD,
        ]
        self.kernel32.SetFilePointerEx.restype = wintypes.BOOL
        self.kernel32.GetFileSizeEx.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(ctypes.c_longlong),
        ]
        self.kernel32.GetFileSizeEx.restype = wintypes.BOOL
        self.ntdll.NtCreateFile.argtypes = [
            ctypes.POINTER(wintypes.HANDLE), wintypes.ULONG,
            ctypes.POINTER(_ObjectAttributes), ctypes.POINTER(_IoStatusBlock),
            ctypes.POINTER(ctypes.c_longlong), wintypes.ULONG, wintypes.ULONG,
            wintypes.ULONG, wintypes.ULONG, wintypes.LPVOID, wintypes.ULONG,
        ]
        self.ntdll.NtCreateFile.restype = ctypes.c_long

    @staticmethod
    def _same_path(left: Path, right: Path) -> bool:
        return os.path.normcase(os.path.normpath(str(left))) == \
            os.path.normcase(os.path.normpath(str(right)))

    @staticmethod
    def _display_status(status: int) -> str:
        return f"0x{status & 0xFFFFFFFF:08X}"

    def _last_error(self, operation: str) -> CodegenError:
        return CodegenError(f"{operation} failed with WinError {ctypes.get_last_error()}")

    def close_handle(self, handle: int) -> None:
        if not self.kernel32.CloseHandle(wintypes.HANDLE(handle)):
            raise self._last_error("CloseHandle")

    def final_path(self, handle: int) -> Path:
        flags = 0x0  # FILE_NAME_NORMALIZED | VOLUME_NAME_DOS
        required = self.kernel32.GetFinalPathNameByHandleW(
            wintypes.HANDLE(handle), None, 0, flags)
        if required == 0:
            raise self._last_error("GetFinalPathNameByHandleW")
        buffer = ctypes.create_unicode_buffer(required + 1)
        written = self.kernel32.GetFinalPathNameByHandleW(
            wintypes.HANDLE(handle), buffer, len(buffer), flags)
        if written == 0 or written >= len(buffer):
            raise self._last_error("GetFinalPathNameByHandleW")
        value = buffer.value
        if value.startswith("\\\\?\\UNC\\"):
            value = "\\\\" + value[8:]
        elif value.startswith("\\\\?\\"):
            value = value[4:]
        return Path(value)

    def _validate_directory(self, handle: int, operation: str) -> None:
        information = _ByHandleFileInformation()
        if not self.kernel32.GetFileInformationByHandle(
                wintypes.HANDLE(handle), ctypes.byref(information)):
            raise self._last_error(operation)
        if not information.FileAttributes & self.FILE_ATTRIBUTE_DIRECTORY:
            raise CodegenError(f"{operation} resolved to a non-directory object")

    def open_directory(self, path: Path, *, writable: bool) -> _WinHandle:
        access = self.FILE_READ_ATTRIBUTES | self.FILE_TRAVERSE | self.SYNCHRONIZE
        if writable:
            access |= self.FILE_LIST_DIRECTORY | self.FILE_ADD_FILE | self.FILE_ADD_SUBDIRECTORY
        handle = self.kernel32.CreateFileW(
            str(path), access, self.FILE_SHARE_READ | self.FILE_SHARE_WRITE,
            None, self.OPEN_EXISTING, self.FILE_FLAG_BACKUP_SEMANTICS, None)
        if handle is None or handle == self.invalid_handle:
            raise self._last_error(f"cannot lease directory {path}")
        result = _WinHandle(self, int(handle))
        try:
            self._validate_directory(result.value, f"cannot validate directory {path}")
            return result
        except Exception:
            result.close()
            raise

    def create_relative(
            self, parent: int, name: str, *, directory: bool,
            share_read: bool = True) -> _WinHandle:
        if not name or name in {".", ".."} or "\\" in name or "/" in name:
            raise CodegenError("handle-relative publication requires safe simple names")
        encoded_length = len(name.encode("utf-16-le"))
        if encoded_length > 0xFFFC:
            raise CodegenError("handle-relative publication name is too long")
        name_buffer = ctypes.create_unicode_buffer(name)
        unicode_name = _UnicodeString(
            encoded_length, encoded_length + 2,
            ctypes.cast(name_buffer, wintypes.LPWSTR))
        attributes = _ObjectAttributes(
            ctypes.sizeof(_ObjectAttributes), wintypes.HANDLE(parent),
            ctypes.pointer(unicode_name), self.OBJ_CASE_INSENSITIVE, None, None)
        io_status = _IoStatusBlock()
        handle = wintypes.HANDLE()
        if directory:
            access = (
                self.FILE_LIST_DIRECTORY | self.FILE_ADD_FILE |
                self.FILE_ADD_SUBDIRECTORY | self.FILE_TRAVERSE |
                self.FILE_READ_ATTRIBUTES | self.SYNCHRONIZE)
            file_attributes = self.FILE_ATTRIBUTE_DIRECTORY
            share = self.FILE_SHARE_READ | self.FILE_SHARE_WRITE
            options = self.FILE_DIRECTORY_FILE | self.FILE_SYNCHRONOUS_IO_NONALERT
        else:
            access = self.GENERIC_READ | self.GENERIC_WRITE | self.SYNCHRONIZE
            file_attributes = self.FILE_ATTRIBUTE_NORMAL
            share = self.FILE_SHARE_READ if share_read else 0
            options = self.FILE_NON_DIRECTORY_FILE | self.FILE_SYNCHRONOUS_IO_NONALERT
        status = self.ntdll.NtCreateFile(
            ctypes.byref(handle), access, ctypes.byref(attributes), ctypes.byref(io_status),
            None, file_attributes, share, self.FILE_CREATE, options, None, 0)
        if status < 0 or not handle.value:
            raise CodegenError(
                f"exclusive handle-relative create failed for {name}: "
                f"NTSTATUS {self._display_status(status)}")
        result = _WinHandle(self, int(handle.value))
        if directory:
            try:
                self._validate_directory(result.value, f"cannot validate created directory {name}")
            except Exception:
                result.close()
                raise
        return result

    def write_and_read_back(self, handle: int, payload: bytes) -> bytes:
        offset = 0
        while offset < len(payload):
            chunk = payload[offset:offset + (1 << 20)]
            buffer = ctypes.create_string_buffer(chunk)
            written = wintypes.DWORD()
            if not self.kernel32.WriteFile(
                    wintypes.HANDLE(handle), buffer, len(chunk),
                    ctypes.byref(written), None):
                raise self._last_error("WriteFile")
            if written.value == 0:
                raise CodegenError("WriteFile made no progress")
            offset += written.value
        if not self.kernel32.FlushFileBuffers(wintypes.HANDLE(handle)):
            raise self._last_error("FlushFileBuffers")
        size = ctypes.c_longlong()
        if not self.kernel32.GetFileSizeEx(wintypes.HANDLE(handle), ctypes.byref(size)):
            raise self._last_error("GetFileSizeEx")
        if size.value != len(payload):
            raise CodegenError("handle-bound payload size changed after write")
        if not self.kernel32.SetFilePointerEx(
                wintypes.HANDLE(handle), 0, None, self.FILE_BEGIN):
            raise self._last_error("SetFilePointerEx")
        result = bytearray()
        while len(result) < size.value:
            requested = min(1 << 20, size.value - len(result))
            buffer = ctypes.create_string_buffer(requested)
            read = wintypes.DWORD()
            if not self.kernel32.ReadFile(
                    wintypes.HANDLE(handle), buffer, requested,
                    ctypes.byref(read), None):
                raise self._last_error("ReadFile")
            if read.value == 0:
                raise CodegenError("handle-bound payload ended before its declared size")
            result.extend(buffer.raw[:read.value])
        return bytes(result)


def _safe_windows_component(value: str, path: str) -> None:
    if not value or value in {".", ".."} or value[-1] in {".", " "} or any(
            ord(character) < 0x20 or character in '<>:"/\\|?*' for character in value):
        raise CodegenError(f"{path} is not a safe Windows path component")
    reserved = {"CON", "PRN", "AUX", "NUL"}
    reserved.update(f"COM{index}" for index in range(1, 10))
    reserved.update(f"LPT{index}" for index in range(1, 10))
    if value.split(".", 1)[0].upper() in reserved:
        raise CodegenError(f"{path} uses a reserved Windows device name")


def _lease_output_parent(api: _WindowsApi, parent: Path) -> tuple[Path, list[_WinHandle]]:
    handles: list[_WinHandle] = []
    try:
        leaf = api.open_directory(parent, writable=True)
        handles.append(leaf)
        final_parent = api.final_path(leaf.value)
        ancestor = final_parent.parent
        while ancestor and not api._same_path(ancestor, ancestor.parent):
            handles.append(api.open_directory(ancestor, writable=False))
            ancestor = ancestor.parent
        if not api._same_path(api.final_path(leaf.value), final_parent):
            raise CodegenError(
                "output parent identity changed while its ancestor leases were acquired")
        return final_parent, handles
    except Exception:
        for handle in reversed(handles):
            try:
                handle.close()
            except CodegenError:
                pass
        raise


def _verify_handoff_inventory(handoff: bytes, measured: dict[str, bytes]) -> None:
    try:
        document = json.loads(handoff.decode("utf-8"))
        entries = document["artifacts"]
    except (KeyError, TypeError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CodegenError("internal handoff inventory is malformed") from error
    expected_paths = set(measured)
    if not isinstance(entries, list) or len(entries) != len(expected_paths):
        raise CodegenError("internal handoff inventory does not cover every payload")
    seen: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"path", "sha256", "bytes"}:
            raise CodegenError("internal handoff inventory entry is malformed")
        relative = entry["path"]
        if relative not in measured or relative in seen:
            raise CodegenError("internal handoff inventory path is missing or duplicated")
        payload = measured[relative]
        if entry["bytes"] != len(payload) or entry["sha256"] != hashlib.sha256(payload).hexdigest():
            raise CodegenError("internal handoff inventory is not bound to read-back bytes")
        seen.add(relative)
    if seen != expected_paths:
        raise CodegenError("internal handoff inventory path set is incomplete")


def _verify_fixed_output_tree(output: Path) -> None:
    expected_files = set(OUTPUT_PATHS)
    expected_directories: set[str] = set()
    for relative in OUTPUT_PATHS:
        components = relative.split("/")[:-1]
        for count in range(1, len(components) + 1):
            expected_directories.add("/".join(components[:count]))
    observed_files: set[str] = set()
    observed_directories: set[str] = set()
    pending = [output]
    visited = 0
    while pending:
        directory = pending.pop()
        try:
            entries = list(os.scandir(directory))
        except OSError as error:
            raise CodegenError(f"cannot enumerate handle-bound output tree: {error}") from error
        for entry in entries:
            visited += 1
            if visited > 128:
                raise CodegenError("generated output tree exceeds its 128-entry safety limit")
            relative = Path(entry.path).relative_to(output).as_posix()
            try:
                information = entry.stat(follow_symlinks=False)
            except OSError as error:
                raise CodegenError(
                    f"cannot inspect generated output entry {relative}: {error}") from error
            if getattr(information, "st_file_attributes", 0) & 0x00000400:
                raise CodegenError(f"generated output contains a reparse point: {relative}")
            if entry.is_dir(follow_symlinks=False):
                observed_directories.add(relative)
                if relative in expected_directories:
                    pending.append(Path(entry.path))
            elif entry.is_file(follow_symlinks=False):
                observed_files.add(relative)
            else:
                raise CodegenError(f"generated output contains a non-file entry: {relative}")
    if observed_files != expected_files or observed_directories != expected_directories:
        raise CodegenError("generated output tree contains missing or unexpected entries")


def publish_outputs(output: Path, artifacts: dict[str, str]) -> None:
    if tuple(artifacts) != OUTPUT_PATHS:
        raise CodegenError("publication refuses an output set outside the fixed contract")
    try:
        output = output.absolute()
    except (OSError, RuntimeError, UnicodeError) as error:
        raise CodegenError(f"cannot resolve output path: {error}") from error
    _safe_windows_component(output.name, "output filename")
    if os.path.lexists(output):
        raise CodegenError(f"output already exists; overwrite is forbidden: {output}")
    parent = output.parent
    try:
        parent.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise CodegenError(f"cannot create output parent: {error}") from error

    api = _WindowsApi()
    handles: list[_WinHandle] = []
    output_created = False
    try:
        final_parent, parent_handles = _lease_output_parent(api, parent)
        handles.extend(parent_handles)
        root = api.create_relative(parent_handles[0].value, output.name, directory=True)
        handles.append(root)
        output_created = True
        expected_root = final_parent / output.name
        if not api._same_path(api.final_path(root.value), expected_root):
            raise CodegenError("created output directory escaped the leased parent identity")

        # Reserve the completion marker before any payload path becomes writable. It remains
        # unreadable until every payload has been flushed and verified, and it is closed only
        # after all payload handles are released.
        handoff_handle = api.create_relative(
            root.value, "warvk-handoff.json", directory=False, share_read=False)
        handles.append(handoff_handle)

        directory_handles: dict[str, _WinHandle] = {"": root}
        directories: set[str] = set()
        for relative in artifacts:
            components = relative.split("/")[:-1]
            for count in range(1, len(components) + 1):
                directories.add("/".join(components[:count]))
        for relative in sorted(directories, key=lambda value: (value.count("/"), value)):
            parent_name, _, name = relative.rpartition("/")
            _safe_windows_component(name, f"generated directory {relative}")
            created = api.create_relative(
                directory_handles[parent_name].value, name, directory=True)
            handles.append(created)
            directory_handles[relative] = created

        measured: dict[str, bytes] = {}
        for relative in OUTPUT_PATHS[:-1]:
            parent_name, _, name = relative.rpartition("/")
            _safe_windows_component(name, f"generated file {relative}")
            payload = artifacts[relative].encode("utf-8")
            created = api.create_relative(
                directory_handles[parent_name].value, name, directory=False)
            handles.append(created)
            actual = api.write_and_read_back(created.value, payload)
            if actual != payload:
                raise CodegenError(f"handle-bound read-back mismatch for {relative}")
            measured[relative] = actual

        handoff_payload = artifacts["warvk-handoff.json"].encode("utf-8")
        _verify_handoff_inventory(handoff_payload, measured)
        _verify_fixed_output_tree(output)
        if api.write_and_read_back(handoff_handle.value, handoff_payload) != handoff_payload:
            raise CodegenError("handle-bound read-back mismatch for warvk-handoff.json")
    except CodegenError as error:
        if output_created:
            raise CodegenError(
                f"{error}; incomplete candidate preserved without a completion guarantee at "
                f"{output}") from error
        if os.path.lexists(output):
            raise CodegenError(
                f"output appeared during generation; overwrite is forbidden: {output}") from error
        raise
    except (OSError, RuntimeError, UnicodeError) as error:
        suffix = (
            f"; incomplete candidate preserved without a completion guarantee at {output}"
            if output_created else "")
        raise CodegenError(f"cannot publish generated output: {error}{suffix}") from error
    finally:
        close_error: CodegenError | None = None
        for handle in reversed(handles):
            try:
                handle.close()
            except CodegenError as error:
                close_error = close_error or error
        if close_error is not None and sys.exc_info()[0] is None:
            raise close_error


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate a static WarVK editor hand-off bundle from an explicit manifest.")
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--expected-manifest-sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--catalog-sha256",
        help="Optional expected external Catalog SHA-256. No Catalog files are read by this tool.")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    if sys.version_info < MINIMUM_PYTHON:
        print("warvk-codegen: Python 3.10 or newer is required", file=sys.stderr)
        return 2
    parser = _argument_parser()
    arguments = parser.parse_args(argv)
    try:
        if os.name != "nt":
            raise CodegenError(
                "no-replace candidate publication is supported only on Windows")
        catalog_sha256 = (
            _normalize_sha256(arguments.catalog_sha256, "--catalog-sha256")
            if arguments.catalog_sha256 is not None else None)
        model, manifest_sha256, manifest_bytes = load_manifest(
            arguments.manifest, arguments.expected_manifest_sha256)
        manifest_name = _expect_bounded_string(
            arguments.manifest.name, "manifest filename", 255)
        artifacts = build_outputs(
            model, manifest_name, manifest_sha256, manifest_bytes, catalog_sha256)
        publish_outputs(arguments.output, artifacts)
    except CodegenError as error:
        print(f"warvk-codegen: error: {error}", file=sys.stderr)
        return 2
    except Exception as error:  # Defensive CLI boundary: never expose a traceback to consumers.
        print(
            f"warvk-codegen: error: unexpected fail-closed error "
            f"({type(error).__name__}): {error}",
            file=sys.stderr)
        return 2
    print(f"generated {len(artifacts)} files at {arguments.output.absolute()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
