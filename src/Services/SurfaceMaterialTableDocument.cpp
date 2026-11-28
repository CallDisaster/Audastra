#include "War3MapCore/Services/SurfaceMaterialTableDocument.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace War3Map::Services {
namespace {
using Render::SurfaceMaterialKeyKind;
using Kind = Render::SurfaceMaterialLine::Kind;

Status Fail(std::string code, std::string message) {
    return Status::Failure(std::string(kSurfaceMaterialSubsystem), std::move(code), std::move(message));
}
std::size_t KeyIndex(const Render::SurfaceMaterialKey& key) {
    return static_cast<std::size_t>(&key - Render::kSurfaceMaterialKeys.data());
}
bool CommentOnly(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    return first != std::string_view::npos && (text[first] == ';' || text[first] == '#');
}
std::string NumberText(float value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed);
    return written.ec == std::errc{} ? std::string(buffer, written.ptr) : std::to_string(value);
}
std::string RangeText(const Render::SurfaceMaterialKey& key) {
    return NumberText(key.minimum) + "–" + NumberText(key.maximum);
}
}

Result<std::string> NormalizeSurfaceMaterialValue(std::string_view keyName, std::string_view value) {
    using Return = Result<std::string>;
    const auto fail = [](std::string code, std::string message) {
        return Return::Failure(std::string(kSurfaceMaterialSubsystem), std::move(code), std::move(message));
    };
    const auto* key = Render::FindSurfaceMaterialKey(keyName);
    if (!key) return fail("SURFACE_MATERIAL_KEY_UNKNOWN", "未知键 " + std::string(keyName) + "。");
    const std::string name(key->name);
    switch (key->kind) {
    case SurfaceMaterialKeyKind::Image: {
        if (value.empty()) return fail("SURFACE_MATERIAL_VALUE_INVALID", name + " 的图片路径为空；不要图片时删除这个键。");
        if (value.front() == ' ' || value.front() == '\t' || value.back() == ' ' || value.back() == '\t')
            return fail("SURFACE_MATERIAL_VALUE_INVALID", name + " 的图片路径不能以空白开头或结尾。");
        for (const char c : value) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
                return fail("SURFACE_MATERIAL_VALUE_INVALID", name + " 的图片路径含控制字符。");
            if (c == ';' || c == '#')
                return fail("SURFACE_MATERIAL_VALUE_INVALID", name + " 的图片路径不能含“;”或“#”，表里会读成注释。");
        }
        return Return::Success(std::string(value));
    }
    case SurfaceMaterialKeyKind::Number: {
        const auto parsed = Render::ParseSurfaceMaterialNumber(value, *key);
        if (!parsed) return fail("SURFACE_MATERIAL_VALUE_INVALID", name + " 须是 " + RangeText(*key) + " 之间的数。");
        return Return::Success(NumberText(*parsed));
    }
    case SurfaceMaterialKeyKind::Choice:
        if (value != key->choices[0] && value != key->choices[1])
            return fail("SURFACE_MATERIAL_VALUE_INVALID", name + " 只能是 " + std::string(key->choices[0]) + " 或 "
                + std::string(key->choices[1]) + "。");
        return Return::Success(std::string(value));
    }
    return fail("SURFACE_MATERIAL_KEY_UNKNOWN", "未知键 " + name + "。");
}

int SurfaceMaterialTableDocument::Scan::Find(std::string_view rawcode) const {
    for (std::size_t i = 0; i < order.size(); ++i) if (order[i] == rawcode) return static_cast<int>(i);
    return -1;
}

SurfaceMaterialTableDocument SurfaceMaterialTableDocument::Parse(std::string_view text) {
    SurfaceMaterialTableDocument document;
    if (text.starts_with("\xEF\xBB\xBF")) { document.bom_ = "\xEF\xBB\xBF"; text.remove_prefix(3); }
    bool eolKnown = false;
    // 与读表相同按“\n”切行，行号一致；“\r\n”整体记作行尾。
    while (!text.empty()) {
        const auto end = text.find('\n');
        Line line;
        if (end == std::string_view::npos) {
            line.text = std::string(text);
            text = {};
        } else {
            const bool crlf = end > 0 && text[end - 1] == '\r';
            line.text = std::string(text.substr(0, crlf ? end - 1 : end));
            line.end = crlf ? "\r\n" : "\n";
            text.remove_prefix(end + 1);
            if (!eolKnown) { document.eol_ = line.end; eolKnown = true; }
        }
        document.lines_.push_back(std::move(line));
    }
    return document;
}

std::string SurfaceMaterialTableDocument::Text() const {
    std::string out = bom_;
    for (const auto& line : lines_) { out += line.text; out += line.end; }
    return out;
}

Render::SurfaceMaterialTable SurfaceMaterialTableDocument::Table() const {
    return Render::ParseSurfaceMaterialTable(Text());
}

SurfaceMaterialTableDocument::Scan SurfaceMaterialTableDocument::Read() const {
    Scan scan;
    scan.lines.reserve(lines_.size());
    int current = -1;
    for (const auto& line : lines_) {
        ScannedLine scanned{Render::ReadSurfaceMaterialLine(line.text), -1};
        if (scanned.read.kind == Kind::Section || scanned.read.kind == Kind::BadSection) {
            current = -1;
            if (scanned.read.kind == Kind::Section) {
                current = scan.Find(scanned.read.rawcode);
                if (current < 0 && scan.order.size() < Render::kSurfaceMaterialTableMaxTypes) {
                    scan.order.emplace_back(scanned.read.rawcode);
                    current = static_cast<int>(scan.order.size() - 1);
                }
            }
        }
        scanned.section = current;
        scan.lines.push_back(scanned);
    }
    return scan;
}

std::vector<SurfaceMaterialSection> SurfaceMaterialTableDocument::Sections() const {
    const auto scan = Read();
    std::vector<SurfaceMaterialSection> sections(scan.order.size());
    for (std::size_t i = 0; i < scan.order.size(); ++i) sections[i].rawcode = scan.order[i];
    for (std::size_t i = 0; i < scan.lines.size(); ++i) {
        const auto& line = scan.lines[i];
        if (line.section < 0) continue;
        auto& section = sections[static_cast<std::size_t>(line.section)];
        if (line.read.kind == Kind::Section) {
            if (!section.headers++) section.line = i + 1;
            continue;
        }
        if (line.read.kind != Kind::KeyValue) continue;
        const auto* key = Render::FindSurfaceMaterialKey(line.read.key);
        if (!key) {
            const std::string name(line.read.key);
            if (std::find(section.unknownKeys.begin(), section.unknownKeys.end(), name) == section.unknownKeys.end())
                section.unknownKeys.push_back(name);
            continue;
        }
        const auto index = KeyIndex(*key);
        const bool valid = key->kind == SurfaceMaterialKeyKind::Image
            || (key->kind == SurfaceMaterialKeyKind::Number && Render::ParseSurfaceMaterialNumber(line.read.value, *key))
            || (key->kind == SurfaceMaterialKeyKind::Choice
                && (line.read.value == key->choices[0] || line.read.value == key->choices[1]));
        section.invalid[index] = !valid;
        if (valid) section.values[index] = std::string(line.read.value);
    }
    return sections;
}

std::optional<SurfaceMaterialSection> SurfaceMaterialTableDocument::Section(std::string_view rawcode) const {
    for (auto& section : Sections()) if (section.rawcode == rawcode) return std::move(section);
    return std::nullopt;
}

void SurfaceMaterialTableDocument::Insert(std::size_t at, std::string text) {
    // 插在没有换行的最后一行之后时，先给它补上换行。
    if (at > 0 && at == lines_.size() && lines_.back().end.empty()) lines_.back().end = eol_;
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at), Line{std::move(text), eol_});
}

void SurfaceMaterialTableDocument::Erase(const std::vector<bool>& remove) {
    // 原文件末尾没有换行时，删除后新的最后一行也不加换行。
    const bool lastHadEnd = !lines_.empty() && !lines_.back().end.empty();
    std::vector<Line> kept;
    kept.reserve(lines_.size());
    for (std::size_t i = 0; i < lines_.size(); ++i) if (!remove[i]) kept.push_back(std::move(lines_[i]));
    lines_ = std::move(kept);
    if (!lines_.empty() && !lastHadEnd) lines_.back().end.clear();
}

Status SurfaceMaterialTableDocument::Set(std::string_view rawcode, std::string_view keyName, std::string_view value) {
    if (!Render::SurfaceMaterialRawcode(rawcode))
        return Fail("SURFACE_MATERIAL_RAWCODE_INVALID", "物体类型须是四个可打印字符：" + std::string(rawcode) + "。");
    auto normalized = NormalizeSurfaceMaterialValue(keyName, value);
    if (!normalized.Succeeded() || !normalized.value) return {std::move(normalized.diagnostics)};
    const auto& text = *normalized.value;
    const auto* key = Render::FindSurfaceMaterialKey(keyName);
    const auto scan = Read();
    const int section = scan.Find(rawcode);
    if (section < 0) {
        if (scan.order.size() >= Render::kSurfaceMaterialTableMaxTypes)
            return Fail("SURFACE_MATERIAL_TABLE_FULL", "材质表已有 256 种物体类型，不能再加 " + std::string(rawcode) + "。");
        if (!lines_.empty() && lines_.back().text.find_first_not_of(" \t\r") != std::string::npos) Insert(lines_.size(), {});
        Insert(lines_.size(), "[" + std::string(rawcode) + "]");
        Insert(lines_.size(), std::string(key->name) + "=" + text);
        return Status::Ok();
    }
    // 已写过：改最后一次出现的那一行。
    for (std::size_t i = scan.lines.size(); i-- > 0;) {
        const auto& line = scan.lines[i];
        if (line.section != section || line.read.kind != Kind::KeyValue || line.read.key != key->name) continue;
        lines_[i].text.replace(line.read.valueBegin, line.read.valueEnd - line.read.valueBegin, text);
        return Status::Ok();
    }
    // 没写过：本类型最后一节的最后一个已知键之后，沿用那一行的缩进与“=”两侧空白。
    std::size_t header = 0;
    for (std::size_t i = 0; i < scan.lines.size(); ++i)
        if (scan.lines[i].section == section && scan.lines[i].read.kind == Kind::Section) header = i;
    std::size_t after = header;
    for (std::size_t i = header + 1; i < scan.lines.size() && scan.lines[i].section == section
         && scan.lines[i].read.kind != Kind::Section; ++i)
        if (scan.lines[i].read.kind == Kind::KeyValue && Render::FindSurfaceMaterialKey(scan.lines[i].read.key)) after = i;
    std::string newLine = std::string(key->name) + "=" + text;
    if (after != header) {
        const auto& reference = lines_[after].text;
        const auto& read = scan.lines[after].read;
        const auto keyBegin = static_cast<std::size_t>(read.key.data() - reference.data());
        const auto keyEnd = keyBegin + read.key.size();
        newLine = reference.substr(0, keyBegin) + std::string(key->name)
            + reference.substr(keyEnd, read.valueBegin - keyEnd) + text;
    }
    Insert(after + 1, std::move(newLine));
    return Status::Ok();
}

Status SurfaceMaterialTableDocument::Unset(std::string_view rawcode, std::string_view keyName) {
    const auto* key = Render::FindSurfaceMaterialKey(keyName);
    if (!key) return Fail("SURFACE_MATERIAL_KEY_UNKNOWN", "未知键 " + std::string(keyName) + "。");
    const auto scan = Read();
    const int section = scan.Find(rawcode);
    if (section < 0) return Status::Ok();
    std::vector<bool> remove(lines_.size(), false);
    for (std::size_t i = 0; i < scan.lines.size(); ++i) {
        const auto& line = scan.lines[i];
        if (line.section != section || line.read.kind != Kind::KeyValue || line.read.key != key->name) continue;
        const auto& original = lines_[i].text;
        const auto comment = original.find_first_of(";#");
        if (comment == std::string::npos) { remove[i] = true; continue; }
        const auto indent = original.find_first_not_of(" \t");
        lines_[i].text = original.substr(0, std::min(indent, comment)) + original.substr(comment);
    }
    Erase(remove);
    return Status::Ok();
}

Status SurfaceMaterialTableDocument::RemoveSection(std::string_view rawcode) {
    const auto scan = Read();
    const int section = scan.Find(rawcode);
    if (section < 0) return Fail("SURFACE_MATERIAL_SECTION_MISSING", "材质表里没有 " + std::string(rawcode) + "。");
    std::vector<bool> remove(lines_.size(), false);
    for (std::size_t i = 0; i < scan.lines.size(); ++i) {
        if (scan.lines[i].section != section || scan.lines[i].read.kind != Kind::Section) continue;
        std::size_t end = i + 1;
        while (end < scan.lines.size() && scan.lines[end].read.kind != Kind::Section
            && scan.lines[end].read.kind != Kind::BadSection) ++end;
        // 紧挨下一节头的注释行属于下一节。
        if (end < scan.lines.size()) while (end > i + 1 && CommentOnly(lines_[end - 1].text)) --end;
        for (std::size_t j = i; j < end; ++j) remove[j] = true;
    }
    Erase(remove);
    return Status::Ok();
}
}
