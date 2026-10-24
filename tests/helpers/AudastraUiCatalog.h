#pragma once

#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

// 界面触发器测试共用的函数库：合成的原版签名基础层（只含生成文本用到的函数，参数类型与 TriggerData 一致）
// 加仓库里真实的 AudastraUI 包，以及按它写成 WTG/WCT 再读回的空触发器文档。使用方须定义 AUDASTRA_UI_LIBRARY_DIR。
namespace AudastraUiTest {

inline constexpr std::string_view kBaseDefine =
    "[TriggerTypes]\n"
    "boolean=0,1,1,Boolean,boolean\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\nstring=0,1,1,String,string\n"
    "ComparisonOperator=0,0,0,Comparison\nEqualNotEqualOperator=0,0,0,EqualNotEqual\nArithmeticOperator=0,0,0,Arithmetic\n"
    "scriptcode=0,0,0,Script,string\n"
    "[TriggerParams]\n"
    "OperatorEqual=0,ComparisonOperator,==,Equal\nOperatorNotEqual=0,ComparisonOperator,!=,NotEqual\n"
    "OperatorEqualENE=0,EqualNotEqualOperator,==,Equal\nOperatorNotEqualENE=0,EqualNotEqualOperator,!=,NotEqual\n"
    "OperatorAdd=0,ArithmeticOperator,+,Add\nOperatorMultiply=0,ArithmeticOperator,*,Multiply\n";
inline constexpr std::string_view kBaseEvent = "[TriggerRegisterTimerEventSingle]\n[[.args]]\ntype=real\n";
inline constexpr std::string_view kBaseCondition =
    "[OperatorCompareBoolean]\nreturns=boolean\n[[.args]]\ntype=boolean\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=boolean\n"
    "[OperatorCompareInteger]\nreturns=boolean\n[[.args]]\ntype=integer\n[[.args]]\ntype=ComparisonOperator\n[[.args]]\ntype=integer\n"
    "[OperatorCompareReal]\nreturns=boolean\n[[.args]]\ntype=real\n[[.args]]\ntype=ComparisonOperator\n[[.args]]\ntype=real\n"
    "[OperatorCompareString]\nreturns=boolean\n[[.args]]\ntype=string\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=string\n";
inline constexpr std::string_view kBaseAction =
    "[IfThenElseMultiple]\n[ForLoopAMultiple]\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
    "[SetVariable]\n[[.args]]\ntype=AnyGlobal\n[[.args]]\ntype=Null\n[ReturnAction]\n[CommentString]\n[[.args]]\ntype=scriptcode\n";
inline constexpr std::string_view kBaseCall =
    "[GetForLoopIndexA]\nreturns=integer\n"
    "[I2R]\nreturns=real\n[[.args]]\ntype=integer\n[I2S]\nreturns=string\n[[.args]]\ntype=integer\n"
    "[OperatorInt]\nreturns=integer\n[[.args]]\ntype=integer\n[[.args]]\ntype=ArithmeticOperator\n[[.args]]\ntype=integer\n"
    "[OperatorReal]\nreturns=real\n[[.args]]\ntype=real\n[[.args]]\ntype=ArithmeticOperator\n[[.args]]\ntype=real\n"
    "[OperatorString]\nreturns=string\n[[.args]]\ntype=string\n[[.args]]\ntype=string\n";

inline std::filesystem::path PackageDirectory() { return std::filesystem::path(AUDASTRA_UI_LIBRARY_DIR) / "AudastraUI"; }

struct Catalog {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("Audastra-UiCatalog-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    std::optional<War3Map::Trigger::UiCatalog> catalog;
    std::optional<War3Map::Trigger::TriggerDocument> document;

    Catalog() = default;
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    ~Catalog() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    // 写出 root/ui 并读入；失败时返回原因。
    [[nodiscard]] std::string Load() {
        using namespace War3Map::Trigger;
        const auto first = [](const std::vector<War3Map::Diagnostic>& diagnostics) {
            return diagnostics.empty() ? std::string("（无诊断）") : diagnostics.front().code + " " + diagnostics.front().message;
        };
        const auto write = [](const std::filesystem::path& file, std::string_view text) {
            std::error_code error;
            std::filesystem::create_directories(file.parent_path(), error);
            if (error) return false;
            std::ofstream stream(file, std::ios::binary);
            stream << text;
            return stream.good();
        };
        const auto ui = root / "ui";
        if (!write(ui / "config", "base\nAudastraUI\n") || !write(ui / "base/define.txt", kBaseDefine)
            || !write(ui / "base/event.txt", kBaseEvent) || !write(ui / "base/condition.txt", kBaseCondition)
            || !write(ui / "base/action.txt", kBaseAction) || !write(ui / "base/call.txt", kBaseCall))
            return "写不出合成基础层：" + ui.string();
        std::error_code error;
        std::filesystem::copy(PackageDirectory(), ui / "AudastraUI", std::filesystem::copy_options::recursive, error);
        if (error) return "复制 AudastraUI 包失败：" + error.message();
        auto loaded = UiCatalog::Load(ui);
        if (!loaded.Succeeded()) return "AudastraUI 函数库读入：" + first(loaded.diagnostics);
        catalog.emplace(std::move(*loaded.value));
        const auto empty = WtgWctCodec{}.CreateEmpty(*catalog);
        if (!empty.Succeeded()) return "空触发器文档：" + first(empty.diagnostics);
        const auto encoded = WtgWctCodec{}.Serialize(*empty.value, *catalog);
        if (!encoded.Succeeded()) return "空触发器文档写出：" + first(encoded.diagnostics);
        auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, *catalog);
        if (!parsed.Succeeded()) return "空触发器文档读回：" + first(parsed.diagnostics);
        document.emplace(std::move(*parsed.value));
        return {};
    }
};

} // namespace AudastraUiTest
