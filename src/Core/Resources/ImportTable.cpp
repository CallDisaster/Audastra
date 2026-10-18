#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <limits>

namespace War3Map::Resources {
namespace {

std::string_view StandardPrefix(ImportScope scope) {
    return scope == ImportScope::Campaign ? "war3campaignImported\\" : "war3mapImported\\";
}
std::string_view CanonicalPrefix(ImportScope scope) {
    return scope == ImportScope::Campaign ? "WAR3CAMPAIGNIMPORTED\\" : "WAR3MAPIMPORTED\\";
}
constexpr std::size_t kMaxFileBytes = 64u * 1024u * 1024u;

std::uint32_t ReadU32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8);
    }
    return value;
}

void AppendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

std::string NormalizeSeparators(std::string_view path) {
    std::string normalized(path);
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    return normalized;
}

bool ComponentSafe(std::string_view component) {
    if (component.empty() || component == "." || component == ".." ||
        component.back() == '.' || component.back() == ' ') {
        return false;
    }
    const auto stem = component.substr(0, component.find('.'));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
        return false;
    }
    return !(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        stem[3] >= '1' && stem[3] <= '9');
}

Result<std::string> UnsafePath() {
    return Result<std::string>::Failure("ImportTable", "IMPORT_PATH_UNSAFE",
        "导入路径必须是安全的相对归档路径，不能包含绝对路径、父目录、设备名或不支持的字符");
}

Result<std::string> AmbiguousPath(const ImportRecord& record) {
    return Result<std::string>::Failure("ImportTable", "IMPORT_PATH_AMBIGUOUS",
        "导入记录的标准路径与完整路径无法唯一确定：" + record.path);
}

template <typename T>
Status FailureStatus(const Result<T>& result) {
    Status status;
    status.diagnostics = result.diagnostics;
    return status;
}

} // namespace

bool IsEditorManagedMemberPath(std::string_view path) {
    std::string key(path);
    for (auto& character : key) {
        if (character == '/') character = '\\';
        else if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    if (key.starts_with('(') || key == "scripts\\war3map.j" || key == "scripts\\war3map.lua") return true;
    return key.find('\\') == std::string::npos && (key.starts_with("war3map") || key.starts_with("war3campaign"));
}

Result<std::string> CanonicalImportPath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\') {
        return UnsafePath();
    }
    std::string canonical;
    canonical.reserve(path.size());
    std::size_t componentBegin = 0;
    for (unsigned char character : path) {
        if (character < 0x20 || character >= 0x7F || character == ':' || character == '"' ||
            character == '<' || character == '>' || character == '|' || character == '?' || character == '*') {
            return UnsafePath();
        }
        if (character == '/' || character == '\\') {
            if (!ComponentSafe(std::string_view(canonical).substr(componentBegin))) return UnsafePath();
            canonical.push_back('\\');
            componentBegin = canonical.size();
        } else {
            canonical.push_back(static_cast<char>(character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character));
        }
    }
    if (!ComponentSafe(std::string_view(canonical).substr(componentBegin))) return UnsafePath();
    return Result<std::string>::Success(std::move(canonical));
}

Result<std::string> ResolveArchivePath(const ImportRecord& record, const ImportMemberExists& exists, ImportScope scope) {
    const auto directCanonical = CanonicalImportPath(record.path);
    if (!directCanonical.Succeeded()) return directCanonical;
    const auto direct = NormalizeSeparators(record.path);
    if (record.flags == 10 || record.flags == 13) {
        return Result<std::string>::Success(direct);
    }
    const std::string prefixed = std::string(StandardPrefix(scope)) + direct;
    const bool isStandard = record.flags == 5 || record.flags == 8;
    if (exists) {
        const bool directExists = exists(direct);
        const bool prefixedExists = exists(prefixed);
        if (directExists && prefixedExists) return AmbiguousPath(record);
        if (directExists) return Result<std::string>::Success(direct);
        if (prefixedExists) return Result<std::string>::Success(prefixed);
    }
    if (!isStandard) {
        return Result<std::string>::Failure("ImportTable", "IMPORT_FLAGS_UNSUPPORTED",
            "导入记录的路径标志尚未确认且缺少唯一归档成员证据：" + std::to_string(record.flags));
    }
    if (directCanonical.value->starts_with(CanonicalPrefix(scope))) return AmbiguousPath(record);
    return Result<std::string>::Success(prefixed);
}

// 有归档证据的未知标志或双路径歧义记录：只有两种安全候选均非本次目标时才能原样保留。
// 不选择其中一个路径，不改写该记录；缺少证据、任一候选相关或其它错误仍拒绝。
Result<bool> MatchesRecord(const ImportRecord& record, const std::string& wanted, const ImportMemberExists& exists, ImportScope scope) {
    const auto fail = [](std::vector<Diagnostic> diagnostics) {
        Result<bool> output;
        output.diagnostics = std::move(diagnostics);
        return output;
    };
    const auto resolved = ResolveArchivePath(record, exists, scope);
    if (resolved.Succeeded()) {
        const auto canonical = CanonicalImportPath(*resolved.value);
        if (!canonical.Succeeded()) return fail(canonical.diagnostics);
        return Result<bool>::Success(*canonical.value == wanted);
    }
    const bool canExcludeTarget = !resolved.diagnostics.empty() &&
        (resolved.diagnostics.front().code == "IMPORT_FLAGS_UNSUPPORTED" || resolved.diagnostics.front().code == "IMPORT_PATH_AMBIGUOUS");
    if (!canExcludeTarget || !exists) return fail(resolved.diagnostics);
    const auto direct = CanonicalImportPath(record.path);
    const auto prefixed = CanonicalImportPath(std::string(StandardPrefix(scope)) + NormalizeSeparators(record.path));
    if (!direct.Succeeded() || !prefixed.Succeeded()) return fail(resolved.diagnostics);
    if (*direct.value == wanted || *prefixed.value == wanted) return fail(resolved.diagnostics);
    return Result<bool>::Success(false);
}

Result<ImportTable> ImportTable::Parse(std::span<const std::byte> bytes, ImportScope scope) {
    if (bytes.size() < 8) {
        return Result<ImportTable>::Failure("ImportTable", "IMPORT_HEADER_TRUNCATED", "导入清单头部不完整");
    }
    if (bytes.size() > kMaxFileBytes) {
        return Result<ImportTable>::Failure("ImportTable", "IMPORT_BUDGET_EXCEEDED", "导入清单超过解析预算");
    }
    if (ReadU32(bytes, 0) != 1) {
        return Result<ImportTable>::Failure("ImportTable", "IMPORT_VERSION_UNSUPPORTED", "仅支持 classic war3map.imp v1，原清单必须保留");
    }
    const auto count = ReadU32(bytes, 4);
    if (count > (bytes.size() - 8) / 2) {
        return Result<ImportTable>::Failure("ImportTable", "IMPORT_COUNT_INVALID", "导入记录数量超出文件边界");
    }
    ImportTable table;
    table.scope_ = scope;
    table.originalBytes_.assign(bytes.begin(), bytes.end());
    table.entries_.reserve(count);
    std::size_t position = 8;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (position >= bytes.size()) {
            return Result<ImportTable>::Failure("ImportTable", "IMPORT_RECORD_TRUNCATED", "导入记录缺少路径标志");
        }
        ImportRecord record;
        record.flags = std::to_integer<std::uint8_t>(bytes[position++]);
        const auto start = position;
        while (position < bytes.size() && bytes[position] != std::byte{0}) ++position;
        if (position == bytes.size()) {
            return Result<ImportTable>::Failure("ImportTable", "IMPORT_PATH_TRUNCATED", "导入路径缺少结束符");
        }
        record.path.assign(reinterpret_cast<const char*>(bytes.data() + start), position - start);
        ++position;
        table.entries_.push_back(std::move(record));
    }
    // 记录计数给出确定边界；扩展尾部无需解释，后续编辑仍原样保留。
    table.trailing_.assign(bytes.begin() + position, bytes.end());
    return Result<ImportTable>::Success(std::move(table));
}

ImportTable ImportTable::CreateEmpty(ImportScope scope) {
    ImportTable table;
    table.scope_ = scope;
    AppendU32(table.originalBytes_, 1);
    AppendU32(table.originalBytes_, 0);
    return table;
}

Status ImportTable::Upsert(std::string_view archivePath, const ImportMemberExists& exists) {
    if (archivePath.size() > kMaxFileBytes - 10) {
        return Status::Failure("ImportTable", "IMPORT_BUDGET_EXCEEDED", "导入路径超过清单输出预算");
    }
    const auto wanted = CanonicalImportPath(archivePath);
    if (!wanted.Succeeded()) return FailureStatus(wanted);
    // 本次导入的成员保存后必然存在。未知标志记录若仅因缺少成员证据而可能指向它，
    // 就把它计入证据重新解析：唯一指向本次路径时沿用原记录（不改写 flags），另一路径也存在仍是歧义。
    // 只对“缺证据”的记录补证据，标准/完整路径记录的解析不受影响；没有归档证据的调用方保持拒绝。
    const ImportMemberExists afterImport = exists ? ImportMemberExists([&](std::string_view path) {
        const auto canonical = CanonicalImportPath(path);
        return (canonical.Succeeded() && *canonical.value == *wanted.value) || exists(path);
    }) : ImportMemberExists{};
    bool found = false;
    for (const auto& entry : entries_) {
        auto matches = MatchesRecord(entry, *wanted.value, exists, scope_);
        if (!matches.Succeeded() && afterImport && !matches.diagnostics.empty()
            && matches.diagnostics.front().code == "IMPORT_FLAGS_UNSUPPORTED") {
            matches = MatchesRecord(entry, *wanted.value, afterImport, scope_);
        }
        if (!matches.Succeeded()) return FailureStatus(matches);
        found = found || *matches.value;
    }
    if (found) return Status::Ok();
    if (entries_.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
        return Status::Failure("ImportTable", "IMPORT_COUNT_INVALID", "导入记录数量超出格式上限");
    }
    std::size_t outputSize = 8 + trailing_.size();
    for (const auto& entry : entries_) outputSize += 2 + entry.path.size();
    if (outputSize > kMaxFileBytes || archivePath.size() + 2 > kMaxFileBytes - outputSize) {
        return Status::Failure("ImportTable", "IMPORT_BUDGET_EXCEEDED", "新增记录将使导入清单超过输出预算");
    }
    // 本机 YDWE/w3x2lni 原生打包器使用 13 (CR) + 完整归档路径。
    // 格式边界参考 War3Net ImportedFiles binary reader；实现为独立 C++ 编写。
    entries_.push_back({13, NormalizeSeparators(archivePath)});
    modified_ = true;
    return Status::Ok();
}

Status ImportTable::Remove(std::string_view archivePath, const ImportMemberExists& exists) {
    const auto wanted = CanonicalImportPath(archivePath);
    if (!wanted.Succeeded()) return FailureStatus(wanted);
    // 先全部解析成功再修改，失败时清单保持原样。
    std::vector<bool> matches(entries_.size(), false);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto match = MatchesRecord(entries_[index], *wanted.value, exists, scope_);
        if (!match.Succeeded()) return FailureStatus(match);
        matches[index] = *match.value;
    }
    // 无匹配时不能先移走 entries_ 再丢弃 kept；保留原记录供后续操作使用。
    if (std::none_of(matches.begin(), matches.end(), [](bool matched) { return matched; })) return Status::Ok();
    std::vector<ImportRecord> kept;
    kept.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (!matches[index]) kept.push_back(std::move(entries_[index]));
    }
    if (kept.size() != entries_.size()) {
        entries_ = std::move(kept);
        modified_ = true;
    }
    return Status::Ok();
}

Result<std::vector<std::byte>> ImportTable::Serialize() const {
    if (!modified_) return Result<std::vector<std::byte>>::Success(originalBytes_);
    std::vector<std::byte> bytes;
    AppendU32(bytes, 1);
    AppendU32(bytes, static_cast<std::uint32_t>(entries_.size()));
    for (const auto& record : entries_) {
        bytes.push_back(static_cast<std::byte>(record.flags));
        const auto* begin = reinterpret_cast<const std::byte*>(record.path.data());
        bytes.insert(bytes.end(), begin, begin + record.path.size());
        bytes.push_back(std::byte{0});
    }
    bytes.insert(bytes.end(), trailing_.begin(), trailing_.end());
    if (bytes.size() > kMaxFileBytes) {
        return Result<std::vector<std::byte>>::Failure("ImportTable", "IMPORT_BUDGET_EXCEEDED", "导入清单超过输出预算");
    }
    return Result<std::vector<std::byte>>::Success(std::move(bytes));
}

} // namespace War3Map::Resources
