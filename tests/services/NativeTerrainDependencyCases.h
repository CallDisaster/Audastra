// 在NativeDestructablePathingTests.cpp的原创Fixture之后纳入，不读取作者或游戏资源。
Result<NativeTerrainDependencyDescription> DependencyDescription(const Fixture& fixture,
    std::initializer_list<std::string_view> placed, std::initializer_list<std::string_view> special = {},
    std::size_t maximum = kWorkingBytes) {
    return InspectNativeTerrainDependencies(std::span(placed.begin(), placed.size()),
        std::span(special.begin(), special.size()), fixture.sources, maximum);
}

Fixture DependencyBridgeFixture() {
    auto fixture = BridgeOriginalFixture();
    fixture.AddDoodadMetadata();
    fixture.metadata.push_back({{"ID", "Art0"}, {"field", "file"}, {"slk", "DestructableData"},
        {"type", "model"}, {"index", "0"}});
    fixture.Tables();
    return fixture;
}

void NativeDependencyBridgeArtIsNotAPathingQualification() {
    auto fixture = DependencyBridgeFixture();
    auto document = Empty(); Check(document.AddOriginalObject("S001"));
    Modification(document, "S001", "Art0", NativeValueType::String, std::string("Models\\First.mdx"));
    fixture.SetNative(document);
    const auto before = Take(DependencyDescription(fixture, {"S001"}));
    Require(before.objects.size() == 1 && before.objects[0].contribution == NativeTerrainDependencyContribution::WalkableBridge
        && before.objects[0].pathTexture == "PathTextures\\SyntheticBridge.tga", "probe读取桥实际占地，不校验外观保存资格");
    Rejected(fixture.Inspect({"S001"}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "完整原生重建严格外观门保持");
    Check(document.SetModification({ObjectType::Destructable, "S001"}, {"Art0"}, std::string("Models\\Second.mdx")));
    fixture.SetNative(document);
    const auto afterArt = Take(DependencyDescription(fixture, {"S001"}));
    Require(before.objects == afterArt.objects, "已知模型外观字段修改不改变静态通行描述");
    Modification(document, "S001", "Pth0", NativeValueType::String, std::string("PathTextures\\Changed.tga"));
    fixture.SetNative(document);
    const auto afterPath = Take(DependencyDescription(fixture, {"S001"}));
    Require(afterPath.objects[0].pathTexture == "PathTextures\\Changed.tga" && before.objects != afterPath.objects,
        "相同W3E的真实pathTex覆盖仍必须被probe发现");
}

void NativeDependencyDynamicObjectsDoNotAcquireStaticPathing() {
    auto fixture = DependencyBridgeFixture();
    auto document = Empty(); Check(document.AddOriginalObject("S002"));
    Modification(document, "S002", "Pth0", NativeValueType::String, std::string("PathTextures\\DynamicOne.tga"));
    fixture.SetNative(document);
    const auto before = Take(DependencyDescription(fixture, {"S002"}));
    Check(document.SetModification({ObjectType::Destructable, "S002"}, {"Pth0"}, std::string("PathTextures\\DynamicTwo.tga")));
    fixture.SetNative(document);
    const auto after = Take(DependencyDescription(fixture, {"S002"}));
    Require(before.objects == after.objects && before.objects[0].contribution == NativeTerrainDependencyContribution::Dynamic
        && before.objects[0].pathTexture.empty(), "walkable=0的动态占地不烘成静态WPM依赖");
}

void NativeDependencyUnknownFieldsRetainExactWireIdentity() {
    auto fixture = DependencyBridgeFixture();
    auto document = Empty(); Check(document.AddOriginalObject("S001"));
    Modification(document, "S001", "ZZZ0", NativeValueType::Int32, std::int32_t{7});
    fixture.SetNative(document);
    const auto source = fixture.sources;
    const auto before = Take(DependencyDescription(fixture, {"S001"}));
    const auto repeated = Take(DependencyDescription(fixture, {"S001"}));
    Require(before.objects == repeated.objects && before.objects[0].unknownFieldsSha256.size() == 64,
        "原字未变的未知字段有稳定独立身份，不能变成无未知");
    for (std::size_t i = 0; i < source.size(); ++i)
        Require(source[i].name == fixture.sources[i].name && source[i].present == fixture.sources[i].present
            && source[i].bytes == fixture.sources[i].bytes, "probe不改写任何原生对象或SLK");
    Check(document.SetModification({ObjectType::Destructable, "S001"}, {"ZZZ0"}, std::int32_t{8}));
    fixture.SetNative(document);
    const auto after = Take(DependencyDescription(fixture, {"S001"}));
    Require(before.objects[0].unknownFieldsSha256 != after.objects[0].unknownFieldsSha256,
        "未知字段实际变动不能被静默判无关");
}

void NativeDependencyParentArtAndUnknownOwnershipArePreserved() {
    auto fixture = DependencyBridgeFixture();
    auto document = Empty();
    Check(document.AddOriginalObject("S001")); Check(document.AddCustomObject("C001", "S001"));
    Modification(document, "S001", "Art0", NativeValueType::String, std::string("Models\\Parent.mdx"));
    Modification(document, "S001", "ZZZ0", NativeValueType::Int32, std::int32_t{7});
    fixture.SetNative(document);
    const auto parent = Take(DependencyDescription(fixture, {"C001"}));
    Require(parent.objects[0].parentRawcode == "S001" && !parent.objects[0].unknownFieldsSha256.empty(),
        "一层标准父的外观字段不成为占地门，未知wire仍保留身份");
    Check(document.RemoveModification({ObjectType::Destructable, "S001"}, {"ZZZ0"}));
    Modification(document, "C001", "ZZZ0", NativeValueType::Int32, std::int32_t{7});
    fixture.SetNative(document);
    const auto child = Take(DependencyDescription(fixture, {"C001"}));
    Require(parent.objects[0].unknownFieldsSha256 != child.objects[0].unknownFieldsSha256,
        "相同未知wire从父移到子也不能冒充原字未变");
}

Fixture DependencyDoodadFixture() {
    Fixture fixture;
    fixture.doodads[0]["pathTex"] = "PathTextures\\SyntheticDoodad.tga";
    fixture.doodads[0]["fixedRot"] = "-1";
    fixture.doodads[0]["category"] = "Z";
    fixture.doodadMetadata.push_back({{"ID", "Rot0"}, {"field", "fixedRot"}, {"slk", "DoodadData"},
        {"type", "unreal"}, {"repeat", "0"}, {"index", "0"}});
    fixture.doodadMetadata.push_back({{"ID", "Cat0"}, {"field", "category"}, {"slk", "DoodadData"},
        {"type", "string"}, {"repeat", "0"}, {"index", "0"}});
    fixture.doodadMetadata.push_back({{"ID", "Art0"}, {"field", "file"}, {"slk", "DoodadData"},
        {"type", "model"}, {"repeat", "0"}, {"index", "0"}});
    fixture.AddDoodadMetadata(); fixture.Tables();
    return fixture;
}

void NativeDependencyDoodadFieldsAndSpecialAreSeparate() {
    auto fixture = DependencyDoodadFixture();
    auto document = Empty(ObjectType::Doodad); Check(document.AddCustomObject("D002", "D001"));
    Modification(document, "D002", "Art0", NativeValueType::String, std::string("Models\\Doodad.mdx"));
    fixture.SetNative(document);
    const auto before = Take(DependencyDescription(fixture, {"D002"}, {"D002"}));
    Require(before.objects.size() == 2 && !before.objects[0].special && before.objects[1].special
        && before.objects[0].parentRawcode == "D001" && before.objects[0].category == "Z"
        && before.objects[0].fixedRotationDegrees == -1.f && before.objects[1].category.empty()
        && !before.objects[1].fixedRotationDegrees, "ordinary和special使用分开的实际依赖描述");
    Modification(document, "D002", "Rot0", NativeValueType::Unreal, 90.f);
    fixture.SetNative(document);
    const auto rotated = Take(DependencyDescription(fixture, {"D002"}, {"D002"}));
    Require(before.objects[0] != rotated.objects[0] && before.objects[1] == rotated.objects[1],
        "普通对象旋转依赖改变不能套给不读取旋转的special");
    Modification(document, "D002", "Dt00", NativeValueType::String, std::string{});
    fixture.SetNative(document);
    const auto cleared = Take(DependencyDescription(fixture, {"D002"}, {"D002"}));
    Require(cleared.objects[0].pathTexture.empty() && cleared.objects[1].pathTexture.empty(),
        "显式空占地覆盖不能回退标准非空占地");
}

void NativeDependencyBudgetAndMetadataRemainBounded() {
    auto fixture = DependencyDoodadFixture();
    auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
    Modification(document, "D001", "Dt00", NativeValueType::String, std::string("PathTextures\\Changed.tga"));
    fixture.SetNative(document);
    const auto result = Take(DependencyDescription(fixture, {"D001"}));
    Require(result.retainedBytes > sizeof(result) && result.retainedBytes < result.workBytes,
        "返回描述只保留正文容量，不把已经释放的catalog/codec解析峰值当作常驻正文");
    Require(result.workBytes && Take(DependencyDescription(fixture, {"D001"}, {}, result.workBytes)).workBytes == result.workBytes,
        "只读probe精确累计预算可复用");
    Rejected(DependencyDescription(fixture, {"D001"}, {}, result.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET", "probe预算不能超支一字节");
    Rejected(DependencyDescription(fixture, {"D001"}, {}, result.retainedBytes), "TERRAIN_NATIVE_OBJECT_BUDGET",
        "常驻描述预算不能冒充单阶段完整解析预算");
    fixture.doodadMetadata.push_back({{"ID", "Bad0"}, {"field", "pathTex"}, {"slk", ""}, {"type", "string"}});
    fixture.Tables();
    Rejected(DependencyDescription(fixture, {"D001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "原始metadata被丢弃的pathTex别名仍拒绝");
    fixture = DependencyDoodadFixture(); fixture.sources.pop_back();
    Rejected(DependencyDescription(fixture, {"D001"}), "TERRAIN_NATIVE_DEPENDENCY_SOURCE", "六项来源不能用未查询代替明确缺失");
}

void RunNativeTerrainDependencyCases() {
    NativeDependencyBridgeArtIsNotAPathingQualification();
    NativeDependencyDynamicObjectsDoNotAcquireStaticPathing();
    NativeDependencyUnknownFieldsRetainExactWireIdentity();
    NativeDependencyParentArtAndUnknownOwnershipArePreserved();
    NativeDependencyDoodadFieldsAndSpecialAreSeparate();
    NativeDependencyBudgetAndMetadataRemainBounded();
}
