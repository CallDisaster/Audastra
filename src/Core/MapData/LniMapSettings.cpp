#include "War3MapCore/MapData/LniMapSettings.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace War3Map::MapData {
namespace {
constexpr std::size_t maxBytes = 16 * 1024 * 1024;
constexpr std::size_t maxNodes = 250000;

// 字段名是文件格式词汇；映射仅用于读取，不修改原始文本。
constexpr std::pair<std::string_view, std::string_view> aliases[] = {
    {"地图", "map"}, {"文件版本", "file_version"}, {"地图版本", "map_version"},
    {"编辑器版本", "we_version"}, {"地图名称", "map_name"}, {"作者名字", "author_name"},
    {"地图描述", "map_desc"}, {"推荐玩家", "player_desc"}, {"镜头", "camera"},
    {"镜头范围", "camera_bound"}, {"镜头范围扩充", "camera_complement"}, {"地形", "map_info"},
    {"地图宽度", "map_width"}, {"地图长度", "map_height"}, {"地形类型", "map_main_ground"},
    {"选项", "config"}, {"使用的游戏数据设置", "game_data_setting"},
    {"关闭预览图", "disable_preview"}, {"自定义结盟优先权", "custom_ally"}, {"对战地图", "melee_map"},
    {"大型地图", "large_map"}, {"迷雾区域显示地形", "masked_area_show_terrain"},
    {"自定义玩家分组", "fix_force_setting"}, {"自定义队伍", "custom_force"},
    {"自定义科技树", "custom_techtree"}, {"自定义技能", "custom_ability"}, {"自定义升级", "custom_upgrade"},
    {"地图菜单标记", "map_menu_mark"}, {"地形悬崖显示水波", "show_wave_on_cliff"},
    {"地形起伏显示水波", "show_wave_on_rolling"}, {"载入图", "loading_screen"}, {"序号", "id"},
    {"路径", "path"}, {"文本", "text"}, {"标题", "title"}, {"子标题", "subtitle"}, {"战役", "prologue"},
    {"迷雾", "fog"}, {"类型", "type"}, {"z轴起点", "start_z"}, {"z轴终点", "end_z"},
    {"密度", "density"}, {"颜色", "color"}, {"环境", "environment"}, {"天气", "weather"},
    {"音效", "sound"}, {"光照", "light"}, {"水面颜色", "water_color"}, {"玩家", "player"},
    {"玩家数量", "player_count"}, {"种族", "race"}, {"修正出生点", "fix_start_position"}, {"名字", "name"},
    {"出生点", "start_position"}, {"低结盟优先权标记", "ally_low_flag"}, {"高结盟优先权标记", "ally_high_flag"},
    {"队伍", "force"}, {"队伍数量", "force_count"}, {"结盟", "ally"}, {"结盟胜利", "ally_win"},
    {"共享视野", "share_visible"}, {"共享单位控制", "share_control"}, {"共享高级单位设置", "share_advance"},
    {"玩家列表", "player_list"}, {"队伍名称", "force_name"}, {"升级", "upgrade"}, {"等级", "level"},
    {"可用性", "available"}, {"科技", "tech"}, {"随机组", "random_group"}, {"随机组名称", "random_group_name"},
    {"位置类型", "position_type"}, {"设置", "setting"}, {"几率", "chance"},
    {"物品列表", "random_item"}, {"物品列表名称", "random_item_name"}
};

std::string Key(std::string_view key) {
    for (const auto& [localized, canonical] : aliases) if (key == localized) return std::string(canonical);
    for (const auto prefix : {std::string_view("玩家"), std::string_view("队伍"), std::string_view("升级"),
        std::string_view("科技"), std::string_view("随机组"), std::string_view("物品列表")}) {
        if (key.starts_with(prefix) && key.size() > prefix.size()
            && key[prefix.size()] >= '0' && key[prefix.size()] <= '9')
            return Key(prefix) + std::string(key.substr(prefix.size()));
    }
    if (key.starts_with("未知")) return "unknown_" + std::string(key.substr(std::string_view("未知").size()));
    return std::string(key);
}

struct Value {
    enum class Kind { Number, String, Table } kind = Kind::Table;
    std::string text;
    std::map<std::string, Value> fields;
    std::size_t offset = 0;
    bool used = false;
};

struct ParseFailure : std::runtime_error {
    std::string code;
    std::size_t offset;
    ParseFailure(std::string code, std::string message, std::size_t offset)
        : std::runtime_error(std::move(message)), code(std::move(code)), offset(offset) {}
};

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {
        if (input_.starts_with("\xef\xbb\xbf")) position_ = 3;
    }
    Value Read() {
        Value root;
        Value* section = nullptr;
        while (true) {
            Space();
            if (position_ == input_.size()) break;
            if (Take('[')) {
                const auto begin = position_;
                const auto end = input_.find(']', begin);
                if (end == input_.npos || input_.substr(begin, end - begin).find_first_of("\r\n[") != input_.npos)
                    Fail("LNI_SYNTAX", "节名称未完整结束。");
                auto name = War3Map::Text::TrimLineWhitespace(input_.substr(begin, end - begin));
                if (name.empty()) Fail("LNI_SYNTAX", "节名称不能为空。");
                position_ = end + 1;
                auto [it, inserted] = root.fields.emplace(Key(name), Value{});
                if (!inserted) Fail("LNI_DUPLICATE", "重复或别名冲突的节：" + std::string(name));
                section = &it->second; section->offset = begin;
                EndLine();
            } else {
                if (!section) Fail("LNI_SYNTAX", "字段必须位于节内。");
                const auto name = Identifier();
                Horizontal(); Expect('=');
                auto value = ReadValue(0);
                if (!section->fields.emplace(Key(name), std::move(value)).second)
                    Fail("LNI_DUPLICATE", "重复或别名冲突的字段：" + name);
                EndLine();
            }
        }
        return root;
    }
private:
    [[noreturn]] void Fail(std::string code, std::string message) const {
        throw ParseFailure(std::move(code), std::move(message), position_);
    }
    bool Take(char c) { if (position_ < input_.size() && input_[position_] == c) { ++position_; return true; } return false; }
    void Expect(char c) { if (!Take(c)) Fail("LNI_SYNTAX", std::string("缺少字符：") + c); }
    void Horizontal() { while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t')) ++position_; }
    bool Comment() {
        if (position_ == input_.size()) return false;
        if (input_[position_] == ';' || input_.substr(position_, 2) == "--" || input_.substr(position_, 2) == "//") {
            while (position_ < input_.size() && input_[position_] != '\n') ++position_;
            return true;
        }
        return false;
    }
    void Space() {
        do {
            while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t'
                || input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
        } while (Comment());
    }
    void EndLine() {
        Horizontal(); Comment();
        if (position_ < input_.size() && input_[position_] != '\r' && input_[position_] != '\n')
            Fail("LNI_SYNTAX", "字段或节后存在不支持的表达式。");
    }
    std::string Identifier() {
        Horizontal(); const auto begin = position_;
        while (position_ < input_.size()) {
            const auto c = static_cast<unsigned char>(input_[position_]);
            if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_') ++position_;
            else break;
        }
        if (begin == position_) Fail("LNI_SYNTAX", "无效字段名或表索引。");
        return std::string(input_.substr(begin, position_ - begin));
    }
    std::string String() {
        if (Take('[')) {
            std::size_t equals = 0;
            while (Take('=')) { if (++equals > 16) Fail("LNI_BUDGET", "长字符串分隔符超出预算。"); }
            Expect('[');
            // Lua 长字符串忽略紧接开分隔符的第一行换行。
            Take('\r'); Take('\n');
            const std::string closing = "]" + std::string(equals, '=') + "]";
            const auto end = input_.find(closing, position_);
            if (end == input_.npos) Fail("LNI_SYNTAX", "长字符串未结束。");
            auto result = std::string(input_.substr(position_, end - position_)); position_ = end + closing.size(); return result;
        }
        const auto quote = input_[position_++];
        std::string result;
        while (position_ < input_.size()) {
            char c = input_[position_++];
            if (c == quote) return result;
            if (c == '\r' || c == '\n' || c == '\0') Fail("LNI_SYNTAX", "引号字符串包含未转义的换行或零字节。");
            if (c != '\\') { result += c; continue; }
            if (position_ == input_.size()) Fail("LNI_SYNTAX", "转义序列被截断。");
            c = input_[position_++];
            switch (c) {
            case 'a': result += '\a'; break; case 'b': result += '\b'; break; case 'f': result += '\f'; break;
            case 'n': result += '\n'; break; case 'r': result += '\r'; break; case 't': result += '\t'; break;
            case 'v': result += '\v'; break; case '\\': case '\'': case '"': result += c; break;
            default:
                if (c < '0' || c > '9') Fail("LNI_SYNTAX", "不支持的转义序列。");
                unsigned value = c - '0';
                for (int n = 1; n < 3 && position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9'; ++n)
                    value = value * 10 + (input_[position_++] - '0');
                if (value > 255) Fail("LNI_SYNTAX", "十进制字节转义超出范围。");
                result += static_cast<char>(value);
            }
        }
        Fail("LNI_SYNTAX", "引号字符串未结束。");
    }
    Value ReadValue(std::size_t depth) {
        Space();
        if (++nodes_ > maxNodes || depth > 32) Fail("LNI_BUDGET", "嵌套深度或字段数超出预算。");
        if (position_ == input_.size()) Fail("LNI_SYNTAX", "字段值被截断。");
        Value value; value.offset = position_;
        const auto c = input_[position_];
        if (c == '"' || c == '\'' || c == '[') {
            value.kind = Value::Kind::String; value.text = String(); return value;
        }
        if (Take('{')) {
            std::size_t implicitIndex = 1;
            Space();
            while (!Take('}')) {
                if (position_ == input_.size()) Fail("LNI_SYNTAX", "表未结束。");
                const auto begin = position_;
                std::string key;
                const auto first = static_cast<unsigned char>(input_[position_]);
                if (first >= 0x80 || (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')
                    || (first >= '0' && first <= '9') || first == '_') {
                    auto candidate = Identifier(); Horizontal();
                    if (Take('=')) key = Key(candidate); else position_ = begin;
                }
                if (key.empty()) key = std::to_string(implicitIndex++);
                auto child = ReadValue(depth + 1);
                if (!value.fields.emplace(key, std::move(child)).second) Fail("LNI_DUPLICATE", "表内存在重复索引或字段。");
                Space();
                if (Take('}')) break;
                Expect(','); Space();
            }
            return value;
        }
        value.kind = Value::Kind::Number;
        const auto begin = position_;
        while (position_ < input_.size()) {
            const auto n = input_[position_];
            if ((n >= '0' && n <= '9') || n == '-' || n == '+' || n == '.' || n == 'e' || n == 'E') ++position_;
            else break;
        }
        if (position_ == begin) Fail("LNI_SYNTAX", "只允许数值、字符串和数据表；不执行表达式。");
        value.text = std::string(input_.substr(begin, position_ - begin));
        double number = 0;
        const auto parsed = std::from_chars(value.text.data(), value.text.data() + value.text.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.text.data() + value.text.size() || !std::isfinite(number))
            Fail("LNI_NUMBER", "无效或非有限数值。");
        return value;
    }
    std::string_view input_;
    std::size_t position_ = 0;
    std::size_t nodes_ = 0;
};

[[noreturn]] void Invalid(const Value& value, std::string message, std::string code = "LNI_FIELD_INVALID") {
    throw ParseFailure(std::move(code), std::move(message), value.offset);
}
Value& Field(Value& value, std::string_view name) {
    value.used = true;
    const auto found = value.fields.find(std::string(name));
    if (value.kind != Value::Kind::Table || found == value.fields.end()) Invalid(value, "缺少必填字段：" + std::string(name), "LNI_FIELD_MISSING");
    return found->second;
}
std::int32_t Integer(Value& value, std::int32_t min = std::numeric_limits<std::int32_t>::min(),
    std::int32_t max = std::numeric_limits<std::int32_t>::max()) {
    value.used = true;
    std::int32_t number = 0;
    const auto parsed = std::from_chars(value.text.data(), value.text.data() + value.text.size(), number);
    if (value.kind != Value::Kind::Number || parsed.ec != std::errc{} || parsed.ptr != value.text.data() + value.text.size()
        || number < min || number > max) Invalid(value, "字段必须是范围内的整数。");
    return number;
}
float Real(Value& value) {
    value.used = true; float number = 0;
    const auto parsed = std::from_chars(value.text.data(), value.text.data() + value.text.size(), number);
    if (value.kind != Value::Kind::Number || parsed.ec != std::errc{} || parsed.ptr != value.text.data() + value.text.size()
        || !std::isfinite(number)) Invalid(value, "字段必须是有限实数。");
    return number;
}
std::string Text(Value& value, bool allowNull = false) {
    value.used = true;
    if (value.kind != Value::Kind::String || (!allowNull && value.text.find('\0') != value.text.npos))
        Invalid(value, "字段必须是合法字符串。");
    return value.text;
}
std::vector<Value*> Array(Value& value, std::size_t min = 0, std::size_t max = 10000) {
    value.used = true;
    if (value.kind != Value::Kind::Table || value.fields.size() < min || value.fields.size() > max)
        Invalid(value, "数组类型或长度不正确。");
    std::vector<Value*> result;
    for (std::size_t index = 1; index <= value.fields.size(); ++index) {
        const auto found = value.fields.find(std::to_string(index));
        if (found == value.fields.end()) Invalid(value, "数组必须使用连续的 1 基索引。");
        result.push_back(&found->second);
    }
    return result;
}
SettingsRawcode Rawcode(Value& value) {
    const auto text = Text(value, true);
    if (text.size() != 4) Invalid(value, "对象或天气 ID 必须为四个字节。");
    SettingsRawcode result; std::copy_n(reinterpret_cast<const std::byte*>(text.data()), 4, result.begin()); return result;
}
std::uint8_t Character(Value& value) {
    const auto text = Text(value, true);
    if (text.size() != 1) Invalid(value, "地形或光照类型必须为一个字节。");
    return static_cast<std::uint8_t>(text.front());
}
std::uint32_t Mask(Value& value) {
    std::uint32_t result = 0;
    for (auto* entry : Array(value, 0, 32)) {
        const auto index = Integer(*entry, 1, 32);
        const auto bit = std::uint32_t{1} << (index - 1);
        if (result & bit) Invalid(*entry, "玩家列表重复。");
        result |= bit;
    }
    return result;
}
SettingsColor Color(Value& value) {
    const auto parts = Array(value, 4, 4);
    return {static_cast<std::uint8_t>(Integer(*parts[0], 0, 255)), static_cast<std::uint8_t>(Integer(*parts[1], 0, 255)),
        static_cast<std::uint8_t>(Integer(*parts[2], 0, 255)), static_cast<std::uint8_t>(Integer(*parts[3], 0, 255))};
}
void Screen(Value& source, MapScreenSettings& target) {
    target.path = Text(Field(source, "path")); target.text = Text(Field(source, "text"));
    target.title = Text(Field(source, "title")); target.subtitle = Text(Field(source, "subtitle"));
}
std::vector<Value*> Sections(Value& root, std::string_view prefix, std::optional<std::size_t> expected = {}) {
    std::map<std::size_t, Value*> sections;
    for (auto& [key, value] : root.fields) {
        if (!key.starts_with(prefix) || key.size() == prefix.size()) continue;
        const auto suffix = std::string_view(key).substr(prefix.size());
        if (suffix.front() < '0' || suffix.front() > '9') continue;
        std::size_t index = 0;
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() || index == 0
            || std::to_string(index) != suffix || index > 10000) Invalid(value, "编号节必须使用有界的 1 基整数。");
        sections.emplace(index, &value);
    }
    if (expected && sections.size() != *expected) Invalid(root, "编号节数量与声明不一致：" + std::string(prefix));
    std::vector<Value*> result;
    for (std::size_t index = 1; index <= sections.size(); ++index) {
        const auto found = sections.find(index);
        if (found == sections.end()) Invalid(root, "编号节不连续：" + std::string(prefix));
        found->second->used = true; result.push_back(found->second);
    }
    return result;
}
void Unknown(const Value& value, const std::string& path, std::vector<std::string>& unknown) {
    if (!value.used) { unknown.push_back(path); return; }
    for (const auto& [key, child] : value.fields) Unknown(child, path.empty() ? key : path + "/" + key, unknown);
}

MapSettings Decode(Value& root) {
    MapSettings s;
    auto& map = Field(root, "map");
    s.formatVersion = Integer(Field(map, "file_version"));
    if (s.formatVersion != 25) Invalid(map, "首轮只支持经典 TFT v25 地图设置。", "LNI_VERSION_UNSUPPORTED");
    s.saves = Integer(Field(map, "map_version")); s.editorVersion = Integer(Field(map, "we_version"));
    s.mapName = Text(Field(map, "map_name")); s.author = Text(Field(map, "author_name"));
    s.description = Text(Field(map, "map_desc")); s.recommendedPlayers = Text(Field(map, "player_desc"));
    auto& camera = Field(root, "camera");
    const auto bounds = Array(Field(camera, "camera_bound"), 8, 8);
    for (std::size_t i = 0; i < bounds.size(); ++i) s.cameraBounds[i] = Real(*bounds[i]);
    const auto margins = Array(Field(camera, "camera_complement"), 4, 4);
    for (std::size_t i = 0; i < margins.size(); ++i) s.cameraMargins[i] = Integer(*margins[i]);
    auto& terrain = Field(root, "map_info");
    s.playableWidth = Integer(Field(terrain, "map_width"), 1); s.playableHeight = Integer(Field(terrain, "map_height"), 1);
    s.tileset = Character(Field(terrain, "map_main_ground"));
    auto& config = Field(root, "config"); s.gameDataSet = Integer(Field(config, "game_data_setting"));
    constexpr std::string_view flagNames[] = {"disable_preview", "custom_ally", "melee_map", "large_map",
        "masked_area_show_terrain", "fix_force_setting", "custom_force", "custom_techtree", "custom_ability",
        "custom_upgrade", "map_menu_mark", "show_wave_on_cliff", "show_wave_on_rolling", "unknown_1", "unknown_2",
        "unknown_3", "unknown_4", "unknown_5", "unknown_6", "unknown_7", "unknown_8", "unknown_9"};
    for (std::size_t i = 0; i < std::size(flagNames); ++i)
        s.flags |= static_cast<std::uint32_t>(Integer(Field(config, flagNames[i]), 0, 1)) << i;
    auto& loading = Field(root, "loading_screen"); s.loadingBackground = Integer(Field(loading, "id")); Screen(loading, s.loadingScreen);
    Screen(Field(root, "prologue"), s.prologueScreen);
    auto& fog = Field(root, "fog");
    s.environment.fogStyle = Integer(Field(fog, "type")); s.environment.fogStart = Real(Field(fog, "start_z"));
    s.environment.fogEnd = Real(Field(fog, "end_z")); s.environment.fogDensity = Real(Field(fog, "density"));
    s.environment.fogColor = Color(Field(fog, "color"));
    auto& environment = Field(root, "environment");
    s.environment.weather = Rawcode(Field(environment, "weather")); s.environment.soundEnvironment = Text(Field(environment, "sound"));
    s.environment.lightEnvironment = Character(Field(environment, "light")); s.environment.waterColor = Color(Field(environment, "water_color"));
    const auto players = Integer(Field(Field(root, "player"), "player_count"), 0, 32);
    for (auto* section : Sections(root, "player", players)) {
        MapPlayerSettings p;
        p.number = Integer(Field(*section, "player"), 0, 31); p.controller = Integer(Field(*section, "type"));
        p.race = Integer(Field(*section, "race")); p.flags = static_cast<std::uint32_t>(Integer(Field(*section, "fix_start_position"), 0));
        p.name = Text(Field(*section, "name")); const auto start = Array(Field(*section, "start_position"), 2, 2);
        p.startX = Real(*start[0]); p.startY = Real(*start[1]);
        p.allyLowPriorityMask = Mask(Field(*section, "ally_low_flag")); p.allyHighPriorityMask = Mask(Field(*section, "ally_high_flag"));
        s.players.push_back(std::move(p));
    }
    const auto forces = Integer(Field(Field(root, "force"), "force_count"), 0, 32);
    for (auto* section : Sections(root, "force", forces)) {
        MapForceSettings f;
        constexpr std::pair<std::string_view, unsigned> flags[] = {
            {"ally", 0}, {"ally_win", 1}, {"share_visible", 3}, {"share_control", 4}, {"share_advance", 5}};
        for (const auto& [name, shift] : flags) f.flags |= static_cast<std::uint32_t>(Integer(Field(*section, name), 0, 1)) << shift;
        // 仅读取声明的掩码，不模拟写出工具给首队补入未使用玩家的行为。
        f.playerMask = Mask(Field(*section, "player_list")); f.name = Text(Field(*section, "force_name")); s.forces.push_back(std::move(f));
    }
    MapCustomSettings custom;
    for (auto* section : Sections(root, "upgrade")) custom.upgrades.push_back({Mask(Field(*section, "player_list")),
        Rawcode(Field(*section, "ID")), Integer(Field(*section, "level"), 0), Integer(Field(*section, "available"))});
    for (auto* section : Sections(root, "tech")) custom.techRestrictions.push_back({Mask(Field(*section, "player_list")), Rawcode(Field(*section, "ID"))});
    for (auto* section : Sections(root, "random_group")) {
        MapRandomUnitTable table; table.number = static_cast<std::int32_t>(custom.randomUnits.size());
        table.name = Text(Field(*section, "random_group_name"));
        for (auto* type : Array(Field(*section, "position_type"), 1, 100)) table.columnTypes.push_back(Integer(*type, 0, 2));
        for (auto* row : Array(Field(*section, "setting"))) {
            MapRandomUnitRow entry; entry.chance = Integer(Field(*row, "chance"), 0, 100);
            for (auto* id : Array(Field(*row, "ID"), table.columnTypes.size(), table.columnTypes.size())) entry.ids.push_back(Rawcode(*id));
            table.rows.push_back(std::move(entry));
        }
        custom.randomUnits.push_back(std::move(table));
    }
    for (auto* section : Sections(root, "random_item")) {
        MapRandomItemTable table; table.number = static_cast<std::int32_t>(custom.randomItems.size());
        table.name = Text(Field(*section, "random_item_name"));
        for (auto* set : Array(Field(*section, "setting"))) {
            std::vector<MapRandomItem> items;
            for (auto* item : Array(*set)) items.push_back({Integer(Field(*item, "chance"), 0, 100), Rawcode(Field(*item, "ID"))});
            table.sets.push_back(std::move(items));
        }
        custom.randomItems.push_back(std::move(table));
    }
    s.custom = std::move(custom);
    return s;
}
} // namespace

Result<LniMapSettings> ParseLniMapSettings(std::span<const std::byte> data) {
    if (data.size() > maxBytes) return Result<LniMapSettings>::Failure("Map.LniSettings", "LNI_BUDGET", "LNI 地图设置超过 16 MiB 读取预算。");
    try {
        const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
        if (text.find('\0') != text.npos) throw ParseFailure("LNI_SYNTAX", "LNI 原文包含未转义的零字节。", text.find('\0'));
        auto root = Reader(text).Read();
        LniMapSettings document; document.settings = Decode(root);
        document.originalBytes.assign(data.begin(), data.end()); Unknown(root, "", document.unknownFields);
        document.fullyParsed = document.unknownFields.empty();
        auto result = Result<LniMapSettings>::Success(std::move(document));
        for (const auto& name : result.value->unknownFields) result.diagnostics.push_back({DiagnosticSeverity::Warning,
            "Map.LniSettings", "LNI_UNKNOWN_FIELD", "保留未识别字段：" + name});
        return result;
    } catch (const ParseFailure& failure) {
        auto result = Result<LniMapSettings>::Failure("Map.LniSettings", failure.code, failure.what());
        result.diagnostics.front().origin = OriginSpan{std::filesystem::path("table/w3i.ini"), failure.offset, 1, {}};
        return result;
    }
}

} // namespace War3Map::MapData
