#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <exception>
#include <map>
#include <set>

namespace War3Map::ObjectData {
namespace {
constexpr std::size_t kMaximumInputs = 256;
constexpr std::size_t kMaximumRetainedBytes = 64U * 1024U * 1024U;
using ReadResult = Result<std::optional<EffectiveCatalogFile>>;

ReadResult Read(const ObjectCatalogReader& reader, std::string_view name) {
    if (!reader) return ReadResult::Failure("Object.Catalog", "OBJECT_CATALOG_READER_REQUIRED", "有效目录需要明确的来源读取器。");
    try {
        auto result = reader(name);
        if (!result.Succeeded() && result.diagnostics.empty())
            return ReadResult::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_FAILED", "物编表来源未返回可确认结果：" + std::string(name));
        return result;
    } catch (const std::exception& error) {
        return ReadResult::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_FAILED", "物编表读取异常：" + std::string(error.what()));
    } catch (...) {
        return ReadResult::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_FAILED", "物编表读取发生未知异常。");
    }
}

Result<ObjectCatalogInputIdentity> Identify(std::string name,
    const std::optional<EffectiveCatalogFile>& source, std::size_t maximum) {
    ObjectCatalogInputIdentity identity;
    identity.logicalPath = std::move(name);
    if (!source) return Result<ObjectCatalogInputIdentity>::Success(std::move(identity));
    const auto canonical = Resources::CanonicalImportPath(source->location.canonicalPath);
    if (!canonical.Succeeded() || *canonical.value != identity.logicalPath || source->location.sourceId.empty()
        || source->location.sourcePath.empty() || source->containerIdentity.empty())
        return Result<ObjectCatalogInputIdentity>::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_IDENTITY", "物编表没有与查询对应的完整来源身份：" + identity.logicalPath);
    if (source->bytes.size() > maximum)
        return Result<ObjectCatalogInputIdentity>::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_BUDGET", "物编表超过读取预算：" + identity.logicalPath);
    const auto hash = Sha256(source->bytes);
    if (!hash.Succeeded()) return {{}, hash.diagnostics};
    identity.present = true;
    identity.location = source->location;
    identity.location.canonicalPath = identity.logicalPath;
    identity.containerIdentity = source->containerIdentity;
    identity.byteSize = source->bytes.size();
    identity.sha256 = *hash.value;
    return Result<ObjectCatalogInputIdentity>::Success(std::move(identity));
}

bool Same(const ObjectCatalogInputIdentity& a, const ObjectCatalogInputIdentity& b) {
    if (a.logicalPath != b.logicalPath || a.present != b.present) return false;
    return !a.present || (a.location.layer == b.location.layer && a.location.priority == b.location.priority
        && a.location.sourceId == b.location.sourceId && a.location.sourcePath == b.location.sourcePath
        && a.containerIdentity == b.containerIdentity && a.sha256 == b.sha256 && a.byteSize == b.byteSize);
}
} // namespace

Result<std::vector<std::string>> EffectiveObjectCatalog::InputNames(const CatalogLoadOptions& options) {
    std::set<std::string> names;
    const auto add = [&](const auto& files) -> Status {
        for (const auto& path : files) {
            const auto canonical = Resources::CanonicalImportPath(path);
            if (!canonical.Succeeded()) return {canonical.diagnostics};
            names.insert(*canonical.value);
            if (names.size() > kMaximumInputs)
                return Status::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_COUNT", "物编表来源超过 256 项预算。");
        }
        return Status::Ok();
    };
    if (const auto status = add(options.stringFiles); !status.Succeeded()) return {{}, status.diagnostics};
    const auto specs = options.sources.empty() ? StandardObjectCatalog::DefaultSources() : options.sources;
    for (const auto& spec : specs) {
        if (const auto status = add(spec.slkFiles); !status.Succeeded()) return {{}, status.diagnostics};
        if (const auto status = add(spec.profileFiles); !status.Succeeded()) return {{}, status.diagnostics};
    }
    return Result<std::vector<std::string>>::Success({names.begin(), names.end()});
}

Result<EffectiveObjectCatalog> EffectiveObjectCatalog::Capture(const ObjectCatalogReader& reader,
    const CatalogLoadOptions& options, ObjectCatalogSourceVerifier verifier) {
    if (options.maximumFileBytes > 32U * 1024U * 1024U)
        return Result<EffectiveObjectCatalog>::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_BUDGET", "物编目录单表预算只能收紧 32 MiB 硬上限。");
    const auto names = InputNames(options);
    if (!names.Succeeded()) return {{}, names.diagnostics};
    EffectiveObjectCatalog result;
    result.maximumFileBytes_ = options.maximumFileBytes;
    result.verifier_ = std::move(verifier);
    if (const auto status = result.VerifyCapturedSources(); !status.Succeeded()) return {{}, status.diagnostics};
    std::map<std::string, std::optional<std::vector<std::byte>>, std::less<>> files;
    std::set<std::string> gameOnly;
    for (const auto& path : options.stringFiles) {
        const auto canonical = Resources::CanonicalImportPath(path);
        if (!canonical.Succeeded()) return {{}, canonical.diagnostics};
        gameOnly.insert(*canonical.value);
    }
    std::size_t retained = 0;
    for (const auto& name : *names.value) {
        auto read = Read(reader, name);
        if (!read.Succeeded()) return {{}, std::move(read.diagnostics)};
        auto identity = Identify(name, *read.value, options.maximumFileBytes);
        if (!identity.Succeeded()) return {{}, std::move(identity.diagnostics)};
        identity.value->gameOnly = gameOnly.contains(name);
        if (identity.value->gameOnly && identity.value->present
            && identity.value->location.layer != Resources::ResourceLayer::Game)
            return Result<EffectiveObjectCatalog>::Failure("Object.Catalog", "OBJECT_CATALOG_STRING_SOURCE_UNTRUSTED",
                "WESTRING 字符串表必须来自游戏来源：" + name);
        if (identity.value->byteSize > kMaximumRetainedBytes - retained)
            return Result<EffectiveObjectCatalog>::Failure("Object.Catalog", "OBJECT_CATALOG_TOTAL_BUDGET", "有效物编目录超过 64 MiB 累积读取预算。");
        retained += identity.value->byteSize;
        result.inputs_.push_back(std::move(*identity.value));
        files.emplace(name, *read.value ? std::make_optional(std::move((**read.value).bytes)) : std::nullopt);
    }
    auto strictOptions = options;
    strictOptions.rejectInvalidPresentSources = true;
    auto catalog = StandardObjectCatalog::Load([&](const std::string& path) -> Result<std::vector<std::byte>> {
        const auto canonical = Resources::CanonicalImportPath(path);
        if (!canonical.Succeeded()) return {{}, canonical.diagnostics};
        const auto found = files.find(*canonical.value);
        if (found == files.end()) return Result<std::vector<std::byte>>::Failure("Object.Catalog", "OBJECT_CATALOG_UNCAPTURED", "目录解析查询了未捕获的来源。");
        if (!found->second) return Result<std::vector<std::byte>>::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_MISSING", "所有来源均明确缺失：" + path);
        return Result<std::vector<std::byte>>::Success(*found->second);
    }, strictOptions);
    if (!catalog.Succeeded()) return {{}, std::move(catalog.diagnostics)};
    if (const auto status = result.VerifyCapturedSources(); !status.Succeeded()) return {{}, status.diagnostics};
    result.catalog_ = std::move(*catalog.value);
    for (auto& entry : result.catalog_.entries_) {
        const auto writable = [&](const std::string& file) {
            const auto* input = result.Input(file);
            return input && input->present && input->location.layer == Resources::ResourceLayer::Game;
        };
        if (!writable(entry.definitionSourceFile) || (!entry.nameSourceFile.empty() && !writable(entry.nameSourceFile))
            || (!entry.iconSourceFile.empty() && !writable(entry.iconSourceFile))) entry.nativeOverridesSupported = false;
        for (const auto& [field, file] : entry.fieldSourceFiles) {
            (void)field;
            const auto* input = result.Input(file);
            if (!input || !input->present || input->location.layer != Resources::ResourceLayer::Game) {
                entry.nativeOverridesSupported = false;
                break;
            }
        }
    }
    auto output = Result<EffectiveObjectCatalog>::Success(std::move(result));
    output.diagnostics = std::move(catalog.diagnostics);
    return output;
}

Status EffectiveObjectCatalog::VerifyCapturedSources() const {
    if (!verifier_) return Status::Ok();
    try { return verifier_(); }
    catch (...) { return Status::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_CHANGED", "物编表来源复核未能完成。"); }
}

Status EffectiveObjectCatalog::Verify(const ObjectCatalogReader& current) const {
    if (const auto status = VerifyCapturedSources(); !status.Succeeded()) return status;
    for (const auto& expected : inputs_) {
        const auto read = Read(current, expected.logicalPath);
        if (!read.Succeeded()) return {read.diagnostics};
        const auto actual = Identify(expected.logicalPath, *read.value, maximumFileBytes_);
        if (!actual.Succeeded()) return {actual.diagnostics};
        if (expected.gameOnly && actual.value->present && actual.value->location.layer != Resources::ResourceLayer::Game)
            return Status::Failure("Object.Catalog", "OBJECT_CATALOG_STRING_SOURCE_UNTRUSTED", "字符串表的受信任来源已变化。");
        if (!Same(expected, *actual.value)) return Status::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_CHANGED",
            "物编表的存在性、来源层或内容已变化，请重新读取：" + expected.logicalPath);
    }
    return VerifyCapturedSources();
}

const ObjectCatalogInputIdentity* EffectiveObjectCatalog::Input(std::string_view path) const {
    const auto canonical = Resources::CanonicalImportPath(path);
    if (!canonical.Succeeded()) return nullptr;
    const auto found = std::find_if(inputs_.begin(), inputs_.end(), [&](const auto& input) { return input.logicalPath == *canonical.value; });
    return found == inputs_.end() ? nullptr : &*found;
}

bool EffectiveObjectCatalog::HasNonGameInputs() const noexcept {
    return std::any_of(inputs_.begin(), inputs_.end(), [](const auto& input) {
        return input.present && input.location.layer != Resources::ResourceLayer::Game;
    });
}
} // namespace War3Map::ObjectData
