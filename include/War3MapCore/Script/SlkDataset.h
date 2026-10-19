#pragma once

#include "War3MapCore/Script/SlkTemplateSession.h"

#include <functional>

namespace War3Map::Script {

enum class SlkDatasetFamily { Custom, Melee };
enum class SlkDatasetCoverage { DecodedPrebuiltFilesOnly };
// w3x2lni 默认数据的目录布局，由框架模板实际加载的 w3x2lni 入口决定，不按分支名判断。
enum class SlkDatasetLayout {
    // core/config.ini 的 global.data 选 data/<id>/prebuilt/<Custom|Melee>/（YDWE 1.32、KKWE）。
    DataSetPrebuilt,
    // 旧入口 script/w3x2lni.lua：script/prebuilt/defined/metadata.ini 与 script/prebuilt/default/，
    // 不区分 Custom/Melee；多分量字段写作带引号的 "名称:序号"，读取时统一成新版的 名称_序号。
    ScriptPrebuiltDefault,
};

struct SlkDatasetLimits {
    // 默认值也是硬上限；只有本单元所需的两层 section 和一维数组，不提供通用 Lua 求值。
    std::size_t fileBytes = 8 * 1024 * 1024;
    std::size_t totalBytes = 16 * 1024 * 1024;
    std::size_t records = 20000;
    std::size_t fields = 400000; // 已查真实 Custom 九文件本身有 254522 项，另需容纳 metadata
    std::size_t values = 1000000; // 字段、标量、数组容器及元素均在构造前记账
    std::size_t stringBytes = 16 * 1024 * 1024;
    std::size_t arrayElements = 256;
    std::size_t metadataFields = 10000;
};

struct SlkDatasetRequest {
    // 两个根必须由宿主明确提供。core 可来自 ydwe_devpath，数据来自 ydwe(true)，不能合并猜测。
    std::filesystem::path coreRoot;          // .../plugin/w3x2lni/script/core；旧入口布局为 .../script/prebuilt
    std::filesystem::path installedYdweRoot;
    std::span<const std::byte> finalW3i;     // 用真实 v25 解析器选择 Custom/Melee；不解析 WTS
    std::uint64_t documentRevision = 0;
    SlkDatasetLayout layout = SlkDatasetLayout::DataSetPrebuilt;
};

// 测试/专用文件来源注入；只能读取请求的确切物理文件，失败不能回退到地图或游戏层。
// 成功结果须是该文件的完整有界字节快照，不能截断。省略时使用本单元专用 VerifiedFile 读取。
using SlkDatasetFileReader = std::function<Result<std::vector<std::byte>>(
    const std::filesystem::path& exactPath, std::size_t maximumBytes)>;

struct SlkDatasetSourceReceipt {
    std::string role;
    std::filesystem::path path;
    std::string sha256;
    std::size_t byteCount = 0;
};
struct SlkDatasetPosition {
    std::size_t sourceIndex = 0; // 对 sources 的索引，避免每个字段重复分配绝对路径
    std::size_t offset = 0;
    std::size_t length = 0;
};
struct SlkDatasetValue {
    SlkSessionValue value;
    SlkDatasetPosition source;
};
using SlkDatasetFields = std::map<std::string, SlkDatasetValue>;
struct SlkDatasetRecord {
    SlkDatasetFields fields;
    SlkDatasetPosition source;
};
using SlkDatasetTable = std::map<std::string, SlkDatasetRecord>;

struct SlkDatasetMetadataScope {
    SlkDatasetFields properties; // 例如 misc scope 的 type；保留实际值
    // canonical 字段 -> 原始 metadata 属性，不提前丢弃或合并。
    // id 保留未补零的 1–4 字节字符串；不是已转换的 native 四字节 ID。
    SlkDatasetTable fields;
    SlkDatasetPosition source;
};
struct SlkDatasetMetadata {
    std::array<SlkDatasetMetadataScope, 7> common;
    SlkDatasetMetadataScope misc;
    // 真实 scope 的大小写保持；包含 baseCode 与具名 misc scope。
    // old 按 common 然后 byCode 两轮应用，new lookup 优先 common，不能提前 union。
    std::map<std::string, SlkDatasetMetadataScope> byCode;
};

struct SlkTypedDataset {
    SlkDatasetCoverage coverage = SlkDatasetCoverage::DecodedPrebuiltFilesOnly;
    SlkDatasetFamily family = SlkDatasetFamily::Custom; // 旧入口布局只有一份默认数据，family 仅记录 W3I 选择
    SlkDatasetLayout layout = SlkDatasetLayout::DataSetPrebuilt;
    std::string dataSetId; // 旧入口布局没有数据集目录，为空
    std::filesystem::path coreRoot, installedYdweRoot;
    std::string w3iSha256;
    std::uint64_t documentRevision = 0;
    // 有长度前缀的来源清单摘要，含模型版本、两个根、W3I、revision、family/data 和逐文件身份。
    std::string manifestSha256;
    std::vector<SlkDatasetSourceReceipt> sources; // config、canonical metadata、九个 prebuilt 文件
    std::array<SlkDatasetTable, 7> defaults; // Unit, Ability, Item, Buff, Upgrade, Doodad, Destructable
    SlkDatasetTable txt, misc; // get_default 任一缺失都会走 buildslk，不能忽略这两份来源
    SlkDatasetMetadata metadata;
};

// 读取 core/config.ini 的默认 global.data；不声称支持外部 set_setting 覆盖。旧入口布局不读 config，
// 读 coreRoot 下 defined/metadata.ini 与 default/ 九文件。
// 九文件缺失/失败均明确拒绝未实现的 frontend_buildslk fallback。缺字段（例如 unit._code）
// 与 _name 等内部数据按原文保留，不为通过现有 Session 校验填造字段。
// 纯数据子集解码，无 Lua/I/O 表达式执行。字段键仅字母/下划线开头的 ASCII alnum/_，
// 纯 section 允许数字开头；引号/继承/更深层级未支持。重复节/字段（含 scope 属性与 child
// 同名）、未知 metadata 属性、未知值语法拒绝。
// 成功证明这些来源的字节和 typed 子集结果；不证明目标 lni.dll oracle，也不生成 SLK Session
// Complete/WholeSessionNoObjectWrites 凭据，不能解除 ScriptTemplate 的保存阻断。
[[nodiscard]] Result<SlkTypedDataset> ReadSlkTypedDataset(
    const SlkDatasetRequest& request, const SlkDatasetFileReader& reader = {},
    const SlkDatasetLimits& limits = {});

// 识别 <根>/plugin/w3x2lni 的布局：有 data 目录为新版；有 script/w3x2lni.lua 与 script/prebuilt/default
// 为旧入口。两者并存或都没有时失败，不猜框架加载哪一套。
[[nodiscard]] Result<SlkDatasetLayout> DetectSlkDatasetLayout(const std::filesystem::path& w3x2lniRoot);
// 该布局下 SlkDatasetRequest.coreRoot 应取的目录。
[[nodiscard]] std::filesystem::path SlkDatasetCoreRoot(const std::filesystem::path& w3x2lniRoot, SlkDatasetLayout layout);

} // namespace War3Map::Script
