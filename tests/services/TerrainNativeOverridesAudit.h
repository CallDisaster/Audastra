#pragma once
#include <bit>

int AuditTerrainNativeOverrides(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& output) {
    Require(!std::filesystem::exists(output), "fresh native override audit required");
    const auto source = Take(MapArchiveSession::OpenMinimapAudit(path));
    Require(expected.size() == 64 && source.SourceSha256() == expected, "native override audit source differs");
    nlohmann::json tables = nlohmann::json::array();
    for (const auto type : {ObjectType::Destructable, ObjectType::Doodad}) {
        const auto member = std::string(NativeFileName(type));
        if (!source.HasMember(member)) { tables.push_back({{"member", member}, {"present", false}}); continue; }
        const auto bytes = Take(source.ReadMember(member));
        const auto parsed = NativeObjectCodec(type).Parse(bytes);
        Require(parsed.Succeeded() && parsed.document, "native override audit codec rejected the source");
        const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
        Require(document && document->IsStructurallySafe() && document->Objects().size() <= 4096,
            "bounded complete native document required");
        nlohmann::json objects = nlohmann::json::array(); std::size_t count = 0;
        for (const auto& object : document->Objects()) {
            nlohmann::json fields = nlohmann::json::array();
            for (const auto& modification : object.modifications) {
                Require(++count <= 32768, "native override audit field budget");
                const auto wire = document->ModificationValueType(object.key, modification.key);
                nlohmann::json item{{"field", modification.key.fieldId}, {"opaque", modification.valueWasOpaque},
                    {"value_kind", std::string(ValueKindName(modification.value))},
                    {"wire_type", wire ? nlohmann::json(static_cast<int>(*wire)) : nlohmann::json(nullptr)},
                    {"level", modification.key.level ? nlohmann::json(*modification.key.level) : nlohmann::json(nullptr)},
                    {"variation", modification.key.variation ? nlohmann::json(*modification.key.variation) : nlohmann::json(nullptr)},
                    {"data_index", modification.key.dataIndex ? nlohmann::json(*modification.key.dataIndex) : nlohmann::json(nullptr)}};
                if (const auto* value = std::get_if<std::int32_t>(&modification.value)) item["int32"] = *value;
                if (const auto* value = std::get_if<float>(&modification.value)) item["float_raw_u32"] = std::bit_cast<std::uint32_t>(*value);
                if (const auto* value = std::get_if<std::string>(&modification.value)) {
                    item["string_bytes"] = value->size(); item["string_sha256"] = Take(Sha256(std::as_bytes(std::span(*value))));
                }
                fields.push_back(std::move(item));
            }
            objects.push_back({{"rawcode", object.key.rawcode},
                {"parent", object.parentRawcode ? nlohmann::json(*object.parentRawcode) : nlohmann::json(nullptr)}, {"fields", fields}});
        }
        tables.push_back({{"member", member}, {"present", true}, {"sha256", Take(Sha256(bytes))},
            {"version", document->Version()}, {"original_count", document->OriginalObjectCount()},
            {"custom_count", document->CustomObjectCount()}, {"objects", objects}});
    }
    Check(source.VerifySourceUnchanged()); std::filesystem::create_directories(output);
    const auto report = nlohmann::json{{"source_sha256", expected}, {"source_unchanged", true}, {"tables", tables}, {"map_written", false}}.dump(2);
    Check(IO::WriteFileBytes(output / "report.json", std::as_bytes(std::span(report))));
    std::cout << report << '\n'; return 0;
}
