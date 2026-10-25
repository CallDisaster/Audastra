#pragma once

void TerrainBlockCopyUsesBoundedTypedCommand() {
    const auto base = [] {
        auto args = Arguments();
        args.insert(args.end(), {L"--terrain-copy", L"4", L"5", L"4", L"3", L"20", L"21"});
        return args;
    };
    const auto valid = Invoke(base());
    Require(valid.exitCode != 0 && valid.output.empty() && valid.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "typed block command must reach the shared source gate without client raw bytes or digest");
    for (const auto index : {9, 10, 11, 12, 13, 14}) {
        for (const auto value : {L"-1", L"1.5", L"nan", L"2049", L"4294967295"}) {
            auto args = base(); args[index] = value;
            Rejected(Invoke(args), "indices must be integers");
        }
    }
    for (const auto index : {11, 12}) {
        auto args = base(); args[index] = L"0";
        Rejected(Invoke(args), "nonempty source and destination");
    }
    auto overflow = base(); overflow[13] = L"2047";
    Rejected(Invoke(overflow), "nonempty source and destination");
    auto incomplete = base(); incomplete.pop_back();
    Rejected(Invoke(incomplete), "requires source x/y");
    const std::vector<std::wstring> ordinary{L"--stroke", L"raise", L"circle", L"4", L"4", L"0", L"1"};
    auto copyFirst = base(); copyFirst.insert(copyFirst.end(), ordinary.begin(), ordinary.end());
    Rejected(Invoke(copyFirst), "only terrain command");
    auto copyLast = Arguments(); copyLast.insert(copyLast.end(), ordinary.begin(), ordinary.end());
    const auto block = base(); copyLast.insert(copyLast.end(), block.begin() + 8, block.end());
    Rejected(Invoke(copyLast), "only terrain command");
    auto duplicate = base(); duplicate.insert(duplicate.end(), block.begin() + 8, block.end());
    Rejected(Invoke(duplicate), "only terrain command");
    auto raw = base(); raw.insert(raw.end(), {L"--terrain-copy-raw", L"00"});
    Rejected(Invoke(raw), "unknown map-edit-save option");
}
