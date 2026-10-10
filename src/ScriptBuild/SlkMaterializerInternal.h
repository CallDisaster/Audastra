#pragma once

#include "War3MapCore/Script/SlkTemplateMaterializer.h"

#include <cmath>
#include <map>

// 两种 metadata 入口共用 native 字节身份、old WTS、预算和数组填充；不对外发布。
namespace War3Map::Script::MaterializerDetail {
using namespace ObjectData;
inline constexpr std::array<ObjectType, 7> kTypes{ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
    ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
inline constexpr std::array<std::string_view, 7> kNames{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
inline constexpr std::size_t kMaximumLevel = 256;
struct Rejection { std::string code, message; OriginSpan origin; };
[[noreturn]] void Reject(std::string code, std::string message, const OriginSpan& origin);
OriginSpan Origin(std::string_view path, const SlkNativeFrontendInput& input);
bool Hash(std::string_view value);
bool Rawcode(std::string_view value);
const SlkSessionScalar* Scalar(const SlkSessionObject& object, std::string_view field);
const std::string* String(const SlkSessionObject& object, std::string_view field);
SlkSessionValue Value(SlkSessionScalar value);
std::string_view LevelField(ObjectType type);

struct Budget {
    const SlkNativeFrontendLimits& limits;
    std::size_t objects = 0, modifications = 0, values = 0, strings = 0, metadataVisits = 0;
    void Spend(std::size_t& value, std::size_t count, std::size_t maximum, const OriginSpan& origin) {
        if (value > maximum || count > maximum - value) Reject("SLK_MATERIALIZER_LIMIT", "原生 frontend 子集超过预算。", origin);
        value += count;
    }
    void Text(std::string_view text, const OriginSpan& origin) {
        if (text.size() > 1024 * 1024 || text.find('\0') != std::string_view::npos)
            Reject("SLK_MATERIALIZER_STRING_UNSUPPORTED", "字符串超过 1 MiB 或包含 NUL。", origin);
        Spend(strings, text.size(), limits.stringBytes, origin);
    }
    void Container(const OriginSpan& origin) { Spend(values, 1, limits.values, origin); }
    void FieldKey(std::string_view key, const OriginSpan& origin) {
        if (key.empty() || key.size() > 256) Reject("SLK_MATERIALIZER_LIMIT", "字段名为空或超过 256 字节。", origin);
        Container(origin); Text(key, origin);
    }
    void ScalarValue(const SlkSessionScalar& value, const OriginSpan& origin) {
        Container(origin);
        if (const auto* real = std::get_if<double>(&value); real && !std::isfinite(*real))
            Reject("SLK_MATERIALIZER_NUMBER_UNSUPPORTED", "不接受非有限数。", origin);
        if (const auto* text = std::get_if<std::string>(&value)) Text(*text, origin);
    }
};
std::size_t Levels(const SlkSessionScalar* value, const OriginSpan& origin);
struct Field {
    const ObjectFieldDefinition* definition = nullptr;
    SlkSessionFieldDefinition session;
    bool profile = false, append = false, split = false;
};
using Fields = std::map<std::string, Field>;
using Sparse = std::map<std::size_t, SlkSessionScalar>;
using Overlay = std::map<std::string, Sparse>;
void CheckType(const SlkSessionScalar& value, SlkSessionFieldType expected, const OriginSpan& origin);
std::vector<SlkSessionScalar> Fill(const std::vector<SlkSessionScalar>& base, const Sparse* edits,
    std::size_t levels, const Field& field, Budget& budget, const OriginSpan& origin);
SlkSessionObject Merge(const SlkSessionObject& base, const Fields& fields, const Overlay& edits,
    ObjectType type, Budget& budget, const OriginSpan& origin);
class OldStrings {
public:
    OldStrings(const SlkNativeFrontendInput& input, Budget& budget);
    std::string Resolve(std::string_view value, const OriginSpan& origin, std::size_t& references);
private:
    Budget& budget_;
    std::map<std::uint32_t, const std::string*> entries_;
};
SlkSessionScalar NativeScalar(const ObjectModification& modification, NativeValueType type,
    OldStrings& strings, Budget& budget, std::size_t& references, const OriginSpan& origin);
void VerifyNativeIdentity(const SlkNativeFrontendInput& input, std::size_t index, Budget& budget);
// 在 public 入口的统一异常/预算/来源门禁内调用，失败抛 Rejection，不返回部分图。
SlkNativeFrontendMaterialization MaterializeCanonical(const SlkNativeFrontendInput& input, Budget& budget, OldStrings& strings);
} // namespace War3Map::Script::MaterializerDetail
