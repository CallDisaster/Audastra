#include "War3MapCore/Resources/ResourceResolver.h"

#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace War3Map::Resources {
namespace {

constexpr std::string_view kSubsystem = "ResourceResolver";

bool ValidLayer(ResourceLayer layer) {
    switch (layer) {
    case ResourceLayer::SessionOverride:
    case ResourceLayer::MapImport:
    case ResourceLayer::Campaign:
    case ResourceLayer::Game:
    case ResourceLayer::ToolExtension:
    case ResourceLayer::Other: return true;
    }
    return false;
}

template <typename T>
Result<T> FailedCallback(std::string code, std::string message,
    const std::vector<Diagnostic>& original) {
    auto result = Result<T>::Failure(std::string(kSubsystem), std::move(code), std::move(message));
    result.diagnostics.insert(result.diagnostics.end(), original.begin(), original.end());
    return result;
}

Result<ResourceProbe> ProbeSource(const ResourceSource& source, std::string_view path) {
    try {
        return source.probe(path);
    } catch (const std::exception& exception) {
        return Result<ResourceProbe>::Failure(std::string(kSubsystem), "RESOURCE_PROBE_FAILED",
            "资源来源查询异常（" + source.id + "）：" + exception.what());
    } catch (...) {
        return Result<ResourceProbe>::Failure(std::string(kSubsystem), "RESOURCE_PROBE_FAILED",
            "资源来源查询异常（" + source.id + "）");
    }
}

bool ValidProvenance(const ResourceReadProvenance& provenance) {
    switch (provenance.origin) {
    case ResourceReadOrigin::MapArchive:
    case ResourceReadOrigin::GameArchive:
    case ResourceReadOrigin::SessionOwned:
    case ResourceReadOrigin::SessionFile:
    case ResourceReadOrigin::Synthetic: break;
    default: return false;
    }
    // 只限制新增rich元数据，不对legacy回调添加路径/正文限额；兼容Windows UTF-8长路径。
    constexpr std::size_t maximumText = 128u * 1024u, maximumTotal = 256u * 1024u;
    const auto text = [](const std::string& value) {
        return value.capacity() <= maximumText && value.find('\0') == std::string::npos;
    };
    if (provenance.containerIdentity.empty() || provenance.memberPath.empty()
        || !text(provenance.containerIdentity) || !text(provenance.memberPath) || !text(provenance.sourceContext)) return false;
    return provenance.containerIdentity.capacity() + provenance.memberPath.capacity() + provenance.sourceContext.capacity() <= maximumTotal
        && CanonicalImportPath(provenance.memberPath).Succeeded();
}

Result<ResourceReadResult> ReadSource(const ResourceSource& source, std::string_view path) {
    try {
        if (source.readWithProvenance) {
            auto result = source.readWithProvenance(path);
            if (!result.Succeeded()) return result;
            if (!result.value || !result.value->provenance || !ValidProvenance(*result.value->provenance))
                return Result<ResourceReadResult>::Failure(std::string(kSubsystem), "RESOURCE_PROVENANCE_INVALID",
                    "实际读取来源缺失、无效或超过元数据预算（" + source.id + "）");
            return result;
        }
        auto bytes = source.read(path);
        if (!bytes.Succeeded()) return {std::nullopt, std::move(bytes.diagnostics)};
        return Result<ResourceReadResult>::Success({std::move(*bytes.value), std::nullopt});
    } catch (const std::exception& exception) {
        return Result<ResourceReadResult>::Failure(std::string(kSubsystem), "RESOURCE_READ_FAILED",
            "资源来源读取异常（" + source.id + "）：" + exception.what());
    } catch (...) {
        return Result<ResourceReadResult>::Failure(std::string(kSubsystem), "RESOURCE_READ_FAILED",
            "资源来源读取异常（" + source.id + "）");
    }
}

} // namespace

Status ResourceResolver::AddSource(ResourceSource source) {
    if (source.id.empty() || !source.probe || (!source.read && !source.readWithProvenance)) {
        return Status::Failure(std::string(kSubsystem), "RESOURCE_SOURCE_INVALID",
            "资源来源需要唯一非空 ID，以及查询和读取回调");
    }
    if (!ValidLayer(source.layer)) {
        return Status::Failure(std::string(kSubsystem), "RESOURCE_SOURCE_INVALID", "资源来源层级无效");
    }
    if (std::any_of(sources_.begin(), sources_.end(), [&](const ResourceSource& existing) {
            return existing.id == source.id;
        })) {
        return Status::Failure(std::string(kSubsystem), "RESOURCE_SOURCE_DUPLICATE",
            "重复的资源来源 ID：" + source.id);
    }
    sources_.push_back(std::move(source));
    std::stable_sort(sources_.begin(), sources_.end(), [](const ResourceSource& left,
            const ResourceSource& right) {
        return left.priority > right.priority;
    });
    return Status::Ok();
}

bool ResourceResolver::RemoveSource(std::string_view id) {
    const auto oldSize = sources_.size();
    std::erase_if(sources_, [&](const ResourceSource& source) { return source.id == id; });
    return sources_.size() != oldSize;
}

void ResourceResolver::ClearSources() noexcept {
    sources_.clear();
}

Result<ResourceLocation> ResourceResolver::Locate(std::string_view virtualPath) const {
    const auto canonical = CanonicalImportPath(virtualPath);
    if (!canonical.Succeeded()) {
        return Result<ResourceLocation>::Failure(std::string(kSubsystem), "RESOURCE_PATH_UNSAFE",
            "资源路径不是安全的相对虚拟路径");
    }

    const ResourceSource* foundSource = nullptr;
    std::string foundPath;
    for (const auto& source : sources_) {
        if (foundSource != nullptr && source.priority != foundSource->priority) {
            break;
        }
        const auto probe = ProbeSource(source, *canonical.value);
        if (!probe.Succeeded()) {
            return FailedCallback<ResourceLocation>("RESOURCE_PROBE_FAILED",
                "资源来源查询失败（" + source.id + "）：" + *canonical.value,
                probe.diagnostics);
        }
        if (probe.value->state == ResourceProbeState::Conflict) {
            return Result<ResourceLocation>::Failure(std::string(kSubsystem), "RESOURCE_CONFLICT",
                "资源来源内部存在歧义（" + source.id + "）：" + probe.value->sourcePath);
        }
        if (probe.value->state == ResourceProbeState::Missing) {
            continue;
        }
        if (probe.value->state != ResourceProbeState::Found || probe.value->sourcePath.empty()) {
            return Result<ResourceLocation>::Failure(std::string(kSubsystem), "RESOURCE_PROBE_INVALID",
                "资源来源返回了无效命中（" + source.id + "）");
        }
        if (foundSource != nullptr) {
            return Result<ResourceLocation>::Failure(std::string(kSubsystem), "RESOURCE_CONFLICT",
                "同一优先级有多个资源来源命中：" + foundSource->id + "、" + source.id);
        }
        foundSource = &source;
        foundPath = probe.value->sourcePath;
    }
    if (foundSource == nullptr) {
        return Result<ResourceLocation>::Failure(std::string(kSubsystem), "RESOURCE_NOT_FOUND",
            "所有已注册来源均未找到资源：" + *canonical.value);
    }
    return Result<ResourceLocation>::Success(ResourceLocation{
        *canonical.value, foundSource->layer, foundSource->priority, foundSource->id, std::move(foundPath)});
}

Result<ResolvedResource> ResourceResolver::Read(std::string_view virtualPath) const {
    auto location = Locate(virtualPath);
    if (!location.Succeeded()) {
        Result<ResolvedResource> result;
        result.diagnostics = location.diagnostics;
        return result;
    }
    const auto source = std::find_if(sources_.begin(), sources_.end(), [&](const ResourceSource& candidate) {
        return candidate.id == location.value->sourceId;
    });
    if (source == sources_.end()) {
        return Result<ResolvedResource>::Failure(std::string(kSubsystem), "RESOURCE_SOURCE_MISSING",
            "资源来源在查询后已不存在：" + location.value->sourceId);
    }
    auto read = ReadSource(*source, location.value->sourcePath);
    if (!read.Succeeded()) {
        return FailedCallback<ResolvedResource>("RESOURCE_READ_FAILED",
            "资源来源读取失败（" + source->id + "）：" + location.value->sourcePath,
            read.diagnostics);
    }
    if (read.value->provenance)
        location.value->readProvenance = std::make_shared<const ResourceReadProvenance>(std::move(*read.value->provenance));
    return Result<ResolvedResource>::Success(ResolvedResource{std::move(*location.value), std::move(read.value->bytes)});
}

} // namespace War3Map::Resources
