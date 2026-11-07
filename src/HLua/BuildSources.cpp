#include "BuildInternal.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/MapData/LniMapSettings.h"
#include "War3MapCore/Text/LniData.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <set>

namespace War3Map::HLua::Detail {
namespace {
template<class T> Result<T> Fail(const char* code, std::string message) { return Result<T>::Failure("HLua", code, std::move(message)); }
std::vector<std::byte> Bytes(std::string_view text) { return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())}; }
std::string Scalar(const Text::LniDataValue& value) {
    if (value.kind == Text::LniDataValue::Kind::Table) throw std::runtime_error("misc.ini 不接受嵌套值。");
    return value.text;
}
}
Result<std::vector<TreeFile>> CaptureTree(const std::filesystem::path& root) {
    using Output = Result<std::vector<TreeFile>>;
    std::vector<TreeFile> files;
    std::vector<std::filesystem::path> pending{std::filesystem::absolute(root).lexically_normal()};
    std::set<std::string> names;
    std::size_t total = 0, entries = 0;
    // 验证根的每一层，不通过目录联接读取外部树。
    auto part = pending.front().root_path();
    for (const auto& name : pending.front().relative_path()) {
        part /= name; const auto attr = GetFileAttributesW(part.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT) || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return Fail<std::vector<TreeFile>>("HLUA_INPUT_PATH", "输入目录缺失或包含链接：" + IO::PathUtf8(part));
    }
    const auto absolute = pending.front();
    try {
        while (!pending.empty()) {
            const auto directory = std::move(pending.back()); pending.pop_back();
            for (const auto& item : std::filesystem::directory_iterator(directory)) {
                if (++entries > 65536) return Fail<std::vector<TreeFile>>("HLUA_INPUT_BUDGET", "地图来源目录超过65536项。");
                const auto attr = GetFileAttributesW(item.path().c_str());
                if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
                    return Fail<std::vector<TreeFile>>("HLUA_INPUT_PATH", "地图来源包含不可读条目或链接。");
                if (item.path().filename().wstring().starts_with(L'.')) continue;
                if (attr & FILE_ATTRIBUTE_DIRECTORY) { pending.push_back(item.path()); continue; }
                if (Text::LowerAscii(IO::PathUtf8(item.path().extension())) == ".ai")
                    return Fail<std::vector<TreeFile>>("HLUA_BOOTSTRAP_SOURCE_REFUSED", "目录构建不读取引导成员；请使用已有地图的产品透传路径。");
                auto name = IO::GenericPathUtf8(item.path().lexically_relative(absolute));
                const auto canonical = Resources::CanonicalImportPath(name);
                if (!canonical.Succeeded()) return {std::nullopt, canonical.diagnostics};
                if (!names.insert(*canonical.value).second) return Fail<std::vector<TreeFile>>("HLUA_INPUT_COLLISION", "地图来源有重名路径：" + name);
                const auto size = std::filesystem::file_size(item.path());
                if (size > 64u * 1024u * 1024u || size > 256u * 1024u * 1024u - total)
                    return Fail<std::vector<TreeFile>>("HLUA_INPUT_BUDGET", "地图来源超过单文件64 MiB或合计256 MiB预算。");
                auto sha = Sha256File(item.path());
                if (!sha.Succeeded()) return {std::nullopt, std::move(sha.diagnostics)};
                auto bytes = ReadVerifiedFile(item.path(), *sha.value, 64u * 1024u * 1024u);
                if (!bytes.Succeeded()) return {std::nullopt, std::move(bytes.diagnostics)};
                total += bytes.value->size(); files.push_back({item.path(), std::move(name), *sha.value, std::move(*bytes.value)});
            }
        }
    } catch (const std::exception& e) { return Fail<std::vector<TreeFile>>("HLUA_INPUT_IO", e.what()); }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return Output::Success(std::move(files));
}
Status VerifyTree(std::span<const TreeFile> files) {
    for (const auto& file : files) {
        const auto attr = GetFileAttributesW(file.source.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            return Status::Failure("HLua", "HLUA_INPUT_CHANGED", "地图来源身份已改变。");
        auto sha = Sha256File(file.source);
        if (!sha.Succeeded()) return {std::move(sha.diagnostics)};
        if (*sha.value != file.sha256) return Status::Failure("HLua", "HLUA_INPUT_CHANGED", "地图来源内容已改变：" + file.name);
    }
    return Status::Ok();
}

Result<BaseSnapshot> CaptureBase(const BuildRequest& request) {
    using Output = Result<BaseSnapshot>;
    BaseSnapshot output;
    if (!request.baseMap.empty()) {
        auto archive = MapArchiveSession::Open(request.baseMap);
        if (!archive.Succeeded()) return {std::nullopt, std::move(archive.diagnostics)};
        output.inputSha256 = archive.value->SourceSha256(); output.archive = std::move(*archive.value);
        return Output::Success(std::move(output));
    }
    auto tree = CaptureTree(request.project / "map");
    if (!tree.Succeeded()) return {std::nullopt, std::move(tree.diagnostics)};
    output.files = std::move(*tree.value);
    std::vector<MapArchiveMember> members;
    std::vector<std::byte> importsIni;
    bool settings = false;
    std::set<std::string> names;
    std::string identity = "hlua-map-tree-v1\n";
    const auto add = [&](std::string name, const std::vector<std::byte>& bytes) -> Status {
        const auto key = Resources::CanonicalImportPath(name);
        if (!key.Succeeded()) return {key.diagnostics};
        if (!names.insert(*key.value).second) return Status::Failure("HLua", "HLUA_MAP_COLLISION", "地图来源重复成员：" + name);
        members.push_back({std::move(name), bytes}); return Status::Ok();
    };
    try {
        for (const auto& file : output.files) {
            identity += file.name + '\n' + file.sha256 + '\n';
            Status status;
            if (file.name.starts_with("w3x/")) status = add(file.name.substr(4), file.bytes);
            else if (file.name.starts_with("resource/")) status = add(file.name.substr(9), file.bytes);
            else if (file.name == "slk/w3i.ini") {
                auto decoded = MapData::ParseLniMapSettings(file.bytes);
                if (!decoded.Succeeded()) return {std::nullopt, std::move(decoded.diagnostics)};
                if (!decoded.value->fullyParsed) return Fail<BaseSnapshot>("HLUA_W3I_INCOMPLETE", "w3i.ini 有未解释字段，未写出部分地图设置。");
                auto encoded = decoded.value->settings.Serialize();
                if (!encoded.Succeeded()) return {std::nullopt, std::move(encoded.diagnostics)};
                auto roundtrip = MapData::MapSettings::Parse(*encoded.value);
                if (!roundtrip.Succeeded()) return {std::nullopt, std::move(roundtrip.diagnostics)};
                status = add("war3map.w3i", *encoded.value); settings = true;
            } else if (file.name == "slk/imp.ini") importsIni = file.bytes;
            else if (file.name == "slk/misc.ini") {
                auto data = Text::ParseLniData(file.bytes);
                if (!data.Succeeded()) return {std::nullopt, std::move(data.diagnostics)};
                std::string profile;
                for (const auto& [section, value] : data.value->fields) {
                    profile += '[' + section + "]\r\n";
                    for (const auto& [field, scalar] : value.fields) {
                        if (field == "_parent") {
                            if (Scalar(scalar) != section) return Fail<BaseSnapshot>("HLUA_MISC_PARENT", "misc.ini 不支持非同名继承。");
                            continue;
                        }
                        const auto text = Scalar(scalar);
                        if (text.find_first_of("\r\n\0", 0, 3) != text.npos) return Fail<BaseSnapshot>("HLUA_MISC_VALUE", "misc.ini 的文本含换行或零字节。");
                        profile += field + '=' + text + "\r\n";
                    }
                }
                status = add("war3mapMisc.txt", Bytes(profile));
            } else if (file.name.starts_with("slk/")) {
                const auto type = ObjectData::ObjectTypeFromLniFileName(file.name.substr(4));
                if (!type) return Fail<BaseSnapshot>("HLUA_LNI_FILE_UNSUPPORTED", "未支持的地图表：" + file.name);
                auto objects = ParseF6ObjectIni(*type, {reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()}, file.name, request.limits);
                if (!objects.Succeeded()) return {std::nullopt, std::move(objects.diagnostics)};
                for (auto& object : *objects.value) output.f6Objects.push_back(std::move(object));
            } else return Fail<BaseSnapshot>("HLUA_MAP_TREE_UNSUPPORTED", "map 目录只接受 w3x、slk、resource 三种来源。");
            if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        }
        if (!settings || !names.contains("WAR3MAP.W3E")) return Fail<BaseSnapshot>("HLUA_BASE_INCOMPLETE", "工程缺少 w3i.ini 或 war3map.w3e。");
        auto imports = Resources::ImportTable::CreateEmpty();
        if (!importsIni.empty()) {
            const std::string text = "[imports]\n" + std::string(reinterpret_cast<const char*>(importsIni.data()), importsIni.size());
            auto parsed = Text::ParseLniData(Bytes(text));
            if (!parsed.Succeeded()) return {std::nullopt, std::move(parsed.diagnostics)};
            const auto& fields = parsed.value->fields.at("imports").fields;
            if (fields.size() != 1 || !fields.contains("import")) return Fail<BaseSnapshot>("HLUA_IMPORT_INCOMPLETE", "imp.ini 只接受 import 数组。");
            const auto& list = fields.at("import");
            if (list.kind != Text::LniDataValue::Kind::Table) return Fail<BaseSnapshot>("HLUA_IMPORT_INCOMPLETE", "imp.ini 的 import 不是数组。");
            for (std::size_t i = 1; i <= list.fields.size(); ++i) {
                const auto found = list.fields.find(std::to_string(i));
                if (found == list.fields.end() || found->second.kind != Text::LniDataValue::Kind::String)
                    return Fail<BaseSnapshot>("HLUA_IMPORT_INCOMPLETE", "imp.ini 必须是连续的路径文本数组。");
                const auto canonical = Resources::CanonicalImportPath(found->second.text);
                if (!canonical.Succeeded() || !names.contains(*canonical.value)) return Fail<BaseSnapshot>("HLUA_IMPORT_MISSING", "imp.ini 指向不存在或不安全的成员：" + found->second.text);
                const auto inserted = imports.Upsert(found->second.text);
                if (!inserted.Succeeded()) return {std::nullopt, inserted.diagnostics};
            }
        }
        auto encoded = imports.Serialize();
        if (!encoded.Succeeded()) return {std::nullopt, std::move(encoded.diagnostics)};
        const auto inserted = add("war3map.imp", *encoded.value);
        if (!inserted.Succeeded()) return {std::nullopt, inserted.diagnostics};
        auto sha = Sha256Text(identity);
        if (!sha.Succeeded()) return {std::nullopt, std::move(sha.diagnostics)};
        output.inputSha256 = *sha.value;
        auto seed = WriteVerifiedArchive(request.workspace / "lni-source.mpq", members);
        if (!seed.Succeeded()) return {std::nullopt, std::move(seed.diagnostics)};
        auto archive = MapArchiveSession::Open(seed.value->path);
        if (!archive.Succeeded()) return {std::nullopt, std::move(archive.diagnostics)};
        output.archive = std::move(*archive.value);
        return Output::Success(std::move(output));
    } catch (const std::exception& e) { return Fail<BaseSnapshot>("HLUA_BASE_READ", e.what()); }
}
Result<std::vector<std::string>> CaptureExistingIds(const BuildRequest& request) {
    std::set<std::string> ids;
    if (!request.baseMap.empty()) {
        auto archive = MapArchiveSession::OpenScriptAudit(request.baseMap);
        if (!archive.Succeeded()) return {std::nullopt, std::move(archive.diagnostics)};
        for (unsigned i = 0; i < 7; ++i) {
            const auto type = static_cast<ObjectData::ObjectType>(i);
            const auto member = ObjectData::NativeFileName(type);
            if (!archive.value->HasMember(member)) continue;
            auto bytes = archive.value->ReadMember(member);
            if (!bytes.Succeeded()) return {std::nullopt, std::move(bytes.diagnostics)};
            auto parsed = ObjectData::NativeObjectCodec(type).Parse(*bytes.value);
            const auto* document = parsed.Succeeded() ? dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
            if (!document || !document->IsStructurallySafe()) return {std::nullopt, std::move(parsed.diagnostics)};
            for (const auto& record : document->Objects()) ids.insert(record.key.rawcode);
        }
    } else if (std::filesystem::is_directory(request.project / "map/slk")) {
        auto files = CaptureTree(request.project / "map/slk");
        if (!files.Succeeded()) return {std::nullopt, std::move(files.diagnostics)};
        for (const auto& file : *files.value) {
            const auto type = ObjectData::ObjectTypeFromLniFileName(file.name);
            if (!type) continue;
            auto objects = ParseF6ObjectIni(*type, {reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()}, file.name, request.limits);
            if (!objects.Succeeded()) return {std::nullopt, std::move(objects.diagnostics)};
            for (const auto& object : *objects.value) if (!ids.insert(object.id).second)
                return Fail<std::vector<std::string>>("HLUA_ID_CONFLICT", "F6 各类表之间存在重复 ID：" + object.id);
        }
    }
    return Result<std::vector<std::string>>::Success({ids.begin(), ids.end()});
}
} // namespace War3Map::HLua::Detail
