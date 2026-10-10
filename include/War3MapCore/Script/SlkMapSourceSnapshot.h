#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Script/SlkTemplateSession.h"

namespace War3Map::Script {

struct SlkMapSourceLimits {
    std::size_t memberCount = 65536;
    std::size_t pathBytes = 1024;
    std::size_t totalPathBytes = 8 * 1024 * 1024;
    std::size_t retainedBytes = 64 * 1024 * 1024;
    std::size_t replacementBytes = 512 * 1024 * 1024;
    std::size_t frontendMemberCount = 256; // 固定物理来源槽位（包含 Missing），调用方只能收紧
};

struct SlkMapMemberSnapshot {
    std::string name;
    SlkSessionSourceIdentity identity;
    std::vector<std::byte> bytes; // Present 空成员与 Missing 不合并
};

enum class SlkMapSourceLookupState { Missing, Captured, PresentUncaptured, Unknown };
enum class SlkFrontendInputMode { Archive, Lni };

struct SlkMapSourceLookup {
    SlkMapSourceLookupState state = SlkMapSourceLookupState::Missing;
    std::string memberName; // 规范化物理成员名；不是文件系统路径
    const SlkMapMemberSnapshot* member = nullptr; // 仅 Captured 非空；随所属快照存活
};

// 仅捕获 MPQ 会话最终成员来源，不解析模板、不执行 Lua、不发布保存凭据。
// 源文件 SHA + 全部 replacement 内容 SHA + removals + revision 形成有长度前缀的摘要；
// 这不是未来 MPQ 文件的 SHA。来源切换或任何待保存改动都使摘要失效。
// 七类 native、WTS、W3I 及固定 frontend/proxy 读取的物理成员保有独立字节快照。
// 固定名字批量查证替代完整名字清单假设；匿名归档的域外缺失仍未知。
// 保守名单覆盖已审计 info.lua/frontend/proxy 的来源，不能据此声称任意工具版本均完整。
// Capture 拒绝路径歧义、冲突、读取失败、超预算与源变化。未支持覆盖保留并报告，
// NativeOnlySources() 为 false；成功只表示捕获成功，不是 SLK 可跳过或地图可保存。
class SlkMapSourceSnapshot {
public:
    [[nodiscard]] static Result<SlkMapSourceSnapshot> Capture(
        const MapArchiveSession& archive, std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals, std::uint64_t revision,
        const SlkMapSourceLimits& limits = {});

    [[nodiscard]] const std::string& SourceArchiveSha256() const noexcept { return archiveSha256_; }
    [[nodiscard]] const std::string& ManifestSha256() const noexcept { return manifestSha256_; }
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    // Unit, Ability, Item, Buff, Upgrade, Doodad, Destructable, WTS, W3I。
    [[nodiscard]] std::span<const SlkMapMemberSnapshot> Members() const noexcept {
        return std::span<const SlkMapMemberSnapshot>(members_).first(members_.empty() ? 0 : 9);
    }
    // 与 Members() 同序的原始九个来源；三方合并祖先不得使用最终 overlay。
    // 原始字节和最终字节一起受 retainedBytes 约束，不另开保留预算。
    [[nodiscard]] std::span<const SlkMapMemberSnapshot> OriginalMembers() const noexcept { return originals_; }
    // 包含 Members() 的同一批对象，不重复复制 native 字节；固定缺失来源也保留槽位。
    [[nodiscard]] std::span<const SlkMapMemberSnapshot> FrontendMembers() const noexcept { return members_; }
    // 固定域 Missing 已查证；域外已列名返回 PresentUncaptured，匿名清单域外返回 Unknown。
    [[nodiscard]] Result<SlkMapSourceLookup> LookupMember(std::string_view name) const;
    // 固定 proxy 的 map/table/w3x2lni 三类查询；未知 group/name/mode 是失败，不是 Missing。
    // table 别名按原顺序选择，空成员也立即命中。Lni 只给 map 加 MAP\\ 前缀，不混用根成员。
    // 不读取安装数据、不自动推断 input mode，也不执行 init_proxy 的 locale 删除。
    [[nodiscard]] Result<SlkMapSourceLookup> LoadFrontendMember(std::string_view group,
        std::string_view name, SlkFrontendInputMode mode = SlkFrontendInputMode::Archive) const;
    // 已知最终成员名；在含匿名块时不是完整 inventory 或域外缺失证明。
    [[nodiscard]] std::span<const std::string> FinalMemberNames() const noexcept { return finalNames_; }
    [[nodiscard]] bool FinalMemberNamesComplete() const noexcept { return finalNamesComplete_; }
    [[nodiscard]] std::span<const std::string> UnsupportedSources() const noexcept { return unsupported_; }
    [[nodiscard]] bool NativeOnlySources() const noexcept { return finalNamesComplete_ && unsupported_.empty(); }

private:
    SlkMapSourceSnapshot() = default;
    std::string archiveSha256_, manifestSha256_;
    std::uint64_t revision_ = 0;
    std::vector<SlkMapMemberSnapshot> members_, originals_;
    std::vector<std::string> finalNames_, unsupported_;
    bool finalNamesComplete_ = true;
};

} // namespace War3Map::Script
