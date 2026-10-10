#include "War3MapCore/Services/MapSizeAnalysis.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/SourceEncoding.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>

namespace War3Map::Services {
namespace {
constexpr std::size_t kCategoryCount = static_cast<std::size_t>(MapSizeCategory::Count);
std::size_t Index(MapSizeCategory category) { return static_cast<std::size_t>(category); }

bool Utf8(std::string_view text) {
    for (std::uint32_t offset = 0; offset < text.size();) {
        const auto scalar = Text::ReadSourceScalar(text, offset, Text::SourceEncoding::Utf8);
        if (!scalar.Succeeded() || !scalar.value || !scalar.value->bytes) return false;
        offset += scalar.value->bytes;
    }
    return true;
}
struct Path {
    std::string key;
    bool nonAscii = false;
};
std::optional<Path> MemberPath(std::string_view text) {
    if (!Utf8(text)) return {};
    const auto canonical = Resources::CanonicalImportPath(text);
    if (canonical.Succeeded() && canonical.value) return Path{*canonical.value, false};
    // 有效UTF8来源继续统计。只把非ASCII字节替为安全字符来复用相对路径/组件安全门；
    // 真正identity保留原UTF8字节，仅统一分隔符及ASCII大小写，不猜Unicode大小写或目录别名。
    std::string shape(text);
    bool nonAscii = false;
    for (auto& character : shape) if (static_cast<unsigned char>(character) >= 0x80) {
        character = 'X'; nonAscii = true;
    }
    if (!nonAscii || !Resources::CanonicalImportPath(shape).Succeeded()) return {};
    auto key = Text::UpperAscii(text);
    std::replace(key.begin(), key.end(), '/', '\\');
    return Path{std::move(key), true};
}
MapSizeCategory Category(std::string_view key) {
    const auto dot = key.find_last_of('.');
    const auto slash = key.find_last_of('\\');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return MapSizeCategory::Other;
    const auto extension = key.substr(dot);
    if (extension == ".MDX" || extension == ".MDL") return MapSizeCategory::Model;
    if (extension == ".BLP" || extension == ".DDS" || extension == ".TGA" || extension == ".PNG"
        || extension == ".JPG" || extension == ".JPEG" || extension == ".BMP") return MapSizeCategory::Texture;
    if (extension == ".WAV" || extension == ".MP3" || extension == ".OGG" || extension == ".FLAC") return MapSizeCategory::Sound;
    if (extension == ".J" || extension == ".LUA" || extension == ".AI") return MapSizeCategory::Script;
    return MapSizeCategory::Other;
}
bool GeneratedScript(std::string_view key) {
    if (key.starts_with("MAP\\")) key.remove_prefix(4); // 只标对应生成来源，不用于合并identity。
    return key == "WAR3MAP.J" || key == "WAR3MAP.LUA" || key == "SCRIPTS\\WAR3MAP.J"
        || key == "SCRIPTS\\WAR3MAP.LUA";
}
bool Managed(std::string_view key) {
    if (key.starts_with('(') || GeneratedScript(key)) return true;
    auto local = key;
    if (local.starts_with("MAP\\")) local.remove_prefix(4); // 只判管理资格，不作为来源identity别名。
    if (local.find('\\') == std::string_view::npos && (local.starts_with("WAR3MAP") || local.starts_with("WAR3CAMPAIGN"))) return true;
    local = key;
    if (local.starts_with("TABLE\\")) local.remove_prefix(6);
    const auto lower = Text::LowerAscii(local);
    return key == "TABLE\\W3I.INI" || ObjectData::ObjectTypeFromLniFileName(lower).has_value()
        || ObjectData::ObjectTypeFromNativeFileName(lower).has_value();
}
void Add(std::optional<std::uint64_t>& total, std::uint64_t value, bool& overflow) {
    if (!total) return;
    if (value > (std::numeric_limits<std::uint64_t>::max)() - *total) { total.reset(); overflow = true; }
    else *total += value;
}
MapSizeTotals EmptyTotals() {
    MapSizeTotals totals;
    for (std::size_t index = 0; index < kCategoryCount; ++index) totals.categories[index].category = static_cast<MapSizeCategory>(index);
    return totals;
}
void Count(MapSizeTotals& totals, MapSizeCategory category, MapSizeByteTrust trust, std::optional<std::uint64_t> bytes) {
    auto& part = totals.categories[Index(category)];
    if (!bytes || trust == MapSizeByteTrust::Unknown) { ++part.unknownRows; ++totals.unknownRows; return; }
    if (trust == MapSizeByteTrust::OwnedMetadata) {
        ++part.knownRows; ++totals.knownRows;
        Add(part.knownBytes, *bytes, totals.sumOverflow); Add(totals.knownLogicalSubtotal, *bytes, totals.sumOverflow);
    } else {
        ++part.reportedUnverifiedRows; ++totals.reportedUnverifiedRows;
        Add(part.reportedUnverifiedBytes, *bytes, totals.sumOverflow); Add(totals.reportedUnverifiedSubtotal, *bytes, totals.sumOverflow);
    }
}
MapSizeState TotalsState(const MapSizeTotals& totals, bool closed) {
    if (closed && !totals.sumOverflow && !totals.unknownRows && !totals.reportedUnverifiedRows) return MapSizeState::Ready;
    return totals.knownRows || totals.reportedUnverifiedRows ? MapSizeState::Partial : MapSizeState::Unknown;
}
struct Item {
    MapSizeRow row;
    std::string key;
    bool ambiguous = false, removed = false;
};
struct Edit { std::string from, displayTo; std::optional<Path> to; bool deleted = false; };
struct Pending { MapSizePendingMetadata metadata; Path path; };

class Analysis {
public:
    Analysis(const MapSizeContext& context, const MapSizeAnalysisOptions& options) : context_(context), options_(options) {
        report_.contextRevision = context.contextRevision; report_.sourceKind = context.sourceKind;
        report_.opaqueMemberCount = context.opaqueMemberCount; report_.opaqueMemberCountUnknown = context.opaqueMemberCountUnknown;
        report_.generatedScriptSizeUnknown = context.generatedScriptSizeUnknown;
        report_.baseline = EmptyTotals(); report_.effective = EmptyTotals();
    }
    Result<MapSizeReport> Run() {
        CaptureMembers(); CaptureEdits(); CapturePending();
        MarkCollisions(baseline_);
        for (const auto& item : baseline_) Count(report_.baseline, item.row.category,
            item.ambiguous ? MapSizeByteTrust::Unknown : item.row.trust, item.row.originalLogicalBytes);
        for (std::size_t index = 0; index < invalidMembers_; ++index) Count(report_.baseline, MapSizeCategory::Other, MapSizeByteTrust::Unknown, {});
        current_ = baseline_;
        report_.effectiveUnknown = !context_.overlayComplete || overlayTruncated_ || invalidOverlaySource_;
        if (!context_.membersComplete || membersTruncated_) Warn("MAP_SIZE_SOURCE_PARTIAL", "仅统计已捕获的命名成员窗口；目录跳过或清单截断未计为全图。");
        if (context_.opaqueMemberCount || context_.opaqueMemberCountUnknown) Warn("MAP_SIZE_OPAQUE_MEMBERS", "归档含匿名或未确认数量的成员，其逻辑大小未知，命名成员小计不是完整归档大小。");
        if (report_.effectiveUnknown) {
            Warn("MAP_SIZE_OVERLAY_UNKNOWN", "资源编辑态未完整可信地应用，当前大小未知；原始快照统计仍保留。");
            report_.effective.knownLogicalSubtotal.reset(); report_.effective.reportedUnverifiedSubtotal.reset();
            for (auto& part : report_.effective.categories) { part.knownBytes.reset(); part.reportedUnverifiedBytes.reset(); }
        } else {
            ApplyEdits();
            if (!report_.effectiveUnknown) {
                ApplyPending(); MarkCollisions(current_);
                std::size_t generatedRows = 0;
                for (auto& item : current_) if (!item.removed) {
                    if (context_.generatedScriptSizeUnknown && GeneratedScript(item.key)) {
                        item.row.trust = MapSizeByteTrust::Unknown; item.row.currentLogicalBytes.reset(); ++generatedRows;
                    }
                    Count(report_.effective, item.row.category, item.ambiguous ? MapSizeByteTrust::Unknown : item.row.trust, item.row.currentLogicalBytes);
                }
                for (std::size_t index = 0; index < invalidMembers_; ++index) Count(report_.effective, MapSizeCategory::Other, MapSizeByteTrust::Unknown, {});
                if (context_.generatedScriptSizeUnknown && !generatedRows) Count(report_.effective, MapSizeCategory::Script, MapSizeByteTrust::Unknown, {});
            }
        }
        if (context_.generatedScriptSizeUnknown) Warn("MAP_SIZE_GENERATED_SCRIPT_UNKNOWN", "当前生成脚本的对应逻辑大小未知，未用旧 war3map.j/lua 大小冒充生成结果。");
        if (report_.baseline.unknownRows || report_.effective.unknownRows) Warn("MAP_SIZE_UNKNOWN_BYTES", "部分成员大小或唯一来源未知，已与已知逻辑字节小计分开。");
        if (report_.effective.reportedUnverifiedRows) Warn("MAP_SIZE_PENDING_REPORTED", "外盘待导入大小仅为宿主上报值，本查询没有打开、读取或核验该文件。");
        if (report_.baseline.sumOverflow || report_.effective.sumOverflow) Warn("MAP_SIZE_SUM_OVERFLOW", "64位字节合计溢出；对应合计为未知，没有返回截断、饱和或回绕总和。");
        const bool sourceClosed = context_.membersComplete && !membersTruncated_ && !context_.opaqueMemberCount && !context_.opaqueMemberCountUnknown;
        report_.baseline.state = TotalsState(report_.baseline, sourceClosed);
        report_.effective.state = report_.effectiveUnknown ? MapSizeState::Unknown : TotalsState(report_.effective, sourceClosed);
        // UTF8非ASCII路径大小正常计数，但不把保守的ASCII-only身份称为完整Unicode去重证明。
        if (report_.nonAsciiRows) {
            if (baselineNonAscii_ && report_.baseline.state == MapSizeState::Ready) report_.baseline.state = MapSizeState::Partial;
            if (report_.effective.state == MapSizeState::Ready) report_.effective.state = MapSizeState::Partial;
        }
        if (!MakeTop(sourceClosed)) return Result<MapSizeReport>::Failure("MapSizeAnalysis", "MAP_SIZE_OUTPUT_BUDGET", "输出预算不足以容纳固定统计与诊断，未返回超预算报告。");
        report_.state = report_.baseline.state == MapSizeState::Ready && report_.effective.state == MapSizeState::Ready
            ? MapSizeState::Ready : report_.baseline.state != MapSizeState::Unknown || report_.effective.state != MapSizeState::Unknown
            ? MapSizeState::Partial : MapSizeState::Unknown;
        return Result<MapSizeReport>::Success(std::move(report_));
    }
private:
    void Warn(std::string_view code, std::string_view message) {
        if (std::any_of(report_.diagnostics.begin(), report_.diagnostics.end(), [&](const auto& item) { return item.code == code; })) return;
        report_.diagnostics.push_back({DiagnosticSeverity::Warning, "MapSizeAnalysis", std::string(code), std::string(message), {}});
    }
    bool Visit() {
        if (report_.examinedMetadataRows == options_.maxMetadataRows) { report_.inputTruncated = true; return false; }
        ++report_.examinedMetadataRows; return true;
    }
    std::optional<Path> ReadPath(std::string_view text) {
        if (text.size() > options_.maxPathBytes) { Warn("MAP_SIZE_PATH_INVALID", "成员路径过长或不安全，没有截短路径后猜身份。"); return {}; }
        if (text.size() > options_.maxTotalPathBytes - report_.processedPathBytes) {
            report_.inputTruncated = true; pathBudgetExhausted_ = true;
            Warn("MAP_SIZE_PATH_BUDGET", "全部路径处理字节预算已用尽，只保留已核窗口的统计。"); return {};
        }
        report_.processedPathBytes += text.size();
        auto path = MemberPath(text);
        if (!path) Warn("MAP_SIZE_PATH_INVALID", "成员路径编码无效或不安全，未改写、替换或猜测其来源身份。");
        else if (path->nonAscii) {
            Warn("MAP_SIZE_NON_ASCII_IDENTITY", "有效UTF8非ASCII路径保留原路径并统计大小；只比较分隔符和ASCII大小写，不猜Unicode别名或授予资源导航。");
        }
        return path;
    }
    void CaptureMembers() {
        for (const auto& member : context_.members) {
            if (pathBudgetExhausted_ || !Visit()) { membersTruncated_ = true; break; }
            auto path = ReadPath(member.path);
            if (!path) {
                if (pathBudgetExhausted_) { membersTruncated_ = true; break; }
                ++report_.invalidPathRows; ++invalidMembers_; continue;
            }
            MapSizeRow row; row.identity = "member/" + path->key;
            row.originalPath = row.currentPath = member.path; row.originalLogicalBytes = row.currentLogicalBytes = member.logicalBytes;
            row.category = Category(path->key); row.trust = member.logicalBytes ? MapSizeByteTrust::OwnedMetadata : MapSizeByteTrust::Unknown;
            row.nonAsciiIdentity = path->nonAscii; row.managedMember = Managed(path->key);
            if (path->nonAscii) { ++report_.nonAsciiRows; baselineNonAscii_ = true; }
            baseline_.push_back({std::move(row), std::move(path->key)});
        }
    }
    void CaptureEdits() {
        for (const auto& metadata : context_.assetEdits) {
            if (pathBudgetExhausted_ || !Visit()) { overlayTruncated_ = true; break; }
            auto from = ReadPath(metadata.from);
            if (!from) {
                if (pathBudgetExhausted_) { overlayTruncated_ = true; break; }
                ++report_.invalidPathRows; invalidOverlaySource_ = true; continue;
            }
            Edit edit; edit.from = std::move(from->key); edit.deleted = metadata.to.empty();
            if (!edit.deleted) edit.to = ReadPath(metadata.to);
            if (edit.to) edit.displayTo = metadata.to;
            if (pathBudgetExhausted_) { overlayTruncated_ = true; break; }
            if (!edit.deleted && !edit.to) ++report_.invalidPathRows;
            if (from->nonAscii || (edit.to && edit.to->nonAscii)) ++report_.nonAsciiRows;
            edits_.push_back(std::move(edit));
        }
    }
    void CapturePending() {
        for (const auto& metadata : context_.pending) {
            if (pathBudgetExhausted_ || !Visit()) { overlayTruncated_ = true; break; }
            auto path = ReadPath(metadata.path);
            if (!path) {
                if (pathBudgetExhausted_) { overlayTruncated_ = true; break; }
                ++report_.invalidPathRows; invalidOverlaySource_ = true; continue;
            }
            if (path->nonAscii) ++report_.nonAsciiRows;
            auto bounded = metadata;
            if (bounded.trust != MapSizeByteTrust::OwnedMetadata && bounded.trust != MapSizeByteTrust::ReportedUnverified
                && bounded.trust != MapSizeByteTrust::Unknown) {
                bounded.trust = MapSizeByteTrust::Unknown;
                Warn("MAP_SIZE_PENDING_TRUST_INVALID", "待导入大小的信任状态无效，已保留未知状态而未当作已验证大小。");
            }
            pending_.push_back({std::move(bounded), std::move(*path)});
        }
        if (pathBudgetExhausted_ && (!context_.assetEdits.empty() || !context_.pending.empty())) overlayTruncated_ = true;
        if (report_.inputTruncated) Warn("MAP_SIZE_METADATA_BUDGET", "全局成员和资源编辑态元数据窗口达到预算；未处理部分不计为完整地图。");
    }
    void MarkUnknown(Item& item) {
        item.row.trust = MapSizeByteTrust::Unknown; item.row.currentLogicalBytes.reset(); item.row.navigationAllowed = false;
    }
    void MarkCollisions(std::vector<Item>& items) {
        std::map<std::string, std::vector<std::size_t>> groups;
        for (std::size_t index = 0; index < items.size(); ++index) if (!items[index].removed) groups[items[index].key].push_back(index);
        for (const auto& [key, indices] : groups) if (indices.size() > 1) {
            (void)key;
            for (const auto index : indices) {
                if (!items[index].ambiguous) ++report_.ambiguousRows;
                items[index].ambiguous = true; MarkUnknown(items[index]);
            }
            Warn("MAP_SIZE_CANONICAL_DUPLICATE", "多个元数据来源命中相同规范路径，未挑第一项计大小或提供导航。");
        }
    }
    void ApplyEdits() {
        std::map<std::string, std::vector<std::size_t>> edits;
        for (std::size_t index = 0; index < edits_.size(); ++index) edits[edits_[index].from].push_back(index);
        // 所有from绑定原始来源，同时应用改名；不能用已被前一项改名的目标当后一项原来源。
        std::map<std::string, std::vector<std::size_t>> originalRows;
        for (std::size_t index = 0; index < current_.size(); ++index) originalRows[current_[index].key].push_back(index);
        for (const auto& [from, indices] : edits) {
            const auto source = originalRows.find(from);
            if (source == originalRows.end()) {
                report_.effectiveUnknown = true;
                Warn("MAP_SIZE_EDIT_SOURCE_MISSING", "资源编辑的原始成员未在已捕获窗口中唯一确认，当前大小未知。"); continue;
            }
            if (indices.size() != 1 || source->second.size() != 1
                || current_[source->second.front()].ambiguous || current_[source->second.front()].row.managedMember) {
                for (const auto index : source->second) MarkUnknown(current_[index]);
                Warn("MAP_SIZE_EDIT_SOURCE_AMBIGUOUS", "资源编辑原始来源重复、歧义或属于受管成员，未猜测当前大小。"); continue;
            }
            auto& found = current_[source->second.front()];
            const auto& edit = edits_[indices.front()];
            if (edit.deleted) { found.removed = true; ++report_.deletedRows; continue; }
            if (!edit.to) { MarkUnknown(found); continue; }
            if (Managed(edit.to->key)) {
                MarkUnknown(found); Warn("MAP_SIZE_EDIT_SOURCE_AMBIGUOUS", "资源编辑原始来源重复、歧义或属于受管成员，未猜测当前大小。"); continue;
            }
            found.key = edit.to->key; found.row.currentPath = edit.displayTo;
            found.row.category = Category(found.key); found.row.nonAsciiIdentity = found.row.nonAsciiIdentity || edit.to->nonAscii;
            found.row.managedMember = false; found.row.origin = MapSizeRowOrigin::Renamed;
        }
        if (report_.effectiveUnknown) Warn("MAP_SIZE_OVERLAY_UNKNOWN", "资源编辑态未完整可信地应用，当前大小未知；原始快照统计仍保留。");
    }
    void ApplyPending() {
        std::map<std::string, std::vector<std::size_t>> currentRows;
        for (std::size_t index = 0; index < current_.size(); ++index) if (!current_[index].removed) currentRows[current_[index].key].push_back(index);
        for (const auto& pending : pending_) {
            auto& indices = currentRows[pending.path.key];
            if (indices.size() == 1 && current_[indices.front()].row.origin != MapSizeRowOrigin::PendingImport
                && current_[indices.front()].row.origin != MapSizeRowOrigin::PendingReplacement) {
                auto& found = current_[indices.front()];
                found.row.currentPath = pending.metadata.path; found.row.currentLogicalBytes = pending.metadata.logicalBytes;
                found.row.trust = pending.metadata.logicalBytes ? pending.metadata.trust : MapSizeByteTrust::Unknown;
                found.row.origin = MapSizeRowOrigin::PendingReplacement; found.row.nonAsciiIdentity = found.row.nonAsciiIdentity || pending.path.nonAscii;
            } else {
                MapSizeRow row; row.identity = "pending/" + pending.path.key; row.currentPath = pending.metadata.path;
                row.currentLogicalBytes = pending.metadata.logicalBytes; row.trust = pending.metadata.logicalBytes ? pending.metadata.trust : MapSizeByteTrust::Unknown;
                row.category = Category(pending.path.key); row.origin = MapSizeRowOrigin::PendingImport;
                row.nonAsciiIdentity = pending.path.nonAscii; row.managedMember = Managed(pending.path.key);
                indices.push_back(current_.size()); current_.push_back({std::move(row), pending.path.key});
            }
        }
    }
    bool MakeTop(bool sourceClosed) {
        if (report_.effectiveUnknown) {
            report_.effective = EmptyTotals(); report_.effective.state = MapSizeState::Unknown;
            report_.effective.knownLogicalSubtotal.reset(); report_.effective.reportedUnverifiedSubtotal.reset();
            for (auto& part : report_.effective.categories) { part.knownBytes.reset(); part.reportedUnverifiedBytes.reset(); }
        }
        std::size_t bill = 2048 + report_.contextRevision.size() * 6;
        for (const auto& diagnostic : report_.diagnostics) bill += 128 + 6 * (diagnostic.code.size() + diagnostic.message.size());
        constexpr std::string_view truncatedCode = "MAP_SIZE_OUTPUT_TRUNCATED";
        constexpr std::string_view truncatedMessage = "输出字节预算限制了最大的成员窗口，未显示的行没有被当成完整Top20。";
        const auto truncationBill = 128 + 6 * (truncatedCode.size() + truncatedMessage.size());
        // 预留诊断的账，避免发生截断后再加未计费文字使返回超预算。
        bill += truncationBill;
        if (bill > options_.maxResultBytes) return false;
        if (!report_.effectiveUnknown) {
            std::vector<const Item*> ordered;
            for (const auto& item : current_) if (!item.removed && !item.ambiguous && item.row.currentLogicalBytes && item.row.trust != MapSizeByteTrust::Unknown) ordered.push_back(&item);
            std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
                if (*left->row.currentLogicalBytes != *right->row.currentLogicalBytes) return *left->row.currentLogicalBytes > *right->row.currentLogicalBytes;
                return left->row.identity < right->row.identity;
            });
            const auto count = (std::min)(std::size_t{20}, ordered.size());
            for (std::size_t index = 0; index < count; ++index) {
                auto row = ordered[index]->row;
                row.navigationAllowed = sourceClosed && !row.nonAsciiIdentity && !row.managedMember;
                const auto bytes = 384 + 6 * (row.identity.size() + row.currentPath.size() + row.originalPath.size());
                if (bytes > options_.maxResultBytes - bill) { report_.outputTruncated = true; break; }
                bill += bytes; report_.top20.push_back(std::move(row));
            }
        }
        report_.outputBudgetBytes = bill;
        report_.topWindowComplete = report_.effective.state == MapSizeState::Ready && !report_.outputTruncated;
        if (report_.outputTruncated) {
            Warn(truncatedCode, truncatedMessage);
            if (report_.effective.state == MapSizeState::Ready) report_.effective.state = MapSizeState::Partial;
        }
        return true;
    }
    const MapSizeContext& context_;
    const MapSizeAnalysisOptions& options_;
    MapSizeReport report_;
    std::vector<Item> baseline_, current_;
    std::vector<Edit> edits_;
    std::vector<Pending> pending_;
    std::size_t invalidMembers_ = 0;
    bool membersTruncated_ = false, overlayTruncated_ = false, pathBudgetExhausted_ = false, invalidOverlaySource_ = false, baselineNonAscii_ = false;
};
}

std::string_view MapSizeCategoryName(MapSizeCategory category) noexcept {
    switch (category) {
    case MapSizeCategory::Model: return "model";
    case MapSizeCategory::Texture: return "texture";
    case MapSizeCategory::Sound: return "sound";
    case MapSizeCategory::Script: return "script";
    case MapSizeCategory::Other: return "other";
    default: return "unknown";
    }
}
Result<MapSizeReport> AnalyzeMapSize(const MapSizeContext& context, std::string_view expectedContextRevision, const MapSizeAnalysisOptions& options) {
    if (!options.maxMetadataRows || options.maxMetadataRows > 32768 || !options.maxPathBytes || options.maxPathBytes > 2048
        || !options.maxTotalPathBytes || options.maxTotalPathBytes > 8 * 1024 * 1024 || !options.maxResultBytes || options.maxResultBytes > 256 * 1024)
        return Result<MapSizeReport>::Failure("MapSizeAnalysis", "MAP_SIZE_OPTIONS_INVALID", "元数据、路径或输出预算不在允许范围内。");
    if (context.contextRevision.empty() || context.contextRevision.size() > 1024 || context.contextRevision.find('\0') != std::string::npos
        || !Utf8(context.contextRevision) || context.contextRevision != expectedContextRevision)
        return Result<MapSizeReport>::Failure("MapSizeAnalysis", "MAP_SIZE_REVISION_MISMATCH", "工程上下文修订无效或已变化，未用旧结果替代当前地图。");
    if (context.sourceKind != MapSizeSourceKind::ArchiveMembers && context.sourceKind != MapSizeSourceKind::DirectorySnapshot)
        return Result<MapSizeReport>::Failure("MapSizeAnalysis", "MAP_SIZE_SOURCE_INVALID", "来源不是已冻结的归档命名成员或目录快照。");
    try { return Analysis(context, options).Run(); }
    catch (const std::bad_alloc&) { return Result<MapSizeReport>::Failure("MapSizeAnalysis", "MAP_SIZE_ALLOCATION_FAILED", "无法分配有预算的体积分析结果。"); }
    catch (const std::length_error&) { return Result<MapSizeReport>::Failure("MapSizeAnalysis", "MAP_SIZE_CONTAINER_LIMIT", "体积分析容器超过容量限制。"); }
}

} // namespace War3Map::Services
