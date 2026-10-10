#include "War3MapCore/CompatibilityProfile.h"

#include "War3MapCore/Hash.h"
#include "CompatibilityProfiles.generated.h"

#include <charconv>
#include <cctype>
#include <optional>

namespace War3Map {
namespace {

std::optional<std::size_t> ValueStart(
    std::string_view json,
    std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    const auto keyOffset = json.find(token);
    if (keyOffset == std::string_view::npos ||
        json.find(token, keyOffset + token.size()) != std::string_view::npos) {
        return std::nullopt;
    }
    auto cursor = json.find(':', keyOffset + token.size());
    if (cursor == std::string_view::npos) {
        return std::nullopt;
    }
    ++cursor;
    while (cursor < json.size() &&
           std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
        ++cursor;
    }
    return cursor;
}

std::optional<std::string> StringValue(
    std::string_view json,
    std::string_view key) {
    const auto start = ValueStart(json, key);
    if (!start || *start >= json.size() || json[*start] != '"') {
        return std::nullopt;
    }
    std::string value;
    for (std::size_t cursor = *start + 1; cursor < json.size(); ++cursor) {
        const char character = json[cursor];
        if (character == '"') {
            return value;
        }
        if (character != '\\') {
            value.push_back(character);
            continue;
        }
        if (++cursor >= json.size()) {
            return std::nullopt;
        }
        switch (json[cursor]) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<bool> BoolValue(
    std::string_view json,
    std::string_view key,
    bool defaultValue) {
    const std::string token = "\"" + std::string(key) + "\"";
    if (json.find(token) == std::string_view::npos) {
        return defaultValue;
    }
    const auto start = ValueStart(json, key);
    if (!start) {
        return std::nullopt;
    }
    if (json.substr(*start, 4) == "true") {
        return true;
    }
    if (json.substr(*start, 5) == "false") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> UnsignedValue(
    std::string_view json,
    std::string_view key) {
    const auto start = ValueStart(json, key);
    if (!start) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(
        json.data() + *start,
        json.data() + json.size(),
        value);
    if (parsed.ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<std::string>> StringArrayValue(
    std::string_view json,
    std::string_view key) {
    const auto start = ValueStart(json, key);
    if (!start || *start >= json.size() || json[*start] != '[') {
        return std::nullopt;
    }
    std::vector<std::string> values;
    std::size_t cursor = *start + 1;
    while (cursor < json.size()) {
        while (cursor < json.size() &&
               (std::isspace(static_cast<unsigned char>(json[cursor])) != 0 || json[cursor] == ',')) {
            ++cursor;
        }
        if (cursor < json.size() && json[cursor] == ']') {
            return values;
        }
        if (cursor >= json.size() || json[cursor] != '"') {
            return std::nullopt;
        }
        const auto end = json.find('"', cursor + 1);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        values.emplace_back(json.substr(cursor + 1, end - cursor - 1));
        cursor = end + 1;
    }
    return std::nullopt;
}

} // namespace

Status CompatibilityProfile::Validate() const {
    if (schemaVersion != 1) {
        return Status::Failure(
            "Profile", "PROFILE_SCHEMA_UNSUPPORTED", "Compatibility profile schemaVersion must be 1.");
    }
    if (id.empty()) {
        return Status::Failure(
            "Profile", "PROFILE_ID_EMPTY", "Compatibility profile id is required.");
    }
    if (warcraftVersion.empty() || locale.empty()) {
        return Status::Failure(
            "Profile",
            "PROFILE_INCOMPLETE",
            "Compatibility profile must identify Warcraft version and locale.");
    }
    if (unknownDataPolicy.empty()) {
        return Status::Failure(
            "Profile", "PROFILE_POLICY_MISSING", "Compatibility profile must declare unknownDataPolicy.");
    }
    const auto definitionHash = Sha256Text(definitionSource);
    if (!definitionHash.Succeeded() || definitionSha256.size() != 64 ||
        *definitionHash.value != definitionSha256) {
        return Status::Failure(
            "Profile",
            "PROFILE_SHA_MISMATCH",
            "Compatibility profile SHA-256 does not match its JSON definition bytes.");
    }
    return Status::Ok();
}

Result<CompatibilityProfile> CompatibilityProfile::FromJson(
    std::string_view definition) {
    const auto schemaVersion = UnsignedValue(definition, "schemaVersion");
    const auto id = StringValue(definition, "id");
    const auto displayName = StringValue(definition, "displayName");
    const auto warcraftVersion = StringValue(definition, "warcraftVersion");
    const auto locale = StringValue(definition, "locale");
    const auto unknownDataPolicy = StringValue(definition, "unknownDataPolicy");
    if (!schemaVersion || !id || !displayName || !warcraftVersion || !locale ||
        !unknownDataPolicy) {
        return Result<CompatibilityProfile>::Failure(
            "Profile",
            "PROFILE_JSON_INVALID",
            "Compatibility profile JSON is missing a required field or uses an unsupported value encoding.");
    }

    CompatibilityProfile profile;
    profile.schemaVersion = *schemaVersion;
    profile.id = *id;
    profile.displayName = *displayName;
    profile.warcraftVersion = *warcraftVersion;
    profile.locale = *locale;
    profile.unknownDataPolicy = *unknownDataPolicy;
    profile.definitionSource.assign(definition);
    auto definitionHash = Sha256Text(definition);
    if (!definitionHash.Succeeded()) {
        Result<CompatibilityProfile> result;
        result.diagnostics = std::move(definitionHash.diagnostics);
        return result;
    }
    profile.definitionSha256 = std::move(*definitionHash.value);
    const auto validation = profile.Validate();
    if (!validation.Succeeded()) {
        Result<CompatibilityProfile> result;
        result.diagnostics = validation.diagnostics;
        return result;
    }
    return Result<CompatibilityProfile>::Success(std::move(profile));
}

CompatibilityProfile CompatibilityProfile::Default() {
    auto profile = FromJson(GeneratedProfiles::Default);
    return profile.Succeeded() ? std::move(*profile.value) : CompatibilityProfile{};
}

} // namespace War3Map
