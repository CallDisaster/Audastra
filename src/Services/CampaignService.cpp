#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <fstream>
#include <limits>
#include <windows.h>

namespace War3Map::Services {
namespace {
std::atomic_uint64_t identities{1};
constexpr std::size_t kMemberBudget = 512u * 1024u * 1024u;
constexpr std::size_t kSharedBudget = 64u * 1024u * 1024u;
std::string Key(std::string_view value) {
    const auto key = Resources::CanonicalImportPath(value);
    return key.value.value_or("");
}
Status Fail(std::string code, std::string message) { return Status::Failure("战役", std::move(code), std::move(message)); }
template<class T, class U> Result<T> Forward(const U& input) { Result<T> result; result.diagnostics = input.diagnostics; return result; }
bool IsMap(std::string_view value) { const auto key = Key(value); return key.ends_with(".W3X") || key.ends_with(".W3M"); }
bool IsResource(std::string_view value) {
    const auto key = Key(value);
    return !key.empty() && !key.starts_with('(') && !IsMap(key)
        && !(key.find('\\') == std::string::npos && (key.starts_with("WAR3CAMPAIGN") || key.starts_with("WAR3MAP")));
}
template<class T> auto Find(std::vector<T>& values, StableId id) {
    return std::find_if(values.begin(), values.end(), [id](const auto& entry) { return entry.id == id; });
}
template<class T> Status Move(std::vector<T>& values, StableId id, std::size_t target) {
    const auto found = Find(values, id);
    if (found == values.end() || target >= values.size()) return Fail("CAMPAIGN_MOVE_INVALID", "条目或目标顺序已失效。");
    auto entry = std::move(*found); values.erase(found); values.insert(values.begin() + static_cast<std::ptrdiff_t>(target), std::move(entry));
    return Status::Ok();
}
Result<std::vector<std::byte>> CampaignHeader(std::span<const std::byte> previous,
    std::string_view name, std::uint32_t flags) {
    using Output = Result<std::vector<std::byte>>;
    if (name.size() > 495 || name.find('\0') != std::string_view::npos)
        return Output::Failure("战役", "CAMPAIGN_HEADER_NAME", "战役名称超过经典 512 字节文件头容量。");
    std::vector<std::byte> result(512);
    std::size_t oldEnd = 8;
    if (!previous.empty()) {
        if (previous.size() != 512 || previous[0] != std::byte{'H'} || previous[1] != std::byte{'M'}
            || previous[2] != std::byte{'3'} || previous[3] != std::byte{'W'})
            return Output::Failure("战役", "CAMPAIGN_HEADER_UNKNOWN", "未知战役外头不能猜测重写；原字节保留。");
        for (std::size_t i = 4; i < 8; ++i) if (previous[i] != std::byte{})
            return Output::Failure("战役", "CAMPAIGN_HEADER_UNKNOWN", "战役外头版本不是经典布局，拒绝改写。");
        auto terminator = std::find(previous.begin() + 8, previous.end(), std::byte{});
        oldEnd = static_cast<std::size_t>(terminator - previous.begin()) + 9;
        if (terminator == previous.end() || oldEnd > 512)
            return Output::Failure("战役", "CAMPAIGN_HEADER_TRUNCATED", "战役外头名称或尾部字段不完整。");
        const auto marker = oldEnd - 4;
        if (previous[marker] != std::byte{1} || previous[marker + 1] != std::byte{}
            || previous[marker + 2] != std::byte{} || previous[marker + 3] != std::byte{})
            return Output::Failure("战役", "CAMPAIGN_HEADER_UNKNOWN", "战役外头末字段不是经典战役标记 1，拒绝猜测。");
        result.assign(previous.begin(), previous.end());
        // 新名称只能占用已知字段或空填充，不能覆盖不明外头扩展。
        const auto newEnd = name.size() + 17;
        for (std::size_t i = oldEnd; i < newEnd; ++i) if (previous[i] != std::byte{})
            return Output::Failure("战役", "CAMPAIGN_HEADER_PADDING", "新名称会覆盖外头未知填充，保存已停止。");
        std::fill(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(oldEnd), std::byte{});
    }
    result[0] = std::byte{'H'}; result[1] = std::byte{'M'}; result[2] = std::byte{'3'}; result[3] = std::byte{'W'};
    for (std::size_t i = 0; i < name.size(); ++i) result[8 + i] = static_cast<std::byte>(name[i]);
    const auto flagOffset = 9 + name.size();
    for (std::size_t i = 0; i < 4; ++i) result[flagOffset + i] = static_cast<std::byte>((flags >> (8 * i)) & 255);
    result[flagOffset + 4] = std::byte{1};
    return Output::Success(std::move(result));
}
} // namespace

CampaignSession::CampaignSession() : identity_(identities.fetch_add(1)) {}

std::shared_ptr<CampaignSession> CampaignSession::Create() {
    auto session = std::shared_ptr<CampaignSession>(new CampaignSession);
    const auto scopes = session->Scopes(session->state_);
    if (scopes.value) (void)session->edits_.Reset(*scopes.value);
    return session;
}

Result<std::shared_ptr<CampaignSession>> CampaignSession::Open(const std::filesystem::path& path) {
    using Output = Result<std::shared_ptr<CampaignSession>>;
    auto opened = MapArchiveSession::Open(path);
    if (!opened.Succeeded() || !opened.value) return Forward<std::shared_ptr<CampaignSession>>(opened);
    const std::array<std::string, 1> names{"war3campaign.w3f"};
    const auto exact = opened.value->CaptureExactMembers(names);
    if (!exact.Succeeded() || !exact.value) return Forward<std::shared_ptr<CampaignSession>>(exact);
    if (!exact.value->front().present) return Output::Failure("战役", "CAMPAIGN_INFO_MISSING", "归档缺少 war3campaign.w3f。");
    const auto parsed = Campaign::ParseInfo(exact.value->front().bytes);
    if (!parsed.Succeeded() || !parsed.value) return Forward<std::shared_ptr<CampaignSession>>(parsed);
    auto session = std::shared_ptr<CampaignSession>(new CampaignSession);
    session->archive_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    session->state_.info = *parsed.value; session->diagnostics_ = parsed.diagnostics;
    for (const auto& button : parsed.value->buttons) session->nextId_ = (std::max)(session->nextId_, button.id + 1);
    for (const auto& map : parsed.value->maps) session->nextId_ = (std::max)(session->nextId_, map.id + 1);
    const auto scopes = session->Scopes(session->state_);
    if (!scopes.Succeeded() || !scopes.value) return Forward<std::shared_ptr<CampaignSession>>(scopes);
    const auto reset = session->edits_.Reset(*scopes.value, *scopes.value, *scopes.value);
    if (!reset.Succeeded()) return Forward<std::shared_ptr<CampaignSession>>(reset);
    const auto status = session->Validate(session->state_);
    session->diagnostics_.insert(session->diagnostics_.end(), status.diagnostics.begin(), status.diagnostics.end());
    return Output::Success(std::move(session));
}

CampaignRevision CampaignSession::Revision() const { return {identity_, edits_.Revision()}; }
const std::filesystem::path& CampaignSession::SourcePath() const noexcept {
    static const std::filesystem::path empty;
    return archive_ ? archive_->SourcePath() : empty;
}
Status CampaignSession::Check(CampaignRevision expected) const {
    return expected == Revision() ? Status::Ok() : Fail("CAMPAIGN_REVISION_STALE", "战役文档或修订已变化，请重新查询后再编辑。");
}
std::string CampaignSession::ObjectMember(ObjectData::ObjectType type) {
    auto name = std::string(ObjectData::NativeFileName(type));
    return "war3campaign" + name.substr(std::string_view("war3map").size());
}
bool CampaignSession::Exists(const State& state, std::string_view name) const {
    const auto key = Key(name);
    if (state.removals.contains(key)) return false;
    if (state.replacements.contains(key)) return true;
    return archive_ && archive_->HasMember(name);
}
Result<std::vector<std::byte>> CampaignSession::Read(const State& state, std::string_view name) const {
    const auto key = Key(name);
    if (key.empty() || state.removals.contains(key))
        return Result<std::vector<std::byte>>::Failure("战役", "CAMPAIGN_MEMBER_MISSING", "战役成员不存在或路径无效。");
    if (const auto found = state.replacements.find(key); found != state.replacements.end())
        return Result<std::vector<std::byte>>::Success(*found->second.bytes);
    if (archive_) return archive_->ReadMember(name);
    return Result<std::vector<std::byte>>::Failure("战役", "CAMPAIGN_MEMBER_MISSING", "战役成员不存在。");
}
Status CampaignSession::Put(State& state, std::string name, std::span<const std::byte> bytes) const {
    const auto key = Key(name);
    if (key.empty() || name.size() > 1024 || bytes.size() > kMemberBudget) return Fail("CAMPAIGN_MEMBER_BUDGET", "成员路径无效或正文超过 512 MiB。");
    const auto hash = Sha256(bytes);
    if (!hash.Succeeded() || !hash.value) return {hash.diagnostics};
    state.removals.erase(key);
    state.replacements[key] = {std::move(name), *hash.value, std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end())};
    return Status::Ok();
}

Result<EditScopeSnapshot> CampaignSession::Scopes(const State& state) const {
    const auto encoded = Campaign::SerializeInfo(state.info);
    if (!encoded.Succeeded() || !encoded.value) return Forward<EditScopeSnapshot>(encoded);
    const auto hash = Sha256(*encoded.value);
    if (!hash.Succeeded() || !hash.value) return Forward<EditScopeSnapshot>(hash);
    EditScopeSnapshot scopes{{"campaign/info", *hash.value}};
    for (const auto& [key, member] : state.replacements) scopes["campaign/member/" + key] = member.sha256;
    for (const auto& key : state.removals) scopes["campaign/removed/" + key] = "removed";
    return Result<EditScopeSnapshot>::Success(std::move(scopes));
}
Status CampaignSession::Validate(const State& state) const {
    const auto encoded = Campaign::SerializeInfo(state.info);
    if (!encoded.Succeeded()) return {encoded.diagnostics};
    std::set<std::string> paths; std::set<StableId> ids;
    for (const auto& map : state.info.maps) {
        if (!IsMap(map.path) || !paths.insert(Key(map.path)).second || !map.id || !ids.insert(map.id).second)
            return Fail("CAMPAIGN_MAP_IDENTITY", "地图列表路径或身份重复、无效。");
        if (!Exists(state, map.path)) return Fail("CAMPAIGN_MAP_MISSING", "地图列表引用的成员不存在：" + map.path);
    }
    for (const auto& button : state.info.buttons) {
        if (!button.id || !ids.insert(button.id).second || !paths.contains(Key(button.mapPath)))
            return Fail("CAMPAIGN_BUTTON_TARGET", "菜单按钮的身份无效或目标不在地图列表中：" + button.mapPath);
    }
    return Status::Ok();
}
Status CampaignSession::Publish(State next, std::string title, EditOperation operation) {
    const auto scopes = Scopes(next);
    if (!scopes.Succeeded() || !scopes.value) return {scopes.diagnostics};
    const auto observed = edits_.Observe(*scopes.value, std::move(title), operation);
    if (!observed.Succeeded()) return observed;
    state_ = std::move(next);
    return Status::Ok();
}
Status CampaignSession::UpdateImports(State& state, std::span<const std::string> add,
    std::span<const std::string> remove) const {
    auto table = Resources::ImportTable::CreateEmpty(Resources::ImportScope::Campaign);
    if (Exists(state, "war3campaign.imp")) {
        const auto bytes = Read(state, "war3campaign.imp");
        if (!bytes.Succeeded() || !bytes.value) return {bytes.diagnostics};
        auto parsed = Resources::ImportTable::Parse(*bytes.value, Resources::ImportScope::Campaign);
        if (!parsed.Succeeded() || !parsed.value) return {parsed.diagnostics};
        table = std::move(*parsed.value);
    }
    const auto exists = [&](std::string_view path) { return Exists(state_, path) || Exists(state, path); };
    for (const auto& path : remove) { const auto result = table.Remove(path, exists); if (!result.Succeeded()) return result; }
    for (const auto& path : add) { const auto result = table.Upsert(path, exists); if (!result.Succeeded()) return result; }
    const auto bytes = table.Serialize();
    return bytes.value ? Put(state, "war3campaign.imp", *bytes.value) : Status{bytes.diagnostics};
}

Result<StableId> CampaignSession::Apply(CampaignRevision expected, const CampaignCommand& command) {
    const auto bound = Check(expected); if (!bound.Succeeded()) return Forward<StableId>(bound);
    try {
        State next = state_; StableId resultId = command.id;
        auto map = Find(next.info.maps, command.id);
        auto button = Find(next.info.buttons, command.id);
        Status status;
        switch (command.operation) {
        case CampaignOperation::SetInfo: {
            auto maps = std::move(next.info.maps); auto buttons = std::move(next.info.buttons);
            auto trailing = std::move(next.info.trailing);
            next.info = command.info; next.info.maps = std::move(maps); next.info.buttons = std::move(buttons);
            next.info.trailing = std::move(trailing); // 未知尾部不能通过设置命令删除。
            break;
        }
        case CampaignOperation::AddMap:
        case CampaignOperation::ReplaceMap: {
            const bool adding = command.operation == CampaignOperation::AddMap;
            if ((!adding && map == next.info.maps.end()) || (adding && (!IsMap(command.path) || Exists(next, command.path))))
                return Forward<StableId>(Fail("CAMPAIGN_MAP_CONFLICT", "章节身份无效、路径已存在或不是 .w3x/.w3m。"));
            auto source = MapArchiveSession::Open(command.mapSource);
            if (!source.Succeeded() || !source.value) return Forward<StableId>(source);
            if (!source.value->HasMember("war3map.w3i")) return Forward<StableId>(Fail("CAMPAIGN_CHAPTER_NOT_MAP", "章节归档缺少 war3map.w3i。"));
            const auto bytes = IO::ReadFileBytes(command.mapSource, kMemberBudget);
            if (!bytes.Succeeded() || !bytes.value) return Forward<StableId>(bytes);
            const auto hash = Sha256(*bytes.value);
            if (!hash.value || *hash.value != source.value->SourceSha256())
                return Forward<StableId>(Fail("CAMPAIGN_CHAPTER_CHANGED", "读取章节期间源文件变化，未加入战役。"));
            status = Put(next, adding ? command.path : map->path, *bytes.value);
            if (adding) { resultId = nextId_; next.info.maps.push_back({resultId, {}, command.path}); }
            break;
        }
        case CampaignOperation::RemoveMap:
            if (map == next.info.maps.end()) return Forward<StableId>(Fail("CAMPAIGN_MAP_MISSING", "章节身份不存在。"));
            if (std::any_of(next.info.buttons.begin(), next.info.buttons.end(), [&](const auto& row) { return Key(row.mapPath) == Key(map->path); }))
                return Forward<StableId>(Fail("CAMPAIGN_MAP_REFERENCED", "菜单仍引用此章节，请先删除或修改菜单按钮；脚本中的动态切图引用也须作者核对。"));
            next.removals.insert(Key(map->path)); next.replacements.erase(Key(map->path)); next.info.maps.erase(map); break;
        case CampaignOperation::MoveMap: status = Move(next.info.maps, command.id, command.position); break;
        case CampaignOperation::AddButton: {
            auto entry = command.button; entry.id = nextId_; resultId = entry.id; next.info.buttons.push_back(std::move(entry)); break;
        }
        case CampaignOperation::UpdateButton:
            if (button == next.info.buttons.end()) return Forward<StableId>(Fail("CAMPAIGN_BUTTON_MISSING", "菜单身份不存在。"));
            *button = command.button; button->id = command.id; break;
        case CampaignOperation::RemoveButton:
            if (button == next.info.buttons.end()) return Forward<StableId>(Fail("CAMPAIGN_BUTTON_MISSING", "菜单身份不存在。"));
            next.info.buttons.erase(button); break;
        case CampaignOperation::MoveButton: status = Move(next.info.buttons, command.id, command.position); break;
        case CampaignOperation::ReplaceSharedData:
        case CampaignOperation::ImportResource:
        case CampaignOperation::RemoveResource: {
            if (command.replacements.size() + command.removals.size() > 4096)
                return Forward<StableId>(Fail("CAMPAIGN_BATCH_BUDGET", "单次战役资源修改最多 4096 项。"));
            std::vector<std::string> addResources, removeResources; std::set<std::string> seen;
            std::size_t bytes = 0;
            for (const auto& replacement : command.replacements) {
                bytes += replacement.data.size();
                if (bytes > kSharedBudget || !seen.insert(Key(replacement.name)).second)
                    return Forward<StableId>(Fail("CAMPAIGN_SHARED_BUDGET", "共享数据批次超过 64 MiB 或路径重复。"));
                bool object = false;
                for (int i = 0; i < 7; ++i) {
                    const auto type = static_cast<ObjectData::ObjectType>(i);
                    if (Key(replacement.name) != Key(ObjectMember(type))) continue;
                    if (command.operation != CampaignOperation::ReplaceSharedData) break;
                    const auto parsed = ObjectData::NativeObjectCodec(type).Parse(replacement.data);
                    if (!parsed.Succeeded() || !parsed.document) return Forward<StableId>(parsed);
                    const auto valid = ObjectData::NativeObjectCodec(type).Validate(*parsed.document);
                    if (!valid.Succeeded()) return Forward<StableId>(valid);
                    object = true;
                }
                if (!object && !IsResource(replacement.name)) return Forward<StableId>(Fail("CAMPAIGN_SHARED_PATH", "该成员由战役结构管理，不能作为导入资源覆盖。"));
                if (!object) addResources.push_back(replacement.name);
                status = Put(next, replacement.name, replacement.data); if (!status.Succeeded()) return Forward<StableId>(status);
            }
            for (const auto& removed : command.removals) {
                if (!IsResource(removed) || !seen.insert(Key(removed)).second || !Exists(next, removed))
                    return Forward<StableId>(Fail("CAMPAIGN_REMOVE_RESOURCE", "资源不存在、路径冲突或属于战役结构。"));
                next.replacements.erase(Key(removed)); next.removals.insert(Key(removed)); removeResources.push_back(removed);
            }
            if (!addResources.empty() || !removeResources.empty()) status = UpdateImports(next, addResources, removeResources);
            break;
        }
        }
        if (!status.Succeeded()) return Forward<StableId>(status);
        status = Validate(next); if (!status.Succeeded()) return Forward<StableId>(status);
        const auto beforeScopes = Scopes(state_), afterScopes = Scopes(next);
        if (!beforeScopes.value || !afterScopes.value) return Forward<StableId>(!beforeScopes.value ? beforeScopes : afterScopes);
        if (*beforeScopes.value == *afterScopes.value) return Result<StableId>::Success(resultId);
        // 历史只复制元数据和 shared_ptr，不复制大章节载荷；分支编辑不重用已分配身份。
        Change change{state_, next, "编辑战役"};
        history_.reserve(cursor_ + 1);
        status = Publish(std::move(next), change.title, EditOperation::Edit);
        if (!status.Succeeded()) return Forward<StableId>(status);
        history_.resize(cursor_); history_.push_back(std::move(change)); ++cursor_;
        if (resultId == nextId_) ++nextId_;
        // 限制保留的事务数；每份章节正文仍只由共享快照拥有一次。
        if (history_.size() > 64) { history_.erase(history_.begin()); --cursor_; }
        return Result<StableId>::Success(resultId);
    } catch (const std::bad_alloc&) { return Result<StableId>::Failure("战役", "CAMPAIGN_MEMORY", "战役事务内存不足，未发布修改。"); }
}

Status CampaignSession::Undo(CampaignRevision expected) {
    const auto check = Check(expected); if (!check.Succeeded()) return check;
    if (!CanUndo()) return Fail("CAMPAIGN_UNDO_EMPTY", "没有可撤销的战役编辑。");
    const auto status = Publish(history_[cursor_ - 1].before, "撤销战役编辑", EditOperation::Undo);
    if (status.Succeeded()) --cursor_;
    return status;
}
Status CampaignSession::Redo(CampaignRevision expected) {
    const auto check = Check(expected); if (!check.Succeeded()) return check;
    if (!CanRedo()) return Fail("CAMPAIGN_REDO_EMPTY", "没有可重做的战役编辑。");
    const auto status = Publish(history_[cursor_].after, "重做战役编辑", EditOperation::Redo);
    if (status.Succeeded()) ++cursor_;
    return status;
}
Result<CampaignPage> CampaignSession::Members(CampaignRevision expected, std::size_t offset, std::size_t limit) const {
    const auto check = Check(expected); if (!check.Succeeded()) return Forward<CampaignPage>(check);
    if (limit == 0 || limit > 256) return Result<CampaignPage>::Failure("战役", "CAMPAIGN_QUERY_LIMIT", "查询页大小必须在 1 至 256 之间。");
    std::map<std::string, CampaignMemberInfo> entries;
    if (archive_) for (const auto& member : archive_->Members())
        if (!state_.removals.contains(Key(member.name))) entries[Key(member.name)] = {member.name, member.data.size(), false};
    for (const auto& [key, member] : state_.replacements) entries[key] = {member.name, member.bytes->size(), true};
    CampaignPage page{Revision(), entries.size(), (std::min)(offset, entries.size()), {}};
    std::size_t index = 0;
    for (const auto& [key, entry] : entries) {
        (void)key;
        if (index++ < offset) continue;
        if (page.members.size() == limit) break;
        page.members.push_back(entry); ++page.nextOffset;
    }
    return Result<CampaignPage>::Success(std::move(page));
}
Result<std::vector<std::byte>> CampaignSession::ReadMember(CampaignRevision expected, std::string_view path) const {
    const auto check = Check(expected); if (!check.Succeeded()) return Forward<std::vector<std::byte>>(check);
    if (Key(path) == "WAR3CAMPAIGN.W3F") return Campaign::SerializeInfo(state_.info);
    return Read(state_, path);
}
Status CampaignSession::ExportMember(CampaignRevision expected, std::string_view path, const std::filesystem::path& target) const {
    const auto bytes = ReadMember(expected, path);
    if (!bytes.Succeeded() || !bytes.value) return {bytes.diagnostics};
    // CREATE_NEW 保证绝不覆盖已有作者文件；失败清理仅本次拥有的输出。
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Fail("CAMPAIGN_EXPORT_TARGET", "导出目标必须不存在且父目录可写。");
    DWORD written = 0;
    const bool success = bytes.value->size() <= MAXDWORD && WriteFile(file, bytes.value->data(), static_cast<DWORD>(bytes.value->size()), &written, nullptr)
        && written == bytes.value->size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!success) { DeleteFileW(target.c_str()); return Fail("CAMPAIGN_EXPORT_WRITE", "导出未完成，本次输出已清理。"); }
    return Status::Ok();
}
Result<CampaignSaveReceipt> CampaignSession::Save(CampaignRevision expected,
    const std::filesystem::path& target, bool inPlace, const InPlaceSaveOptions& backup) {
    const auto check = Check(expected); if (!check.Succeeded()) return Forward<CampaignSaveReceipt>(check);
    const auto valid = Validate(state_); if (!valid.Succeeded()) return Forward<CampaignSaveReceipt>(valid);
    if (inPlace && !archive_) return Result<CampaignSaveReceipt>::Failure("战役", "CAMPAIGN_SAVE_NEW", "新战役需要先另存为 .w3n。");
    const auto output = inPlace ? archive_->SourcePath() : target;
    auto extension = output.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".w3n") return Result<CampaignSaveReceipt>::Failure("战役", "CAMPAIGN_SAVE_EXTENSION", "战役目标必须使用 .w3n 扩展名。");
    const auto info = Campaign::SerializeInfo(state_.info);
    if (!info.Succeeded() || !info.value) return Forward<CampaignSaveReceipt>(info);
    const auto name = ResolveText(state_.info.name);
    if (!name.Succeeded() || !name.value) return Forward<CampaignSaveReceipt>(name);
    const auto prefix = CampaignHeader(archive_ ? archive_->Prefix() : std::span<const std::byte>{}, *name.value, state_.info.flags);
    if (!prefix.Succeeded() || !prefix.value) return Forward<CampaignSaveReceipt>(prefix);
    auto savingArchive = archive_;
    if (archive_ && (archive_->Prefix().size() != prefix.value->size()
        || !std::equal(prefix.value->begin(), prefix.value->end(), archive_->Prefix().begin()))) {
        auto prepared = archive_->WithLeadingHeader(*prefix.value);
        if (!prepared.Succeeded() || !prepared.value) return Forward<CampaignSaveReceipt>(prepared);
        savingArchive = std::make_shared<MapArchiveSession>(std::move(*prepared.value));
    }
    std::vector<MemberReplacement> replacements{{"war3campaign.w3f", *info.value}};
    for (const auto& [key, member] : state_.replacements) {
        if (key != "WAR3CAMPAIGN.W3F") replacements.push_back({member.name, *member.bytes});
    }
    std::vector<std::string> removals(state_.removals.begin(), state_.removals.end());
    // 在触碰输出前，冻结历史中会受新基线影响的原成员。保存后撤销仍可恢复删掉的章节或旧资源。
    // 同一原成员只捕获一次，各历史状态共享该载荷；未触及的大章节不读取。
    auto nextHistory = history_;
    auto nextState = state_;
    std::set<std::string> touched;
    const auto collect = [&](const State& state) {
        for (const auto& [key, unused] : state.replacements) { (void)unused; touched.insert(key); }
        touched.insert(state.removals.begin(), state.removals.end());
    };
    collect(state_);
    for (const auto& change : history_) { collect(change.before); collect(change.after); }
    std::map<std::string, OwnedMember> original;
    if (archive_) for (const auto& key : touched) {
        if (!archive_->HasMember(key)) continue;
        const auto bytes = archive_->ReadMember(key);
        if (!bytes.Succeeded() || !bytes.value) return Forward<CampaignSaveReceipt>(bytes);
        const auto hash = Sha256(*bytes.value);
        if (!hash.Succeeded() || !hash.value) return Forward<CampaignSaveReceipt>(hash);
        original.emplace(key, OwnedMember{key, *hash.value,
            std::make_shared<const std::vector<std::byte>>(std::move(*bytes.value))});
    }
    const auto materialize = [&](State& state) {
        for (const auto& key : touched) {
            if (state.removals.contains(key) || state.replacements.contains(key)) continue;
            if (const auto found = original.find(key); found != original.end()) state.replacements.emplace(key, found->second);
            else state.removals.insert(key);
        }
    };
    materialize(nextState);
    for (auto& change : nextHistory) { materialize(change.before); materialize(change.after); }
    const auto ticket = edits_.BeginSave(EditSaveTarget::Map);
    if (!ticket.Succeeded() || !ticket.value) return Forward<CampaignSaveReceipt>(ticket);
    CampaignSaveReceipt receipt; Result<ArchiveSaveResult> written;
    if (archive_ && inPlace) {
        const auto result = savingArchive->SaveInPlace(replacements, removals, backup);
        if (!result.Succeeded() || !result.value) { (void)edits_.CompleteSave(*ticket.value, false); return Forward<CampaignSaveReceipt>(result); }
        receipt.backup = result.value->backup; written = Result<ArchiveSaveResult>::Success(result.value->written);
    } else if (archive_) written = savingArchive->SaveCandidate(output, replacements, removals);
    else {
        written = WriteVerifiedArchive(output, replacements, *prefix.value);
    }
    if (!written.Succeeded() || !written.value) { (void)edits_.CompleteSave(*ticket.value, false); return Forward<CampaignSaveReceipt>(written); }
    receipt.written = *written.value;
    const auto rebased = archive_ ? savingArchive->AfterSave(*written.value, replacements, removals)
        : MapArchiveSession::Open(written.value->path);
    if (!rebased.Succeeded() || !rebased.value) { (void)edits_.CompleteSave(*ticket.value, false); return Forward<CampaignSaveReceipt>(rebased); }
    (void)edits_.CompleteSave(*ticket.value, true);
    archive_ = std::make_shared<MapArchiveSession>(*rebased.value);
    state_ = std::move(nextState); history_ = std::move(nextHistory);
    const auto scopes = Scopes(state_);
    if (scopes.value) (void)edits_.Reset(*scopes.value, *scopes.value, *scopes.value);
    identity_ = identities.fetch_add(1); // 保存后的新来源拒绝旧票据。
    return Result<CampaignSaveReceipt>::Success(std::move(receipt));
}
Result<std::string> CampaignSession::ResolveText(std::string_view text) const {
    if (!text.starts_with("TRIGSTR_")) return Result<std::string>::Success(std::string(text));
    const auto bytes = Read(state_, "war3campaign.wts");
    if (!bytes.Succeeded() || !bytes.value) return Forward<std::string>(bytes);
    const auto table = MapData::ParseStringTable(*bytes.value);
    if (!table.Succeeded() || !table.value) return Forward<std::string>(table);
    return table.value->Resolve(text);
}
} // namespace War3Map::Services
