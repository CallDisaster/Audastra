#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Text/UiNumberFormat.h"
#include <nlohmann/json.hpp>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

// 手动对照游戏写出的文字回执；只读明确的文本文件，不启动游戏、不改变预览实现。
int main(int argc, char** argv) {
    using namespace War3Map;
    if (argc != 2 && argc != 3) return 2;
    const std::filesystem::path input(argv[1]);
    auto extension = input.extension().string();
    for (char& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (extension != ".txt" || !std::filesystem::is_regular_file(input)
        || std::filesystem::file_size(input) > 64 * 1024) return 2;
    std::ifstream stream(input, std::ios::binary);
    std::map<unsigned, std::uint32_t> nativeBits;
    std::map<unsigned, std::string> nativeCommits;
    if (argc == 3) {
        const std::filesystem::path log(argv[2]);
        if (log.extension() != ".log" || !std::filesystem::is_regular_file(log) || std::filesystem::file_size(log) > 2 * 1024 * 1024) return 2;
        std::ifstream observations(log, std::ios::binary);
        const auto number = [](std::string_view line, std::string_view key) -> std::optional<std::uint32_t> {
            const auto at = line.find(key);
            if (at == std::string_view::npos) return {};
            std::uint32_t value = 0;
            const auto parsed = std::from_chars(line.data() + at + key.size(), line.data() + line.size(), value);
            return parsed.ec == std::errc{} ? std::optional(value) : std::nullopt;
        };
        for (std::string line; std::getline(observations, line);) {
            if (number(line, "epoch=") != 1) continue;
            const auto ordinal = number(line, "ordinal=");
            if (!ordinal || !*ordinal || *ordinal > 512) continue;
            if (line.find("ui-text-argument ") != std::string::npos && line.find("type=real ") != std::string::npos && number(line, "index=") == 0) {
                const auto bits = number(line, "bits=");
                if (!bits || !nativeBits.emplace(*ordinal, *bits).second) return 2;
            }
            if (line.find("ui-text-commit ") != std::string::npos) {
                const auto at = line.find(" text=");
                if (at != std::string::npos && !nativeCommits.emplace(*ordinal, line.substr(at + 6)).second) return 2;
            }
        }
    }
    nlohmann::json rows = nlohmann::json::array();
    std::size_t runtimeFailures = 0, previewFailures = 0;
    for (std::string line; std::getline(stream, line);) {
        const auto quote = line.find('"');
        if (quote == std::string::npos) continue;
        const auto colon = line.find(':', quote + 1), equals = line.find('=', colon);
        const auto end = line.find('"', equals);
        if (colon == std::string::npos || equals == std::string::npos || end == std::string::npos) continue;
        const auto sourceValue = line.substr(quote + 1, colon - quote - 1);
        float value = 0; int precision = 0;
        const auto parsed = std::from_chars(sourceValue.data(), sourceValue.data() + sourceValue.size(), value);
        const auto sourceBits = std::bit_cast<std::uint32_t>(value);
        const auto digits = line.substr(colon + 1, equals - colon - 1);
        const auto parsedPrecision = std::from_chars(digits.data(), digits.data() + digits.size(), precision);
        if (parsed.ec != std::errc{} || parsed.ptr != sourceValue.data() + sourceValue.size()
            || parsedPrecision.ec != std::errc{} || parsedPrecision.ptr != digits.data() + digits.size()) return 2;
        const auto raw = line.substr(equals + 1, end - equals - 1);
        auto reference = raw;
        const auto ordinal = static_cast<unsigned>(rows.size() + 1);
        if (argc == 3) {
            if (!nativeBits.contains(ordinal) || !nativeCommits.contains(ordinal)) return 2;
            value = std::bit_cast<float>(nativeBits.at(ordinal));
        }
        // 新统一规则：固定零位去掉原版附带的.0；所有模式都归一负零。
        if (precision == 0 && reference.ends_with(".0")) reference.resize(reference.size() - 2);
        if (reference.starts_with('-') && reference.find_first_not_of("0.", 1) == std::string::npos) reference.erase(0, 1);
        const auto actual = Text::FormatUiReal(value, precision);
        GameUi::UiDescription description;
        GameUi::UiValue spec; spec.id = "v"; spec.type = GameUi::UiValueType::Real; spec.defaultValue = sourceValue;
        description.values.push_back(std::move(spec));
        const auto pattern = precision < 0 ? "{v}" : "{v:" + std::to_string(precision) + "}";
        const auto sourcePreview = GameUi::FormatUiText(description, {}, pattern);
        // 格式器比较使用同一实收 binary32；作者字面量的解析差异另列，不能混算。
        char observed[64];
        const auto encoded = std::to_chars(observed, observed + sizeof(observed), value);
        if (encoded.ec != std::errc{}) return 2;
        GameUi::UiInstanceState previewState;
        previewState.values["v"] = std::string(observed, encoded.ptr);
        const auto preview = GameUi::FormatUiText(description, previewState, pattern);
        const bool runtimeMatch = actual && *actual == reference, previewMatch = preview == reference;
        const bool commitMatch = argc != 3 || nativeCommits.at(ordinal) == reference;
        runtimeFailures += !runtimeMatch || !commitMatch; previewFailures += !previewMatch;
        rows.push_back({{"value", sourceValue}, {"precision", precision}, {"raw_game_fallback", raw},
            {"contract_reference", reference}, {"runtime", actual.value_or("<refused>")}, {"preview", preview},
            {"source_literal_preview", sourcePreview}, {"source_quantization_differs", sourceBits != std::bit_cast<std::uint32_t>(value)},
            {"source_literal_nearest_bits", sourceBits}, {"received_native_bits", std::bit_cast<std::uint32_t>(value)},
            {"observed_native_commit", argc == 3 ? nativeCommits.at(ordinal) : "<not observed>"},
            {"runtime_match", runtimeMatch}, {"native_commit_match", commitMatch}, {"preview_match", previewMatch}});
    }
    const bool runtimePass = !rows.empty() && !runtimeFailures, previewPass = !rows.empty() && !previewFailures;
    std::cout << nlohmann::json{{"rows", rows}, {"row_count", rows.size()}, {"runtime_failures", runtimeFailures},
        {"preview_failures", previewFailures}, {"runtime_reference_pass", runtimePass}, {"current_preview_pass", previewPass},
        {"game_started", false}}.dump(2) << '\n';
    return runtimePass && previewPass ? 0 : 1;
}
