#include "War3MapCore/HLua/ObjectMapping.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Text/LniData.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::HLua;
void Need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void Good(const T& result) {
    if (result.Succeeded()) return;
    std::string message;
    for (const auto& d : result.diagnostics) message += d.code + ": " + d.message + "\n";
    throw std::runtime_error(message);
}
std::vector<std::byte> Bytes(std::string_view value) { return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())}; }
using Row = std::map<std::string, std::string>;
std::string Slk(const std::vector<Row>& rows) {
    std::vector<std::string> columns{"ID"};
    for (const auto& row : rows) for (const auto& [key, value] : row)
        if (std::find(columns.begin(), columns.end(), key) == columns.end()) columns.push_back(key);
    std::string text = "ID;PWXL\n";
    for (std::size_t y = 0; y <= rows.size(); ++y) for (std::size_t x = 0; x < columns.size(); ++x) {
        auto value = y == 0 ? columns[x] : (rows[y - 1].contains(columns[x]) ? rows[y - 1].at(columns[x]) : "");
        text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y + 1) + ";K\"" + value + "\"\n";
    }
    return text + "E\n";
}
}
void TestHLuaObjectMapping() {
    std::map<std::string, std::string> files;
    files["unit-meta.slk"] = Slk({
        {{"ID","unam"},{"field","Name"},{"slk","Profile"},{"type","string"},{"index","0"}},
        {{"ID","ussc"},{"field","scale"},{"slk","UnitUI"},{"type","real"}},
        {{"ID","ubpx"},{"field","Buttonpos"},{"slk","Profile"},{"type","int"},{"index","0"}},
        {{"ID","ubpy"},{"field","Buttonpos"},{"slk","Profile"},{"type","int"},{"index","1"}}});
    files["ability-meta.slk"] = Slk({
        {{"ID","Hbz1"},{"field","Data"},{"slk","AbilityData"},{"type","unreal"},{"repeat","4"},{"data","1"},{"useSpecific","AHbz"}},
        {{"ID","acdn"},{"field","Cool"},{"slk","AbilityData"},{"type","unreal"},{"repeat","4"}}});
    files["units.slk"] = Slk({{{"ID","hfoo"},{"code","hfoo"},{"Name","Parent"}}});
    files["abilities.slk"] = Slk({{{"ID","AHbz"},{"code","AHbz"},{"Name","Ability parent"}}});
    const auto read = [&](const std::string& path) {
        const auto found = files.find(path);
        return found == files.end() ? Result<std::vector<std::byte>>::Failure("Test", "missing", path)
            : Result<std::vector<std::byte>>::Success(Bytes(found->second));
    };
    ObjectFieldSchemaOptions schemaOptions;
    schemaOptions.sources = {{ObjectType::Unit,"unit-meta.slk"},{ObjectType::Ability,"ability-meta.slk"}};
    schemaOptions.stringFiles.clear();
    auto schema = ObjectFieldSchema::Load(read, schemaOptions); Good(schema);
    CatalogLoadOptions catalogOptions;
    catalogOptions.sources = {{ObjectType::Unit,{"units.slk"},{}},{ObjectType::Ability,{"abilities.slk"},{}}};
    catalogOptions.stringFiles.clear();
    auto standard = StandardObjectCatalog::Load(read, catalogOptions); Good(standard);
    const auto empty = [](ObjectType type) {
        auto parsed = LniObjectCodec(type).Parse({});
        Need(parsed.Succeeded(), "empty LNI");
        return *dynamic_cast<LniObjectDocument*>(parsed.document.get());
    };
    auto unit = ParseF6ObjectIni(ObjectType::Unit,
        "[v000]\n_parent=\"hfoo\"\nName=[=[\nline \"one\"\nline two]=]\nscale=0.13799999999999998\nButtonpos_1=0\nButtonpos_2=2\n", "unit.ini");
    Good(unit);
    auto edited = ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *unit.value, *schema.value, *standard.value); Good(edited);
    auto native = Services::ObjectTransactionSnapshot::Freeze(*edited.value, std::nullopt, std::string(64,'a'), &*schema.value, &*standard.value); Good(native);
    const auto* object = native.value->get()->CandidateNative().FindObject({ObjectType::Unit,"v000"});
    Need(object && object->modifications.size() == 4, "native address count");
    auto scale = std::find_if(object->modifications.begin(), object->modifications.end(), [](const auto& f) { return f.key.fieldId == "ussc"; });
    Need(scale != object->modifications.end() && std::get<float>(scale->value) == static_cast<float>(0.13799999999999998), "real retains float32 value");
    auto ability = ParseF6ObjectIni(ObjectType::Ability,
        "[K000]\n_parent=\"AHbz\"\nDataA={0.125,0.001}\nCool={1=3,2=4}\n", "ability.ini"); Good(ability);
    auto staged = ApplyHslkObjects(ObjectType::Ability, empty(ObjectType::Ability), *ability.value, *schema.value, *standard.value); Good(staged);
    auto encoded = Services::ObjectTransactionSnapshot::Freeze(*staged.value, std::nullopt, std::string(64,'a'), &*schema.value, &*standard.value); Good(encoded);
    const auto* skill = encoded.value->get()->CandidateNative().FindObject({ObjectType::Ability,"K000"});
    Need(skill && skill->modifications.size() == 4, "data letter and repeat addresses");
    Need(!ParseF6ObjectIni(ObjectType::Unit,"[v000]\nName=true\n","unit.ini").Succeeded(), "boolean rejected");
    Need(!ParseF6ObjectIni(ObjectType::Unit,"[v000]\nName={{1}}\n","unit.ini").Succeeded(), "nested rejected");
    Need(!ParseF6ObjectIni(ObjectType::Unit,"[v000]\nName={2=1}\n","unit.ini").Succeeded(), "sparse rejected");
    unit.value->front().fields.push_back({"unknown", {1.0}, false});
    Need(!ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *unit.value, *schema.value, *standard.value).Succeeded(), "unknown not dropped");
}
