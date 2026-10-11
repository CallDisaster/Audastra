#pragma once

#include "War3MapCore/Services/ObjectResourceQuery.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/ObjectData/ProfileList.h"

#include <map>
#include <set>

namespace War3Map::Services::ObjectResourceDetail {
inline Diagnostic Warning(std::string code, std::string message) {
    return {DiagnosticSeverity::Warning, "Object.Resource", std::move(code), std::move(message), std::nullopt};
}
inline std::string Slash(std::string_view path) {
    std::string result(path);
    for (auto& c : result) if (c == '\\') c = '/';
    return result;
}
inline std::optional<ObjectResourceKind> PathKind(std::string_view path) {
    const auto value = Text::LowerAscii(path);
    if (value.ends_with(".mdx") || value.ends_with(".mdl")) return ObjectResourceKind::Model;
    if (value.ends_with(".blp") || value.ends_with(".tga")) return ObjectResourceKind::Texture;
    if (value.ends_with(".wav") || value.ends_with(".mp3")) return ObjectResourceKind::SoundFile;
    return {};
}
inline std::string JoinPath(std::string_view base, std::string_view file) {
    auto path = Slash(base);
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path.empty() ? Slash(file) : path + '/' + Slash(file);
}
inline std::vector<std::string> Values(std::string_view raw, bool list) {
    std::vector<std::string> result;
    if (!list) { result.emplace_back(Text::TrimCWhitespace(raw)); return result; }
    for (const auto& item : ObjectData::SlkText::SplitProfileList(raw))
        result.push_back(ObjectData::SlkText::DecodeProfileItem(item));
    return result;
}
inline void IdentityPart(std::string& buffer, std::string_view value) {
    buffer += std::to_string(value.size()) + ':'; buffer.append(value);
}

class Builder {
public:
    Builder(std::string revision, ObjectResourceOrigin origin, const ObjectResourceCatalogLimits& limits)
        : limits_(limits), catalog_(std::make_shared<ObjectResourceCatalog>()) {
        catalog_->revision = std::move(revision); catalog_->origin = origin;
    }
    bool Valid() const {
        return !catalog_->revision.empty() && catalog_->revision.size() <= 4096
            && limits_.maxRows > 0 && limits_.maxRows <= 262144
            && limits_.maxTextBytes > 0 && limits_.maxTextBytes <= 64 * 1024 * 1024
            && limits_.maxObjects > 0 && limits_.maxObjects <= 65536
            && limits_.maxFields > 0 && limits_.maxFields <= 2097152;
    }
    bool Tick() {
        if (exhausted_ || cancelled_) return false;
        if (++examined_ > limits_.maxFields) { Exhausted(); return false; }
        if (limits_.progress && (examined_ == 1 || examined_ % 128 == 0) && !limits_.progress(examined_)) {
            cancelled_ = true; return false;
        }
        return true;
    }
    bool Add(ObjectResourceCandidate row) {
        if (exhausted_ || cancelled_) return false;
        const auto bytes = row.id.size() + row.title.size() + row.value.size() + row.previewPath.size()
            + row.sourceId.size() + row.state.size() + row.fieldId.size() + (row.object ? row.object->rawcode.size() : 0);
        if (row.value.size() > 4096 || row.previewPath.size() > 4096 || row.title.size() > 4096) {
            catalog_->complete = false;
            if (!longValue_) catalog_->diagnostics.push_back(Warning("OBJECT_RESOURCE_TEXT_LIMIT", "部分资源字段超过4096字节，未作为候选列出。"));
            longValue_ = true; return true;
        }
        if (catalog_->rows.size() >= limits_.maxRows || bytes > limits_.maxTextBytes - textBytes_) { Exhausted(); return false; }
        textBytes_ += bytes; catalog_->rows.push_back(std::move(row)); return true;
    }
    void Exhausted() {
        if (!exhausted_) catalog_->diagnostics.push_back(Warning("OBJECT_RESOURCE_INDEX_LIMIT", "资源索引达到本次预算；目录只列已确认的部分。"));
        exhausted_ = true; catalog_->complete = false;
    }
    void Diagnose(Diagnostic diagnostic) {
        if (catalog_->diagnostics.size() < 128) catalog_->diagnostics.push_back(std::move(diagnostic));
        else if (catalog_->diagnostics.size() == 128)
            catalog_->diagnostics.push_back(Warning("OBJECT_RESOURCE_DIAGNOSTIC_LIMIT", "更多资源诊断已省略；目录仍标记为部分可用。"));
    }
    ObjectResourceCatalog& Catalog() { return *catalog_; }
    const ObjectResourceCatalogLimits& Limits() const { return limits_; }
    Result<std::shared_ptr<const ObjectResourceCatalog>> Finish();
private:
    const ObjectResourceCatalogLimits& limits_;
    std::shared_ptr<ObjectResourceCatalog> catalog_;
    std::size_t examined_ = 0, textBytes_ = 0;
    bool exhausted_ = false, cancelled_ = false, longValue_ = false;
};
} // namespace War3Map::Services::ObjectResourceDetail
