#pragma once

void TerrainResizeUsesSignedBeforeCanvasEdges() {
    const auto base = [] {
        auto args = Arguments(); args.insert(args.end(), {L"--terrain-resize", L"-32", L"-32", L"64", L"64"}); return args;
    };
    const auto valid = Invoke(base());
    Require(valid.exitCode != 0 && valid.output.empty() && valid.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "signed resize reaches the actual source gate without authorizing rules, clipping or object movement");
    for (const auto index : {9, 10, 11, 12}) {
        for (const auto value : {L"1.5", L"nan", L"2147483648", L"-2147483649"}) {
            auto args = base(); args[index] = value; Rejected(Invoke(args), "nonempty signed integer half-open");
        }
    }
    auto invalid = base(); invalid[11] = L"-32"; Rejected(Invoke(invalid), "nonempty signed integer half-open");
    invalid = base(); invalid[12] = L"-33"; Rejected(Invoke(invalid), "nonempty signed integer half-open");
    invalid = base(); invalid.pop_back(); Rejected(Invoke(invalid), "requires west south east north");
    invalid = base(); invalid.insert(invalid.end(), {L"--terrain-resize-move-objects", L"true"});
    Rejected(Invoke(invalid), "unknown map-edit-save option");
    auto tooMany = Arguments();
    for (unsigned i = 0; i < 129; ++i) tooMany.insert(tooMany.end(), {L"--terrain-resize", L"-32", L"-32", L"64", L"64"});
    Rejected(Invoke(tooMany), "at most 128 terrain");
}
