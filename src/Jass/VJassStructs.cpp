#include "VJassTranslate.h"

#include <algorithm>

// vJass 翻译：结构的命名、成员 / 方法查找，以及生成的全局变量、分配器、析构器、onDestroy 触发器与初始化函数。
// 生成代码的形态与 jasshelper 的输出一致（按其可观察结果对齐，独立实现），实例上限 8190。
namespace War3Map::Jass::detail {
namespace {

bool ContainsReturn(const std::vector<std::int32_t>& statements, const SyntaxPool& pool) {
    for (const auto index : statements) {
        if (pool.budget) pool.budget->Poll();
        const Stmt& stmt = pool.StmtAt(index);
        if (stmt.kind == StmtKind::Return) return true;
        if (stmt.kind == StmtKind::Loop && ContainsReturn(stmt.body, pool)) return true;
        if (stmt.kind == StmtKind::If) {
            for (const auto& branch : stmt.branches) {
                if (pool.budget) pool.budget->Poll();
                if (ContainsReturn(branch.body, pool)) return true;
            }
        }
    }
    return false;
}

} // namespace

std::string Translation::MethodName(const VFunction& method) const {
    const std::string base = "s__" + program_.structs[static_cast<std::size_t>(method.structIndex)].fullName;
    switch (method.op) {
    case OperatorKind::Get: return base + "__get_" + std::string(method.decl.name);
    case OperatorKind::Set: return base + "__set_" + std::string(method.decl.name);
    case OperatorKind::Index: return base + (method.isStatic ? "__staticgetindex" : "__getindex");
    case OperatorKind::IndexSet: return base + (method.isStatic ? "__staticsetindex" : "__setindex");
    default: return base + "_" + std::string(method.decl.name);
    }
}

const VMember* Translation::FindMember(std::int32_t structIndex, std::string_view name, std::int32_t& owner) const {
    for (auto current = structIndex; current >= 0; current = program_.structs[static_cast<std::size_t>(current)].parent) {
        budget_.Poll();
        for (const auto& member : program_.structs[static_cast<std::size_t>(current)].members) {
            budget_.Poll();
            if (member.decl.name == name) {
                owner = current;
                return &member;
            }
        }
    }
    return nullptr;
}

std::int32_t Translation::FindMethod(std::int32_t structIndex, std::string_view name, OperatorKind op, int staticness) const {
    for (auto current = structIndex; current >= 0; current = program_.structs[static_cast<std::size_t>(current)].parent) {
        budget_.Poll();
        for (const auto method : program_.structs[static_cast<std::size_t>(current)].methods) {
            budget_.Poll();
            const auto& function = program_.functions[static_cast<std::size_t>(method)];
            if (function.op != op) continue;
            if (op != OperatorKind::Index && op != OperatorKind::IndexSet && function.decl.name != name) continue;
            if (staticness >= 0 && function.isStatic != (staticness == 1)) continue;
            return method;
        }
    }
    return -1;
}

std::int32_t Translation::Root(std::int32_t structIndex) const {
    while (program_.structs[static_cast<std::size_t>(structIndex)].parent >= 0) {
        budget_.Poll();
        structIndex = program_.structs[static_cast<std::size_t>(structIndex)].parent;
    }
    return structIndex;
}

bool Translation::HasChildren(std::int32_t structIndex) const {
    return !program_.structs[static_cast<std::size_t>(structIndex)].children.empty();
}

std::string Translation::DestroyName(std::int32_t structIndex, const CodeScope& scope) const {
    const auto root = Root(structIndex);
    const auto& data = program_.structs[static_cast<std::size_t>(root)];
    // 有子结构：按实际类型经触发器调用 onDestroy；只有 onDestroy：定义之前经触发器，之后直接调用。
    const bool viaTrigger = HasChildren(root) || (data.onDestroy >= 0 && scope.order <= destructorOrder_[static_cast<std::size_t>(root)]);
    return (viaTrigger ? "sc__" : "s__") + data.fullName + "_deallocate";
}

void Translation::PrepareDestructors() {
    for (const auto index : structOrder_) {
        budget_.Poll();
        const auto& data = program_.structs[static_cast<std::size_t>(index)];
        if (data.onDestroy < 0) continue;
        const auto& method = program_.functions[static_cast<std::size_t>(data.onDestroy)];
        const auto& body = bodies_[static_cast<std::size_t>(data.onDestroy)];
        if (method.isStatic || !method.decl.params.empty() || !method.decl.returns.empty() || ContainsReturn(body.statements, body.pool)) {
            Fail(method.decl.line, "onDestroy 带参数、返回值或 return 语句");
            continue;
        }
        CodeScope scope = ScopeFor(data.onDestroy);
        destroyBodies_[data.onDestroy] = BodyText(data.onDestroy, scope);
        destroyCallsMap_[data.onDestroy] = scope.callsMap;
    }
}

MappedText Translation::MemberInits(std::int32_t structIndex) {
    const auto& data = program_.structs[static_cast<std::size_t>(structIndex)];
    MappedOutput text(trace_);
    text.AnchorLine(data.line);
    CodeScope scope;
    scope.pool = &program_.pool;
    scope.library = data.library;
    scope.structIndex = structIndex;
    for (const auto& member : data.members) {
        budget_.Poll();
        if (member.isStatic || member.decl.init < 0) continue;
        const Rendered init = Render(member.decl.init, scope);
        text.AnchorLine(member.decl.line);
        text += "   set s__" + data.fullName + "_" + std::string(member.decl.name) + "[this]= " + TextOf(init, program_.pool.At(member.decl.init)) + "\n";
    }
    return text.Take();
}

void Translation::EmitStructGlobals(MappedOutput& out) {
    for (const auto index : structOrder_) {
        budget_.Poll();
        const auto& data = program_.structs[static_cast<std::size_t>(index)];
        out.AnchorLine(data.line);
        const auto& name = data.fullName;
        out += "constant integer si__" + name + "=" + std::to_string(data.typeId) + "\n";
        if (data.parent < 0) out += "integer si__" + name + "_F=0\ninteger si__" + name + "_I=0\ninteger array si__" + name + "_V\n";
        CodeScope scope;
        scope.pool = &program_.pool;
        scope.library = data.library;
        scope.structIndex = index;
        for (const auto& member : data.members) {
            budget_.Poll();
            out.AnchorLine(member.decl.line);
            const auto type = OutputType(ResolveType(member.decl.type, data.library, index));
            const auto memberName = "s__" + name + "_" + std::string(member.decl.name);
            if (!member.isStatic || member.decl.array) {
                out += type + " array " + memberName + "\n";
                continue;
            }
            if (member.decl.constant) out += "constant ";
            out += type + " " + memberName;
            if (member.decl.init >= 0) {
                const Rendered init = Render(member.decl.init, scope);
                out += "= " + TextOf(init, program_.pool.At(member.decl.init));
            }
            out += "\n";
        }
    }
    for (const auto index : structOrder_) {
        budget_.Poll();
        const auto& data = program_.structs[static_cast<std::size_t>(index)];
        if (data.parent < 0 && HasChildren(index)) {
            out += "integer array si__" + data.fullName + "_type\ntrigger array st__" + data.fullName + "_onDestroy\n";
        }
    }
    for (const auto index : structOrder_) {
        budget_.Poll();
        const auto& data = program_.structs[static_cast<std::size_t>(index)];
        if (data.parent < 0 && !HasChildren(index) && data.onDestroy >= 0) out += "trigger st__" + data.fullName + "_onDestroy\n";
    }
    if (usesTriggers_) out += "integer f__arg_this\n";
}

MappedText Translation::Destructor(std::int32_t structIndex, bool direct) const {
    const auto& data = program_.structs[static_cast<std::size_t>(structIndex)];
    const auto& name = data.fullName;
    const bool children = HasChildren(structIndex);
    const bool trigger = !direct && (children || data.onDestroy >= 0);
    MappedOutput text(trace_);
    text.AnchorLine(data.line);
    text += "//Generated destructor of " + name + "\nfunction " + (trigger ? "sc__" : "s__") + name +
        "_deallocate takes integer this returns nothing\n    if this==null then\n        return\n    elseif (si__" + name +
        "_V[this]!=-1) then\n        return\n    endif\n";
    if (direct) {
        text += "    call s__" + name + "_onDestroy(this)\n";
    } else if (children) {
        text += "    set f__arg_this=this\n    call TriggerEvaluate(st__" + name + "_onDestroy[si__" + name + "_type[this]])\n";
    } else if (data.onDestroy >= 0) {
        text += "    set f__arg_this=this\n    call TriggerEvaluate(st__" + name + "_onDestroy)\n";
    }
    text += "    set si__" + name + "_V[this]=si__" + name + "_F\n    set si__" + name + "_F=this\nendfunction\n";
    return text.Take();
}

// 根结构在生成区与初始化函数中的顺序，与 jasshelper 的可观察结果一致（按大量样例归纳，未参考其源码）：
// 全部结构按类型编号排成表；最后一个“父结构不是表中第一项”的子结构与第二项交换；
// 然后第一项在前，其余倒序；最后只保留根结构。
std::vector<std::int32_t> Translation::RootListOrder() const {
    std::vector<std::int32_t> list = structOrder_;
    std::size_t last = 0;
    for (std::size_t i = 1; i < list.size(); ++i) {
        budget_.Poll();
        const auto parent = program_.structs[static_cast<std::size_t>(list[i])].parent;
        if (parent >= 0 && parent != list.front()) last = i;
    }
    if (last > 1) std::swap(list[1], list[last]);
    if (list.size() > 2) std::reverse(list.begin() + 1, list.end());
    std::erase_if(list, [this](std::int32_t index) { return program_.structs[static_cast<std::size_t>(index)].parent >= 0; });
    return list;
}

void Translation::EmitStructFunctions(MappedOutput& out) {
    for (const auto index : RootListOrder()) {
        budget_.Poll();
        const auto& data = program_.structs[static_cast<std::size_t>(index)];
        out.AnchorLine(data.line);
        const auto& name = data.fullName;
        const auto id = std::to_string(data.typeId);
        const bool children = HasChildren(index);
        if (data.onDestroy >= 0) {
            // onDestroy 的调用器：函数体只用到游戏脚本时直接复制（生成区在地图函数之前），否则经触发器调用。
            out += "\n//Generated method caller for " + name + ".onDestroy\nfunction sc__" + name + "_onDestroy takes integer this returns nothing\n";
            if (destroyCallsMap_[data.onDestroy]) {
                out += "    set f__arg_this=this\n    call TriggerEvaluate(st__" + name + "_onDestroy" + (children ? "[" + id + "]" : std::string{}) + ")\n";
            } else {
                out += destroyBodies_[data.onDestroy];
            }
            out += "endfunction\n";
        }
        out += "\n//Generated allocator of " + name + "\nfunction s__" + name + "__allocate takes nothing returns integer\n local integer this=si__" + name +
            "_F\n    if (this!=0) then\n        set si__" + name + "_F=si__" + name + "_V[this]\n    else\n        set si__" + name + "_I=si__" + name +
            "_I+1\n        set this=si__" + name + "_I\n    endif\n    if (this>8190) then\n        return 0\n    endif\n\n";
        out += MemberInits(index);
        if (children) out += "    set si__" + name + "_type[this]=" + id + "\n";
        out += "    set si__" + name + "_V[this]=-1\n return this\nendfunction\n\n";
        out += Destructor(index, false);
        for (const auto child : data.children) {
            budget_.Poll();
            const auto& childData = program_.structs[static_cast<std::size_t>(child)];
            out.AnchorLine(childData.line);
            out += "\n//Generated allocator of " + childData.fullName + "\nfunction s__" + childData.fullName +
                "__allocate takes nothing returns integer\n local integer this=s__" + name + "__allocate()\n local integer kthis\n"
                "    if(this==0) then\n        return 0\n    endif\n    set si__" + name + "_type[this]=" + std::to_string(childData.typeId) +
                "\n    set kthis=this\n\n";
            out += MemberInits(child);
            out.AnchorLine(childData.line);
            out += " return this\nendfunction\n\n";
        }
    }
    out += "\n";
}

void Translation::EmitStruct(MappedOutput& out, std::int32_t structIndex) {
    const auto& data = program_.structs[static_cast<std::size_t>(structIndex)];
    for (const auto method : data.methods) {
        budget_.Poll();
        EmitFunction(out, method);
        if (method == data.onDestroy && data.parent < 0 && !HasChildren(structIndex)) {
            out += "\n";
            out += Destructor(structIndex, true);
        }
    }
}

void Translation::EmitStructInitializers(MappedOutput& out) {
    // 需要析构触发器的根结构（有 onDestroy 或有子结构），按结构表顺序。
    std::vector<std::int32_t> roots;
    for (const auto index : RootListOrder()) {
        budget_.Poll();
        if (program_.structs[static_cast<std::size_t>(index)].onDestroy >= 0 || HasChildren(index)) roots.push_back(index);
    }
    out += "\n\n\n//Struct method generated initializers/callers:\n";
    for (const auto index : roots) {
        budget_.Poll();
        const auto& data = program_.structs[static_cast<std::size_t>(index)];
        if (data.onDestroy < 0) continue;
        out.AnchorLine(program_.functions[static_cast<std::size_t>(data.onDestroy)].decl.line);
        out += "function sa__" + data.fullName + "_onDestroy takes nothing returns boolean\nlocal integer this=f__arg_this\n";
        out += destroyBodies_[data.onDestroy];
        out.AnchorLine(data.line);
        out += "   return true\nendfunction\n";
    }
    out += "\nfunction " + initName_ + " takes nothing returns nothing\n";
    for (const auto index : roots) {
        budget_.Poll();
        const auto& data = program_.structs[static_cast<std::size_t>(index)];
        const auto trigger = "st__" + data.fullName + "_onDestroy";
        const auto condition = "Condition( function sa__" + data.fullName + "_onDestroy))\n";
        if (HasChildren(index)) {
            const auto root = trigger + "[" + std::to_string(data.typeId) + "]";
            if (data.onDestroy >= 0) {
                out += "    set " + root + "=CreateTrigger()\n";
                for (const auto child : data.children) {
                    budget_.Poll();
                    out += "    set " + trigger + "[" + std::to_string(program_.structs[static_cast<std::size_t>(child)].typeId) + "]=" + root + "\n";
                }
                out += "    call TriggerAddCondition(" + root + "," + condition;
            } else {
                out += "    set " + root + "=null\n";
                for (const auto child : data.children) {
                    budget_.Poll();
                    out += "    set " + trigger + "[" + std::to_string(program_.structs[static_cast<std::size_t>(child)].typeId) + "]=null\n";
                }
            }
        } else if (data.onDestroy >= 0) {
            out += "    set " + trigger + "=CreateTrigger()\n    call TriggerAddCondition(" + trigger + "," + condition;
        }
    }
    out += "endfunction\n";
}

} // namespace War3Map::Jass::detail
