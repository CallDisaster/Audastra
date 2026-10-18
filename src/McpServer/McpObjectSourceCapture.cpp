#include "McpObjectSourceCapture.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/ObjectData/ObjectImport.h"

#include <algorithm>
#include <cctype>

namespace War3Map::Mcp {
namespace {
using namespace ObjectData;
constexpr std::size_t kMemberBudget = 64U * 1024U * 1024U;
constexpr std::size_t kMapBudget = 256U * 1024U * 1024U;
constexpr std::size_t kGameBudget = 128U * 1024U * 1024U;
using StateResult = Result<std::unique_ptr<McpObjectWorkspaceState>>;

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}
std::string Utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
StateResult Fail(const std::vector<Diagnostic>& diagnostics) { return {{}, diagnostics}; }
StateResult Fail(std::string code, std::string message) {
    return Fail(Status::Failure("MCP.Object", std::move(code), std::move(message)).diagnostics);
}
Result<std::shared_ptr<const LniObjectDocument>> ParseLni(ObjectType type, std::span<const std::byte> bytes) {
    auto parsed = LniObjectCodec(type).Parse(bytes);
    const auto* document = dynamic_cast<const LniObjectDocument*>(parsed.document.get());
    if (!parsed.Succeeded() || !document || !document->IsStructurallySafe()) {
        if (!parsed.diagnostics.empty()) return {{}, parsed.diagnostics};
        return {{}, Status::Failure("MCP.Object", "OBJECT_SOURCE_INVALID", "物编来源不能建立完整可编辑 LNI 文档。").diagnostics};
    }
    return Result<std::shared_ptr<const LniObjectDocument>>::Success(std::make_shared<const LniObjectDocument>(*document));
}
} // namespace

StateResult CaptureObjectWorkspaceState(
    const Services::ObjectCatalogSources& sources, const ObjectMemberSource& members, MapDocument map) {
    auto game = Game::GameDataSource::MountStrict(sources.gameRoot);
    if (!game.Succeeded()) return Fail(game.diagnostics);
    auto effective = Services::LoadEffectiveObjectCatalog(sources);
    if (!effective.Succeeded()) return Fail(effective.diagnostics);
    auto state = std::make_unique<McpObjectWorkspaceState>(
        std::make_shared<const EffectiveObjectCatalog>(std::move(*effective.value)));
    std::size_t gameBytes = 0;
    Status gameRead = Status::Ok();
    const StandardObjectCatalog::FileReader reader = [&](const std::string& name) -> Result<std::vector<std::byte>> {
        auto existing = state->gameInputs.find(name);
        if (existing == state->gameInputs.end()) {
            McpObjectWorkspaceState::GameInput input;
            if (game.value->HasFile(name)) {
                auto value = game.value->ReadFileBoundedWithSource(name, CatalogLoadOptions{}.maximumFileBytes);
                if (!value.Succeeded()) { gameRead.diagnostics = value.diagnostics; return {{}, value.diagnostics}; }
                if (value.value->bytes.size() > kGameBudget - gameBytes) {
                    gameRead = Status::Failure("MCP.Object", "OBJECT_GAME_SNAPSHOT_BUDGET", "游戏字段元数据的实际读取超过 128 MiB 累积预算。");
                    return {{}, gameRead.diagnostics};
                }
                gameBytes += value.value->bytes.size();
                input.archive = std::move(value.value->archiveName);
                input.bytes = std::move(value.value->bytes);
            }
            existing = state->gameInputs.emplace(name, std::move(input)).first;
        }
        if (!existing->second.bytes) return Result<std::vector<std::byte>>::Failure("MCP.Object",
            "OBJECT_GAME_MEMBER_MISSING", "游戏来源明确缺失：" + name);
        return Result<std::vector<std::byte>>::Success(*existing->second.bytes);
    };
    // 字段定义仍只信任宿主游戏来源，地图内的完整 SLK 只提供对象数据。
    auto schema = ObjectFieldSchema::Load(reader);
    if (!gameRead.Succeeded()) return Fail(gameRead.diagnostics);
    if (!schema.Succeeded()) return Fail(schema.diagnostics);
    state->schema = std::move(*schema.value);
    std::size_t mapBytes = 0;
    const auto readMember = [&](const std::filesystem::path& path) -> Result<std::vector<std::byte>> {
        auto bytes = members.read(path, kMemberBudget);
        if (!bytes.Succeeded()) return bytes;
        if (bytes.value->size() > kMapBudget - mapBytes)
            return Result<std::vector<std::byte>>::Failure("MCP.Object", "OBJECT_MAP_SNAPSHOT_BUDGET", "地图物编与脚本提示超过 256 MiB 累积预算。");
        mapBytes += bytes.value->size();
        return bytes;
    };
    for (std::size_t index = 0; index < state->mapInputs.size(); ++index) {
        const auto type = static_cast<ObjectType>(index);
        auto& input = state->mapInputs[index];
        std::vector<std::filesystem::path> lniPaths, nativePaths;
        for (const auto& file : members.files) {
            const auto name = Lower(Utf8(file));
            const auto lni = std::string(LniFileName(type));
            const auto native = std::string(NativeFileName(type));
            if (name == lni || name == "table/" + lni) lniPaths.push_back(file);
            if (name == native || name == "map/" + native) nativePaths.push_back(file);
        }
        if (lniPaths.size() > 1 || nativePaths.size() > 1)
            return Fail("OBJECT_SOURCE_AMBIGUOUS", "同类物编存在多个 root/map/table 来源：" + std::string(ToString(type)));
        if (!nativePaths.empty()) {
            auto bytes = readMember(nativePaths.front());
            if (!bytes.Succeeded()) return Fail(bytes.diagnostics);
            const auto parsed = NativeObjectCodec(type).Parse(*bytes.value);
            const auto* native = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
            if (!parsed.Succeeded() || !native || !native->IsStructurallySafe()) return Fail(parsed.diagnostics);
            input.native = *native;
            input.nativeBytes = std::move(*bytes.value);
        }
        if (!lniPaths.empty()) {
            input.sourcePresent = true;
            input.relativePath = lniPaths.front();
            auto bytes = readMember(input.relativePath);
            if (!bytes.Succeeded()) return Fail(bytes.diagnostics);
            auto parsed = ParseLni(type, *bytes.value);
            if (!parsed.Succeeded()) return Fail(parsed.diagnostics);
            input.initial = *parsed.value;
            input.sourceBytes = std::move(*bytes.value);
            if (input.native) {
                auto projected = *input.native;
                const auto status = ApplyLniChangesToNative(projected, *input.initial, &state->schema, &state->standard);
                if (!status.Succeeded()) return Fail(status.diagnostics);
                const auto encoded = NativeObjectCodec(type).Serialize(projected);
                if (!encoded.Succeeded()) return Fail(encoded.diagnostics);
                if (*encoded.value != *input.nativeBytes)
                    return Fail("OBJECT_SOURCE_PROJECTION_CONFLICT", "LNI 与保留的 native 来源不一致；请先明确生效来源：" + Utf8(input.relativePath));
            }
        } else if (input.native) {
            input.sourcePresent = true;
            input.relativePath = nativePaths.front();
            input.nativeSource = true;
            input.sourceBytes = *input.nativeBytes;
            auto bytes = ConvertNativeObjectDataToLni(type, *input.nativeBytes);
            if (!bytes.Succeeded()) return Fail(bytes.diagnostics);
            auto parsed = ParseLni(type, *bytes.value);
            if (!parsed.Succeeded()) return Fail(parsed.diagnostics);
            input.initial = *parsed.value;
        } else {
            input.relativePath = std::filesystem::path("table") / std::string(LniFileName(type));
            auto parsed = ParseLni(type, {});
            if (!parsed.Succeeded()) return Fail(parsed.diagnostics);
            input.initial = *parsed.value;
        }
        input.committed = input.initial;
    }
    state->map = std::make_unique<MapDocument>(std::move(map));
    // 只读实际主脚本供删除提示；.ai 不读取、不检查。
    for (const auto& file : members.files) {
        const auto name = Lower(Utf8(file));
        if (name != "war3map.j" && name != "map/war3map.j" && name != "scripts/war3map.j" &&
            name != "war3map.lua" && name != "map/war3map.lua" && name != "scripts/war3map.lua") continue;
        auto bytes = readMember(file);
        if (!bytes.Succeeded()) return Fail(bytes.diagnostics);
        state->scriptNames.push_back(Utf8(file));
        state->scriptBytes.push_back(std::move(*bytes.value));
    }
    return StateResult::Success(std::move(state));
}

Result<Services::ObjectCommandContext> CommittedObjectContext(const McpObjectWorkspaceState& state, ObjectType type) {
    const auto index = static_cast<std::size_t>(type);
    if (index >= state.mapInputs.size())
        return {{}, Status::Failure("MCP.Object", "OBJECT_KIND_INVALID", "物编类别无效。").diagnostics};
    if (state.standard.Count(type) == 0)
        return {{}, Status::Failure("MCP.Object", "OBJECT_STANDARD_CATEGORY_MISSING",
            "实际生效目录没有此类对象：" + std::string(ToString(type))).diagnostics};
    Services::ObjectCommandContext context;
    context.type = type;
    context.standard = &state.standard;
    context.schema = &state.schema;
    for (std::size_t i = 0; i < context.documents.size(); ++i) context.documents[i] = state.mapInputs[i].committed.get();
    context.document = context.documents[index];
    context.nameMap = state.map.get();
    return Result<Services::ObjectCommandContext>::Success(context);
}

} // namespace War3Map::Mcp
