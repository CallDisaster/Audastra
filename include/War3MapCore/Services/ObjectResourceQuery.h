#pragma once

#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Resources/ResourceResolver.h"
#include "War3MapCore/Services/NameResolution.h"

#include <functional>
#include <memory>

namespace War3Map::Game { class GameDataSource; }
namespace War3Map::Services {
class ImportInventorySnapshot;
struct ImportInventoryBinding;

enum class ObjectResourceKind { Model, Icon, Texture, SoundFile, SoundLabel, UnitSoundSet };
enum class ObjectResourceOrigin { Object, Import, Game };

struct ObjectResourceField {
    ObjectResourceKind kind = ObjectResourceKind::Model;
    bool orderedList = false;
    bool storesPath = true; // soundLabel/unitSound 保存符号，不能写入 wav/mp3 文件名。
};
[[nodiscard]] std::optional<ObjectResourceField> DescribeObjectResourceField(
    const ObjectData::ObjectFieldDefinition& definition);
[[nodiscard]] std::string_view ObjectResourceKindName(ObjectResourceKind kind) noexcept;
[[nodiscard]] std::string_view ObjectResourceOriginName(ObjectResourceOrigin origin) noexcept;

struct ObjectResourceCandidate {
    std::string id, title, value, previewPath, sourceId, state;
    ObjectResourceKind kind = ObjectResourceKind::Model;
    ObjectResourceOrigin origin = ObjectResourceOrigin::Object;
    std::optional<ObjectData::ObjectKey> object;
    std::string fieldId;
    bool customObject = false;
    bool directory = false;
    // 候选来源是列举来源；选中后的实际覆盖层由 InspectObjectResource 确认。
    bool sourceVerified = false;
};

struct ObjectResourceCatalog {
    std::string revision, identity;
    ObjectResourceOrigin origin = ObjectResourceOrigin::Object;
    std::vector<ObjectResourceCandidate> rows;
    std::vector<Diagnostic> diagnostics;
    bool complete = true;
};
struct ObjectResourceCatalogLimits {
    std::size_t maxRows = 131072;        // 硬上限 262144。
    std::size_t maxTextBytes = 32 * 1024 * 1024;
    std::size_t maxObjects = 32768;
    std::size_t maxFields = 1048576;
    std::function<bool(std::size_t examined)> progress; // 返回 false 取消，不返回半份成功。
};
struct ObjectResourceContext {
    std::string revision;
    const ObjectData::ObjectFieldSchema* schema = nullptr;
    NameResolutionContext names;
};

// 仅捕获自有候选与说明；输入借用只活到 Capture 返回。宿主后台索引时先冻结输入，
// 不能在工作线程借用仍可编辑的文档。所有来源失败保留诊断，不冒充空目录。
[[nodiscard]] Result<std::shared_ptr<const ObjectResourceCatalog>> CaptureObjectResourceCatalog(
    const ObjectResourceContext& context, const ObjectResourceCatalogLimits& limits = {});
[[nodiscard]] Result<std::shared_ptr<const ObjectResourceCatalog>> CaptureGameResourceCatalog(
    const Game::GameDataSource& source, std::string revision, const ObjectResourceCatalogLimits& limits = {});
[[nodiscard]] Result<std::shared_ptr<const ObjectResourceCatalog>> CaptureImportResourceCatalog(
    const ImportInventorySnapshot& source, const ImportInventoryBinding& expected,
    const ObjectResourceCatalogLimits& limits = {});

struct ObjectResourceQuery {
    std::string expectedRevision;
    ObjectResourceKind kind = ObjectResourceKind::Model;
    std::string search;
    std::optional<ObjectData::ObjectType> objectType;
    std::optional<std::string> directory; // 指定则只列此目录的直接子项，空串是根目录。
    std::size_t limit = 64;               // 1..128。
    std::size_t maxExamined = 4096;        // 1..8192；含未匹配项。
};
struct ObjectResourceCursor {
    std::string catalogIdentity, queryIdentity;
    std::size_t nextIndex = 0;
};
struct ObjectResourcePage {
    std::string revision, catalogIdentity;
    std::vector<ObjectResourceCandidate> rows;
    std::optional<ObjectResourceCursor> next;
    std::size_t examined = 0, knownRows = 0;
    bool complete = false, scanBudgetExhausted = false;
    std::vector<Diagnostic> diagnostics;
};
[[nodiscard]] Result<ObjectResourcePage> QueryObjectResources(const ObjectResourceCatalog& catalog,
    const ObjectResourceQuery& query, const std::optional<ObjectResourceCursor>& cursor = {});

struct ObjectResourceSequence { std::string name; std::uint32_t startMs = 0, endMs = 0; bool loops = true, usable = false; };
struct ObjectResourceInfo {
    std::string requestedValue, resourcePath;
    ObjectResourceKind kind = ObjectResourceKind::Model;
    Resources::ResourceLocation source;
    std::size_t bytes = 0;
    std::optional<std::uint32_t> width, height;
    std::vector<ObjectResourceSequence> sequences;
    std::vector<Diagnostic> diagnostics;
};
// Resolver 的只读回调须在分配前有界读取。读取、解码失败直接报告原原因，绝不回退低层。
// 只返回来源/尺寸/动作等信息，不发送正文；原始字段值不存在也不会被本服务改写。
[[nodiscard]] Result<ObjectResourceInfo> InspectObjectResource(const Resources::ResourceResolver& resolver,
    std::string_view value, ObjectResourceKind kind, std::size_t maxBytes = 32 * 1024 * 1024);

} // namespace War3Map::Services
