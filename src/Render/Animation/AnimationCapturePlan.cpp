#include "War3MapCore/Render/ModelAnimationCapture.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <set>
#include <locale>
#include <span>
#include <sstream>
#include <unordered_map>

namespace War3Map::Render {
namespace {
template<class T> bool Number(std::string_view text,T& output) {
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),output);
    return error==std::errc{} && end==text.data()+text.size();
}
std::vector<std::string_view> Columns(std::string_view row) {
    std::vector<std::string_view> result;
    while (true) {
        const auto tab=row.find('\t'); result.push_back(row.substr(0,tab));
        if (tab==std::string_view::npos) return result;
        row.remove_prefix(tab+1);
    }
}
bool Label(std::string_view label) {
    return !label.empty() && label.size()<=64 && std::all_of(label.begin(),label.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';
    });
}
}
Result<AnimationCapturePlan> ParseAnimationCapturePlan(std::string_view text) {
    using Output=Result<AnimationCapturePlan>;
    if (text.size()>1024*1024 || text.find('\0')!=std::string_view::npos)
        return Output::Failure("Render.Animation","ANIMATION_PLAN_BUDGET","清单超出 1 MiB 或包含零字节。");
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    AnimationCapturePlan plan; std::vector<AnimationEdit> edits;
    std::set<std::uint64_t> ids,edited; std::set<std::string> labels; std::size_t line=0; bool started=false,particlesSet=false;
    const auto fail=[&](const char* why) { return Output::Failure("Render.Animation","ANIMATION_PLAN_INVALID",
        "清单第 "+std::to_string(line)+" 行："+why); };
    while (!text.empty()) {
        ++line; const auto end=text.find('\n'); auto row=text.substr(0,end);
        text=end==std::string_view::npos?std::string_view{}:text.substr(end+1);
        if (row.ends_with('\r')) row.remove_suffix(1);
        if (row.empty() || row.starts_with('#')) continue;
        const auto c=Columns(row);
        if (c[0]=="instance") {
            if (started || c.size()!=13 || plan.instances.size()>=256) return fail("instance 需要 12 个参数，且必须先于捕获命令。");
            AnimationInstanceSpec s; s.hostKey=std::string(c[1]); s.identity.generation=1;
            if (!Number(c[1],s.identity.value) || !s.identity.value || !ids.insert(s.identity.value).second) return fail("实例编号须非零且不重复。");
            s.model.modelPath=std::string(c[2]); s.action=std::string(c[3]); std::uint64_t ms=0;
            if (c[4]!="mid") { if (!Number(c[4],ms)) return fail("时间须整数毫秒或 mid。"); s.elapsedMs=ms; }
            for (std::size_t k=0;k<3;++k) if (!Number(c[5+k],s.model.position[k]) || !Number(c[9+k],s.model.scale[k])) return fail("位置/缩放无效。");
            if (!Number(c[8],s.model.angle) || !Number(c[12],s.model.teamColor) || s.model.teamColor>15 ||
                s.model.modelPath.empty() || s.model.modelPath.size()>512 || s.action.empty() || s.action.size()>80) return fail("模型、动作、朝向或队伍色无效。");
            plan.instances.push_back(std::move(s));
        } else if (c[0]=="particles") {
            if (started || particlesSet || c.size()!=2 || (c[1]!="true" && c[1]!="false"))
                return fail("particles 只能在捕获前给一次 true 或 false。");
            plan.particles=c[1]=="true"; particlesSet=true;
        } else if (c[0]=="camera") {
            if (started || plan.camera || c.size()!=7) return fail("camera 只能在开头给一次，共 6 个浮点数。");
            War3RenderCameraV2 camera{}; camera.structSize=sizeof(camera);
            float* fields[]{&camera.targetX,&camera.targetY,&camera.targetZ,&camera.distance,&camera.yawRadians,&camera.pitchRadians};
            for(std::size_t i=0;i<6;++i) if(!Number(c[i+1],*fields[i]) || !std::isfinite(*fields[i])) return fail("相机字段无效。");
            plan.camera=camera;
        } else if (c[0]=="time") {
            started=true; AnimationEdit edit; std::uint64_t id=0,ms=0;
            if ((c.size()!=3 && c.size()!=4) || !Number(c[1],id) || !ids.contains(id) || !Number(c[2],ms) || !edited.insert(id).second)
                return fail("time 需要已有实例编号及毫秒；同一捕获前不可重复编辑同一实例。");
            edit.identity={id,1}; edit.elapsedMs=ms;
            if(c.size()==4) { if(!Number(c[3],ms)) return fail("全局毫秒无效。"); edit.globalMs=ms; }
            edits.push_back(edit);
        } else if(c[0]=="capture") {
            started=true;
            if(c.size()!=2 || !Label(c[1]) || !labels.emplace(c[1]).second || plan.captures.size()>=16) return fail("capture 标签无效、重复或超过16帧。");
            plan.captures.push_back({std::string(c[1]),std::move(edits)}); edits.clear(); edited.clear();
        } else return fail("未知命令；仅支持 instance/camera/particles/time/capture。");
    }
    if(plan.instances.empty() || plan.captures.empty() || !edits.empty()) return fail("需要实例和捕获，最后的 time 不能没有 capture。");
    return Output::Success(std::move(plan));
}
std::string AnimationJsonString(std::string_view value) {
    static constexpr char digits[]="0123456789abcdef";
    std::string result="\"";
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') { result+='\\'; result+=static_cast<char>(c); }
        else if(c<32) { result+="\\u00"; result+=digits[c>>4]; result+=digits[c&15]; }
        else result+=static_cast<char>(c);
    }
    return result+'"';
}
std::string AnimationReceiptJson(const AnimatedModelScene& scene,const AnimationFrame& frame,
    const AnimationCaptureReceipt& actual,std::string_view label,std::string_view imageSha) {
    std::ostringstream out; out.imbue(std::locale::classic()); out.precision(9);
    const auto number=[&](std::uint64_t n){out<<'"'<<n<<'"';};
    out<<"{\"quality\":\"PreviewApproximate\",\"label\":"<<AnimationJsonString(label)<<",\"imageSha256\":"<<AnimationJsonString(imageSha);
    out<<",\"sceneVersion\":";number(actual.skin.sceneVersion);out<<",\"contentVersion\":";number(actual.skin.contentVersion);
    out<<",\"documentRevision\":";number(frame.documentRevision);out<<",\"viewRevision\":";number(actual.frame.capture.documentRevision);
    out<<",\"frameCount\":";number(actual.skin.frameCount);out<<",\"captureTicket\":";number(actual.frame.capture.ticket);
    out<<",\"width\":"<<actual.frame.capture.width<<",\"height\":"<<actual.frame.capture.height;
    out<<",\"surfaceEpoch\":";number(actual.frame.view.surfaceEpoch);out<<",\"viewEpoch\":";number(actual.frame.view.viewEpoch);
    out<<",\"renderSceneEpoch\":";number(actual.frame.view.sceneEpoch);
    out<<",\"staticUploadCount\":";number(actual.skin.staticUploadCount);
    out<<",\"matrixUploadCount\":";number(actual.skin.matrixUploadCount);
    out<<",\"particlesEnabled\":"<<(scene.particlesEnabled?"true":"false");
    out<<",\"premEnabled\":"<<(scene.premEnabled?"true":"false");
    out<<",\"dynamicMaterialStates\":"<<(scene.dynamicMaterialStates?"true":"false")
        <<",\"materialStateCount\":"<<frame.materialStates.size()<<",\"materialStateSliceCount\":"<<frame.materialStateSlices.size();
    out<<",\"particleInstanceCount\":"<<scene.particleInstanceCount;
    if (scene.dynamicMaterialAlphas) out<<",\"materialLayerAlphaFormat\":"<<WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1
        <<",\"materialLayerAlphaCount\":"<<frame.layerAlphas.size()<<",\"materialInstanceSliceCount\":"<<frame.layerSlices.size();
    if (actual.particles) {
        const auto& p=*actual.particles;
        out<<",\"particles\":{\"sceneVersion\":";number(p.sceneVersion);
        out<<",\"contentVersion\":";number(p.contentVersion);
        out<<",\"vertexCount\":"<<p.vertexCount<<",\"drawCount\":"<<p.drawCount;
        out<<",\"textureUploadCount\":";number(p.textureUploadCount);
        out<<",\"dynamicUploadCount\":";number(p.dynamicUploadCount);out<<'}';
    }
    const auto& c=actual.frame.camera;
    out<<",\"camera\":{\"target\":["<<c.targetX<<','<<c.targetY<<','<<c.targetZ<<"],\"distance\":"<<c.distance
        <<",\"yaw\":"<<c.yawRadians<<",\"pitch\":"<<c.pitchRadians<<"},\"instances\":[";
    bool comma=false;
    for(const auto& link:frame.poses) {
        if(link.requestIndex==kBakedInstance) continue;
        if (comma) out << ',';
        comma = true;
        const auto& state=frame.states[link.requestIndex]; const auto& spec=scene.specs[link.requestIndex];
        out<<"{\"id\":";number(link.identity.value);out<<",\"generation\":";number(link.identity.generation);
        out<<",\"requestIndex\":"<<link.requestIndex;
        out<<",\"hostKey\":"<<AnimationJsonString(spec.hostKey)<<",\"model\":"<<AnimationJsonString(spec.model.modelPath);
        out<<",\"animated\":"<<(link.animated?"true":"false")<<",\"sequenceIndex\":"<<state.sequenceIndex;
        out<<",\"hidden\":"<<(link.hidden?"true":"false");
        out<<",\"staticForBudget\":"<<(link.staticForBudget?"true":"false");
        if (link.staticForBudget) {
            out<<",\"displayedPose\":{\"sequenceIndex\":"<<link.staticSequenceIndex<<",\"elapsedMs\":";
            number(link.staticElapsedMs); out<<",\"globalMs\":"; number(link.staticGlobalMs); out<<'}';
        }
        out<<",\"partial\":"<<(link.partial?"true":"false")
            <<",\"noVisibleGeometry\":"<<(link.noVisibleGeometry?"true":"false")<<",\"geosetIssues\":[";
        for (std::size_t i=0; i<link.geosetIssues.count; ++i) {
            if (i) out<<',';
            const auto& issue=frame.geosetIssues.at(static_cast<std::size_t>(link.geosetIssues.offset)+i);
            const auto& diagnostic=issue.diagnostic;
            out<<"{\"geosetIndex\":"<<issue.geosetIndex
                <<",\"code\":"<<AnimationJsonString(War3RenderCore::CodeName(diagnostic.code))
                <<",\"message\":"<<AnimationJsonString(War3RenderCore::DescribeDiagnostic(diagnostic))
                <<",\"objectId\":"<<diagnostic.objectId<<",\"track\":"<<static_cast<std::uint32_t>(diagnostic.track)
                <<",\"leftFrame\":"<<diagnostic.leftFrame<<",\"rightFrame\":"<<diagnostic.rightFrame<<'}';
        }
        out<<']';
        if (link.hidden) {
            out<<",\"rejection\":{\"code\":"<<AnimationJsonString(link.rejectionCode.empty()?
                War3RenderCore::CodeName(link.rejection.code):link.rejectionCode);
            if (!link.rejectionCode.empty()) out<<",\"message\":"<<AnimationJsonString(link.rejectionMessage);
            else out<<",\"objectId\":"<<link.rejection.objectId<<",\"track\":"<<static_cast<std::uint32_t>(link.rejection.track)
                <<",\"leftFrame\":"<<link.rejection.leftFrame<<",\"rightFrame\":"<<link.rejection.rightFrame;
            out<<'}';
        }
        out<<",\"assetId\":";number(link.asset.value);out<<",\"assetGeneration\":";number(link.asset.generation);
        if(state.sequenceIndex<scene.sequences[link.requestIndex].size()) out<<",\"action\":"<<AnimationJsonString(scene.sequences[link.requestIndex][state.sequenceIndex].name);
        out<<",\"elapsedMs\":";number(state.elapsedMs);out<<",\"globalMs\":";number(state.globalMs);
        out<<",\"poseEpoch\":";number(link.evaluationEpoch);out<<",\"poseVersion\":";number(link.poseVersion);
        out<<",\"poseWarnings\":[";
        const auto warningView = ReadAnimationPoseWarnings(scene, frame, link);
        if (warningView.Succeeded()) {
            const auto& warnings = *warningView.value;
            for (std::size_t i = 0; i < warnings.records.size(); ++i) {
                if (i) out<<',';
                const auto& warning = warnings.records[i]; const auto& material = warnings.materialRecords[i];
                out<<"{\"code\":"<<AnimationJsonString(War3RenderCore::CodeName(warning.code))
                    <<",\"objectId\":"<<warning.objectId<<",\"leftFrame\":"<<warning.leftFrame<<",\"rightFrame\":"<<warning.rightFrame
                    <<",\"sequenceIndex\":"<<warnings.sequenceIndex<<",\"elapsedMs\":"; number(warnings.elapsedMs);
                out<<",\"globalMs\":"; number(warnings.globalMs);
                out<<",\"cachedStand\":"<<(warnings.cachedStand?"true":"false");
                if (material.materialIndex != War3RenderCore::NoIndex) {
                    out<<",\"materialIndex\":"<<material.materialIndex<<",\"layerIndex\":"<<material.layerIndex
                        <<",\"leftSourceByteOffset\":"; number(material.leftSourceByteOffset);
                    out<<",\"rightSourceByteOffset\":"; number(material.rightSourceByteOffset);
                    out<<",\"sourceValueBits\":"<<material.sourceValueBits<<",\"interpolation\":"<<material.interpolation;
                }
                out<<'}';
            }
        }
        out<<']';
        out<<",\"poseWarningReadError\":";
        if (warningView.Succeeded()) out<<"null";
        else out<<AnimationJsonString(warningView.diagnostics.empty()?"ANIMATION_WARNING_READ_FAILED":warningView.diagnostics.front().code);
        out<<",\"poseWarningsOmitted\":"; number(warningView.Succeeded()?warningView.value->omitted:link.warnings.omitted);
        out<<",\"matrixHashFNV1a\":";number(link.matrixHash);out<<",\"alphaHashFNV1a\":";number(link.alphaHash);
        out<<",\"colorHashFNV1a\":";number(link.colorHash);
        if (scene.dynamicMaterialAlphas) {
            out<<",\"materialAlphaHashFNV1a\":";number(link.materialAlphaHash);
            out<<",\"materialAlphaSlice\":{\"first\":"<<link.materialAlphas.offset<<",\"count\":"<<link.materialAlphas.count<<'}';
            out<<",\"materialIssues\":[";
            for (std::uint32_t i=0;i<link.materialIssues.count;++i) {
                if (i) out<<',';
                const auto& issue=frame.materialIssues.at(link.materialIssues.offset+i).source;
                out<<"{\"materialIndex\":"<<issue.materialIndex<<",\"layerIndex\":"<<issue.layerIndex<<",\"sourceByteOffset\":";
                number(issue.sourceByteOffset); out<<",\"code\":"<<AnimationJsonString(War3RenderCore::CodeName(issue.error))
                    <<",\"sourceFrame\":"<<issue.sourceFrame<<",\"sourceValueBits\":"<<issue.sourceValueBits<<'}';
            }
            out<<']';
        }
        if (scene.dynamicMaterialStates) {
            out<<",\"materialStateHashFNV1a\":"; number(link.materialStateHash);
            out<<",\"materialStateSlice\":{\"first\":"<<link.materialStates.offset<<",\"count\":"<<link.materialStates.count<<'}';
            out<<",\"materialStates\":[";
            for (std::uint32_t i=0;i<link.materialStates.count;++i) {
                if (i) out<<',';
                const auto& value=frame.materialStates.at(link.materialStates.offset+i);
                out<<"{\"residentTextureIndex\":"<<value.textureIndex<<",\"samplerFlags\":"<<value.samplerFlags<<",\"uvTransform\":[";
                for (std::uint32_t q=0;q<6;++q) { if (q) out<<','; out<<value.uvTransform[q]; }
                out<<"]}";
            }
            out<<']';
        }
        out<<",\"worldTransform\":[";
        for(std::size_t i=0;i<state.transform.size();++i){if(i)out<<',';out<<state.transform[i];}
        out<<"]}";
    }
    out<<"],\"particleInstances\":[";comma=false;
    for (const auto& link:frame.particleLinks) {
        if (comma) out<<',';
        comma=true;
        const auto& state=frame.states.at(link.requestIndex);
        const auto& spec=scene.specs.at(link.requestIndex);
        out<<"{\"id\":";number(link.identity.value);out<<",\"generation\":";number(link.identity.generation);
        out<<",\"requestIndex\":"<<link.requestIndex<<",\"assetId\":";number(link.asset.value);
        out<<",\"assetGeneration\":";number(link.asset.generation);
        out<<",\"hostKey\":"<<AnimationJsonString(spec.hostKey)<<",\"model\":"<<AnimationJsonString(spec.model.modelPath);
        if (link.requestIndex<scene.particleCapabilities.size()) {
            const auto& capability=scene.particleCapabilities[link.requestIndex];
            out<<",\"capability\":{\"particle2Count\":"<<capability.particle2Count<<",\"ribbonCount\":"<<capability.ribbonCount
                <<",\"premCount\":"<<capability.premCount<<",\"premPreparedCount\":"<<capability.premPreparedCount
                <<",\"available\":"<<(capability.available?"true":"false")
                <<",\"skippedEmitters\":[";
            bool issueComma=false;
            if (capability.skippedEmitters) for (const auto& issue:*capability.skippedEmitters) {
                if (issueComma) out<<',';
                issueComma=true;
                out<<"{\"kind\":"<<AnimationJsonString(issue.kind)<<",\"sourceIndex\":"<<issue.sourceIndex<<",\"nodeId\":";
                if (issue.nodeId) out<<*issue.nodeId; else out<<"null";
                out<<",\"code\":"<<AnimationJsonString(issue.code)<<",\"message\":"<<AnimationJsonString(issue.message)<<'}';
            }
            out<<"]}";
        }
        out<<",\"sequenceIndex\":"<<state.sequenceIndex;
        if(state.sequenceIndex<scene.sequences.at(link.requestIndex).size())
            out<<",\"action\":"<<AnimationJsonString(scene.sequences[link.requestIndex][state.sequenceIndex].name);
        out<<",\"elapsedMs\":";number(state.elapsedMs);out<<",\"globalMs\":";number(state.globalMs);
        out<<",\"simulationTimeMs\":";number(link.simulationTimeMs);
        out<<",\"simulated\":"<<(link.simulated?"true":"false")<<",\"hidden\":"<<(link.hidden?"true":"false");
        out<<",\"partial\":"<<(link.partial?"true":"false")<<",\"unsupportedPremCount\":"<<link.unsupportedPremCount;
        out<<",\"budgetLimited\":"<<(link.budgetLimited?"true":"false");
        if (!link.rejectionCode.empty()) {
            out<<",\"rejection\":{\"code\":"<<AnimationJsonString(link.rejectionCode)
                <<",\"message\":"<<AnimationJsonString(link.rejectionMessage);
            if (link.hidden) {
                out<<",\"elapsedMs\":";number(link.rejectedElapsedMs);out<<",\"globalMs\":";number(link.rejectedGlobalMs);
            }
            out<<'}';
        }
        out<<",\"particleCount\":"<<link.particleCount<<",\"ribbonSegmentCount\":"<<link.ribbonSegmentCount;
        out<<",\"firstVertex\":"<<link.firstVertex<<",\"vertexCount\":"<<link.vertexCount;
        out<<",\"firstDraw\":"<<link.firstDraw<<",\"drawCount\":"<<link.drawCount;
        out<<",\"spawned\":";number(link.spawned);out<<",\"suppressed\":";number(link.suppressed);
        out<<",\"stateHashFNV1a\":";number(link.stateHash);out<<",\"geometryHashFNV1a\":";number(link.geometryHash);
        out<<",\"resetReason\":"<<AnimationJsonString(link.resetReason)<<",\"worldTransform\":[";
        for(std::size_t i=0;i<state.transform.size();++i){if(i)out<<',';out<<state.transform[i];}
        out<<"]}";
    }
    out<<"],\"diagnostics\":[";comma=false;
    for(const auto& d:frame.diagnostics){if(comma)out<<',';comma=true;out<<AnimationJsonString(War3RenderCore::CodeName(d.code));}
    out<<"],\"omittedDiagnostics\":"<<frame.omittedDiagnostics;
    out<<",\"animatedInstances\":"<<frame.animatedInstances<<",\"retainedStaticInstances\":"<<frame.retainedStaticInstances;
    out<<",\"hiddenInstances\":"<<frame.hiddenInstances;
    out<<",\"budgetStaticInstances\":"<<frame.budgetStaticInstances<<",\"budgetParticleInstances\":"<<frame.budgetParticleInstances
        <<",\"sharedPoseRanges\":"<<(frame.sharedPoseRanges?"true":"false");
    out<<",\"skippedInstances\":"<<scene.geometry.skippedInstances<<",\"untexturedLayers\":"<<scene.geometry.untexturedLayers;
    out<<",\"unavailableInstances\":["; comma=false;
    for (const auto& rejected:scene.unavailableInstances) {
        if (comma) out<<',';
        comma=true;
        const auto& spec=scene.specs.at(rejected.requestIndex);
        out<<"{\"id\":";number(rejected.identity.value);out<<",\"generation\":";number(rejected.identity.generation);
        out<<",\"assetId\":";number(rejected.asset.value);out<<",\"assetGeneration\":";number(rejected.asset.generation);
        out<<",\"hostKey\":"<<AnimationJsonString(spec.hostKey)<<",\"model\":"<<AnimationJsonString(spec.model.modelPath)
            <<",\"code\":"<<AnimationJsonString(rejected.code)<<'}';
    }
    out<<']';
    out<<",\"sceneDiagnostics\":["; comma=false;
    for (const auto& d:scene.geometry.warnings) {
        if (comma) out<<',';
        comma=true; out<<"{\"code\":"<<AnimationJsonString(d.code)<<",\"message\":"<<AnimationJsonString(d.message);
        out<<",\"origin\":";
        if (d.origin) {
            const auto source=d.origin->source.generic_u8string();
            out<<"{\"source\":"<<AnimationJsonString({reinterpret_cast<const char*>(source.data()),source.size()});
            out<<",\"offset\":"; number(d.origin->offset);
            out<<",\"length\":"; number(d.origin->length);
            out<<",\"fingerprint\":"<<AnimationJsonString(d.origin->fingerprint)<<'}';
        } else out<<"null";
        out<<'}';
    }
    out<<"],\"sourceIdentity\":"<<AnimationJsonString(scene.sourceIdentity)<<",\"assets\":[";
    comma=false;
    for(const auto& a:scene.sources) {if(comma)out<<',';comma=true;out<<"{\"path\":"<<AnimationJsonString(a.virtualPath)
        <<",\"layer\":"<<AnimationJsonString(a.sourceLayer)<<",\"sha256\":"<<AnimationJsonString(a.contentDigest)<<'}';}
    out<<"]}"; return out.str();
}
Result<AnimationPoseWarningView> ReadAnimationPoseWarnings(const AnimatedModelScene& scene,
    const AnimationFrame& frame, const AnimationPoseLink& pose) {
    using Output = Result<AnimationPoseWarningView>;
    const auto& warnings = pose.warnings;
    if (!warnings.count && !warnings.omitted) return Output::Success({});
    const auto r = pose.requestIndex;
    if (warnings.count > warnings.records.size() || (warnings.omitted && warnings.count != warnings.records.size()) ||
        frame.sceneVersion != scene.sceneVersion || frame.documentRevision != scene.documentRevision || !frame.contentVersion ||
        r >= scene.specs.size() || r >= frame.states.size() || r >= scene.sequences.size() ||
        pose.renderedInstance >= frame.poses.size() || &frame.poses[pose.renderedInstance] != &pose ||
        pose.renderedInstance >= scene.geometry.instanceRequests.size() ||
        pose.renderedInstance >= scene.geometry.instances.size() ||
        scene.geometry.instanceRequests[pose.renderedInstance] != r ||
        pose.identity != scene.specs[r].identity || pose.identity != frame.states[r].identity ||
        warnings.requestIndex != r || warnings.identity != pose.identity || warnings.asset != pose.asset ||
        warnings.evaluationEpoch != pose.evaluationEpoch || warnings.poseVersion != pose.poseVersion ||
        !pose.asset.value || pose.asset.generation != scene.generation || !pose.evaluationEpoch || !pose.poseVersion ||
        pose.hidden || pose.outsideView || warnings.sequenceIndex >= scene.sequences[r].size())
        return Output::Failure("Render.Animation", "ANIMATION_POSE_WARNING_IDENTITY",
            "实例警告的所属来源、场景、请求或姿势版本不匹配，未使用错属记录。");
    if (warnings.cachedStand) {
        if (!pose.staticForBudget || pose.animated || pose.standPoseIndex == UINT32_MAX ||
            warnings.sequenceIndex != pose.staticSequenceIndex || warnings.elapsedMs != pose.staticElapsedMs ||
            warnings.globalMs != pose.staticGlobalMs)
            return Output::Failure("Render.Animation", "ANIMATION_POSE_WARNING_CLOCK",
                "缓存警告没有对应的实际缓存动作/时钟，未把请求时间冒充缓存样本。");
    } else if (!pose.animated || pose.staticForBudget || warnings.sequenceIndex != frame.states[r].sequenceIndex ||
        warnings.elapsedMs != frame.states[r].elapsedMs || warnings.globalMs != frame.states[r].globalMs) {
        return Output::Failure("Render.Animation", "ANIMATION_POSE_WARNING_CLOCK",
            "本次姿势警告的实际采样动作/时钟与所属帧不匹配。");
    }
    const auto records = std::span<const War3RenderCore::PoseDiagnostic>(warnings.records).first(warnings.count);
    if (std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.severity != War3RenderCore::Severity::Warning || record.code == War3RenderCore::Code::Ok;
    })) return Output::Failure("Render.Animation", "ANIMATION_POSE_WARNING_RANGE", "成功警告容器包含无效或错误级别的记录。");
    const auto materials = std::span<const War3RenderCore::MaterialAlphaPreviewWarning>(warnings.materialRecords).first(warnings.count);
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& source = materials[i];
        const bool material = records[i].code == War3RenderCore::Code::MaterialAlphaIdenticalDuplicatesPreview;
        if (material != (source.materialIndex != War3RenderCore::NoIndex) || (material &&
            (source.diagnosticIndex != i || source.layerIndex == War3RenderCore::NoIndex ||
             records[i].track != War3RenderCore::TrackKind::MaterialAlpha || records[i].objectId != War3RenderCore::NoIndex ||
             source.sourceFrame != records[i].leftFrame || source.sourceFrame != records[i].rightFrame ||
             source.rightSourceByteOffset <= source.leftSourceByteOffset)))
            return Output::Failure("Render.Animation", "ANIMATION_MATERIAL_WARNING_SOURCE", "材质兼容警告的同帧来源绑定不成立。");
        if (material) {
            const auto modelIndex = scene.geometry.instances[pose.renderedInstance].model;
            if (modelIndex >= scene.pickModels.size() || !scene.pickModels[modelIndex].source)
                return Output::Failure("Render.Animation", "ANIMATION_MATERIAL_WARNING_SOURCE", "材质警告没有同一场景的冻结模型来源。");
            const auto& model = *scene.pickModels[modelIndex].source;
            if (source.materialIndex >= model.materials.size() || source.layerIndex >= model.materials[source.materialIndex].layers.size())
                return Output::Failure("Render.Animation", "ANIMATION_MATERIAL_WARNING_SOURCE", "材质警告层超出冻结模型。");
            const auto& layer = model.materials[source.materialIndex].layers[source.layerIndex];
            const auto left = std::lower_bound(layer.alphaKeys.begin(), layer.alphaKeys.end(), source.leftSourceByteOffset,
                [](const auto& key, std::uint64_t offset) { return key.sourceByteOffset < offset; });
            if (left == layer.alphaKeys.end() || left + 1 == layer.alphaKeys.end() ||
                left->sourceByteOffset != source.leftSourceByteOffset || (left + 1)->sourceByteOffset != source.rightSourceByteOffset ||
                left->frame != source.sourceFrame || (left + 1)->frame != source.sourceFrame ||
                layer.alphaInterpolation != source.interpolation || layer.alphaGlobalSequence != UINT32_MAX ||
                std::bit_cast<std::uint32_t>(left->value) != source.sourceValueBits ||
                std::bit_cast<std::uint32_t>((left + 1)->value) != source.sourceValueBits ||
                (source.interpolation > 1 && (std::bit_cast<std::uint32_t>(left->inTangent) != source.inTangentBits ||
                    std::bit_cast<std::uint32_t>((left + 1)->inTangent) != source.inTangentBits ||
                    std::bit_cast<std::uint32_t>(left->outTangent) != source.outTangentBits ||
                    std::bit_cast<std::uint32_t>((left + 1)->outTangent) != source.outTangentBits)))
                return Output::Failure("Render.Animation", "ANIMATION_MATERIAL_WARNING_SOURCE", "材质警告原键位置或原位值与冻结模型不匹配。");
        }
    }
    return Output::Success({records, warnings.omitted, warnings.sequenceIndex,
        warnings.elapsedMs, warnings.globalMs, warnings.cachedStand, materials});
}

Result<std::shared_ptr<AnimationDiagnosticQuery>> AnimationDiagnosticQuery::Create(
    std::shared_ptr<const AnimatedModelScene> scene) {
    using Output = Result<std::shared_ptr<AnimationDiagnosticQuery>>;
    if (!scene || !scene->sceneVersion || !scene->generation || scene->specs.size() >= UINT32_MAX ||
        scene->sources.size() > 16384 || scene->geometry.instanceRequests.size() >= UINT32_MAX ||
        scene->geometry.instanceRequests.size() != scene->geometry.instances.size() ||
        scene->particleCapabilities.size() != scene->specs.size() || scene->unavailableInstances.size() > scene->specs.size())
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_SCENE", "诊断索引需要有界的既有动画场景。");
    auto query = std::shared_ptr<AnimationDiagnosticQuery>(new AnimationDiagnosticQuery);
    query->scene_ = std::move(scene); query->indices_.resize(query->scene_->specs.size());
    const auto& s = *query->scene_;
    for (std::uint32_t i = 0; i < s.geometry.instanceRequests.size(); ++i) {
        const auto request = s.geometry.instanceRequests[i];
        if (request == kBakedInstance) continue;
        if (request >= query->indices_.size() || query->indices_[request].pose != UINT32_MAX)
            return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_IDENTITY", "几何请求索引不唯一或超出场景。");
        query->indices_[request].pose = i;
    }
    for (std::uint32_t i = 0; i < s.unavailableInstances.size(); ++i) {
        const auto& item = s.unavailableInstances[i];
        if (item.requestIndex >= s.specs.size() || item.identity != s.specs[item.requestIndex].identity ||
            query->indices_[item.requestIndex].unavailable != UINT32_MAX)
            return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_IDENTITY", "不可用模型记录不属于唯一原始请求。");
        query->indices_[item.requestIndex].unavailable = i;
    }
    // sources 同时含贴图与模型，不能以 asset.value 猜数组序号。模型来源保留服务实际读取的组合层身份。
    std::unordered_map<std::string, std::uint32_t> sources;
    const auto modelLayerPrefix = s.sourceIdentity + "|";
    for (std::uint32_t i = 0; i < s.sources.size(); ++i) {
        const auto& source = s.sources[i];
        if (!source.sourceLayer.starts_with(modelLayerPrefix)) continue;
        const auto [it, inserted] = sources.emplace(AnimationPathKey(source.virtualPath), i);
        if (!inserted && (it->second == UINT32_MAX || s.sources[it->second] != source)) it->second = UINT32_MAX;
    }
    for (std::uint32_t r = 0; r < s.specs.size(); ++r) {
        const auto source = sources.find(AnimationPathKey(s.specs[r].model.modelPath));
        if (source != sources.end()) query->indices_[r].source = source->second;
    }
    return Output::Success(std::move(query));
}

Status AnimationDiagnosticQuery::SetFrame(std::shared_ptr<const AnimationFrame> acceptedFrame) {
    frame_.reset();
    if (!acceptedFrame) return Status::Ok();
    // 沿用 scene/document 的服务所有者绑定；这里只校对查询形状，不替宿主验证或接受 GPU 帧。
    if (acceptedFrame->sceneVersion != scene_->sceneVersion || acceptedFrame->documentRevision != scene_->documentRevision ||
        !acceptedFrame->contentVersion || acceptedFrame->states.size() != scene_->specs.size() ||
        acceptedFrame->poses.size() != scene_->geometry.instances.size() ||
        acceptedFrame->particleLinks.size() > scene_->specs.size())
        return Status::Failure("Render.Animation", "ANIMATION_FRAME_STALE", "查询帧不属于当前场景或文档。");
    frame_ = std::move(acceptedFrame); return Status::Ok();
}

Result<AnimationDiagnosticBinding> AnimationDiagnosticQuery::Binding(std::uint32_t requestIndex,
    War3RenderCore::InstanceIdentity expectedIdentity) const {
    using Output = Result<AnimationDiagnosticBinding>;
    if (requestIndex >= scene_->specs.size() || scene_->specs[requestIndex].identity != expectedIdentity)
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_IDENTITY", "请求索引和实例身份不匹配。");
    AnimationDiagnosticBinding binding{scene_->sceneVersion, scene_->generation, scene_->documentRevision,
        frame_ ? frame_->contentVersion : 0, 0, 0, requestIndex, expectedIdentity};
    if (!frame_) return Output::Success(binding);
    if (frame_->states[requestIndex].identity != expectedIdentity)
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_IDENTITY", "实际帧状态不属于该实例。");
    const auto pose = indices_[requestIndex].pose;
    if (pose != UINT32_MAX) {
        const auto& link = frame_->poses[pose];
        if (link.requestIndex != requestIndex || link.identity != expectedIdentity)
            return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_IDENTITY", "实际姿势回执不属于该实例。");
        binding.evaluationEpoch = link.evaluationEpoch; binding.poseVersion = link.poseVersion;
    }
    // 服务 particleLinks 已按原始 requestIndex 排序；查询不扫描全场或另建每帧文字缓存。
    const auto particle = std::lower_bound(frame_->particleLinks.begin(), frame_->particleLinks.end(), requestIndex,
        [](const auto& link, auto request) { return link.requestIndex < request; });
    if (particle != frame_->particleLinks.end() && particle->requestIndex == requestIndex && particle->identity != expectedIdentity)
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_IDENTITY", "实际粒子回执不属于该实例。");
    return Output::Success(binding);
}

Result<std::optional<War3RenderCore::AssetSource>> AnimationDiagnosticQuery::Source(std::uint32_t requestIndex,
    War3RenderCore::InstanceIdentity expectedIdentity) const {
    using Output = Result<std::optional<War3RenderCore::AssetSource>>;
    if (requestIndex >= scene_->specs.size() || scene_->specs[requestIndex].identity != expectedIdentity)
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_IDENTITY", "请求索引和实例身份不匹配。");
    const auto source = indices_[requestIndex].source;
    if (source == UINT32_MAX) return Output::Success(std::nullopt);
    return Output::Success(scene_->sources[source]);
}

Result<AnimationDiagnosticPage> AnimationDiagnosticQuery::Query(const AnimationDiagnosticBinding& expected,
    std::uint32_t offset, std::uint32_t limit) const {
    using Output = Result<AnimationDiagnosticPage>;
    if (!limit || limit > 64)
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_BUDGET", "诊断页条数必须为 1..64。");
    const auto actual = Binding(expected.requestIndex, expected.identity);
    if (!actual.Succeeded()) return {std::nullopt, actual.diagnostics};
    if (*actual.value != expected)
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_STALE", "诊断绑定的场景、内容或姿势版本已变化。");
    const auto r = expected.requestIndex; const auto& index = indices_[r];
    const auto& spec = scene_->specs[r]; const auto& capability = scene_->particleCapabilities[r];
    AnimationDiagnosticPage page; page.binding = expected; page.modelPath = spec.model.modelPath; page.hostKey = spec.hostKey;
    page.hasFrame = static_cast<bool>(frame_); page.unavailable = index.unavailable != UINT32_MAX;
    page.particlesAvailable = capability.available;
    page.premDeclaredCount = capability.premCount; page.premPreparedCount = capability.premPreparedCount;
    page.partial = capability.premCount != 0 || (capability.skippedEmitters && !capability.skippedEmitters->empty());
    if (index.source != UINT32_MAX) page.source = scene_->sources[index.source];
    std::vector<AnimationDiagnosticIssue> base;
    const auto add = [&](std::string stage, std::string code, std::string message) {
        base.push_back({std::move(stage), {}, std::move(code), std::move(message), {}, {}, {}});
    };
    if (!page.hasFrame) add("frame", "ANIMATION_FRAME_UNAVAILABLE", "尚无该场景的已接受动画帧。");
    if (capability.premPreparedCount) add("prem-preview", "PREM_CHILD_STAND_FROZEN_PREVIEW",
        "已准入 " + std::to_string(capability.premPreparedCount) + "/" + std::to_string(capability.premCount) +
        " 个 PREM 基础子模型；真实 Stand0ms 冻结近似，实际发射与GPU完成须看独立PREM回执。");
    if (page.unavailable) {
        const auto& item = scene_->unavailableInstances[index.unavailable]; page.asset = item.asset;
        add("model", item.code, {}); // 现有记录只有原始 code，不能捏造丢失的错误详情。
    }
    if (!page.source) add("source", "ANIMATION_SOURCE_UNAVAILABLE", "没有唯一的实际模型读取来源记录。");
    const AnimationParticleLink* particle = nullptr;
    std::span<const AnimationGeosetIssue> geosetIssues;
    if (frame_) {
        const auto& state = frame_->states[r]; page.elapsedMs = state.elapsedMs; page.globalMs = state.globalMs;
        if (index.pose != UINT32_MAX) {
            const auto& pose = frame_->poses[index.pose]; page.asset = pose.asset;
            const auto warningView = ReadAnimationPoseWarnings(*scene_, *frame_, pose);
            if (!warningView.Succeeded()) return {std::nullopt, warningView.diagnostics};
            const auto& warnings = *warningView.value;
            if (warnings.omitted) add("pose-warning", "ANIMATION_POSE_WARNINGS_OMITTED",
                "本实例另有 " + std::to_string(warnings.omitted) +
                " 条成功姿势警告超过固定容量8，未保存或展开；原序预览警告优先保留。");
            for (std::size_t warningIndex = 0; warningIndex < warnings.records.size(); ++warningIndex) {
                const auto& warning = warnings.records[warningIndex];
                add("pose-warning", War3RenderCore::CodeName(warning.code), War3RenderCore::DescribeDiagnostic(warning) +
                    (warnings.cachedStand ? " [实际缓存样本" : " [本次实际样本") +
                    " SEQS=" + std::to_string(warnings.sequenceIndex) + " elapsed/global ms=" +
                    std::to_string(warnings.elapsedMs) + "/" + std::to_string(warnings.globalMs) + "]");
                if (warning.objectId != War3RenderCore::NoIndex) base.back().nodeId = warning.objectId;
                const auto& material = warnings.materialRecords[warningIndex];
                if (material.materialIndex != War3RenderCore::NoIndex) {
                    base.back().materialIndex = material.materialIndex;
                    base.back().layerIndex = material.layerIndex;
                    base.back().sourceByteOffset = material.leftSourceByteOffset;
                    base.back().message += " [KMTA material/layer=" + std::to_string(material.materialIndex) + "/" +
                        std::to_string(material.layerIndex) + " source bytes=" + std::to_string(material.leftSourceByteOffset) +
                        "/" + std::to_string(material.rightSourceByteOffset) + " value bits=" + std::to_string(material.sourceValueBits) + "]";
                }
            }
            page.animated = pose.animated; page.hidden = pose.hidden;
            page.partial |= pose.partial; page.noVisibleGeometry = pose.noVisibleGeometry;
            page.staticForBudget=pose.staticForBudget;
            if (pose.staticForBudget) {
                if (pose.staticSequenceIndex>=scene_->sequences[r].size())
                    return Output::Failure("Render.Animation","ANIMATION_DIAGNOSTIC_RANGE","缓存姿势动作没有实际 SEQS 来源。");
                page.displayedSequenceIndex=pose.staticSequenceIndex;
                page.displayedElapsedMs=pose.staticElapsedMs; page.displayedGlobalMs=pose.staticGlobalMs;
            }
            if (pose.geosetIssues.offset > frame_->geosetIssues.size() ||
                pose.geosetIssues.count > frame_->geosetIssues.size() - pose.geosetIssues.offset)
                return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_RANGE", "本帧网格诊断范围超出所属帧。");
            geosetIssues = std::span<const AnimationGeosetIssue>(frame_->geosetIssues)
                .subspan(pose.geosetIssues.offset, pose.geosetIssues.count);
            if (pose.hidden) {
                add("pose", pose.rejectionCode.empty() ? War3RenderCore::CodeName(pose.rejection.code) : pose.rejectionCode,
                    pose.rejectionCode.empty() ? War3RenderCore::DescribeDiagnostic(pose.rejection) : pose.rejectionMessage);
                if (pose.rejectionCode.empty() && pose.rejection.objectId != War3RenderCore::NoIndex)
                    base.back().nodeId = pose.rejection.objectId;
            } else if (pose.staticForBudget) {
                page.partial = true;
                add("pose", "ANIMATION_INSTANCE_STATIC_FOR_BUDGET", "本实例保留实际缓存动作 " +
                    scene_->sequences[r][pose.staticSequenceIndex].name + "；动态姿势预算用于摄像机附近实例。");
            } else if (!pose.animated) {
                page.partial = true;
                add("pose", "ANIMATION_STATIC_REASON_UNAVAILABLE", "该帧保留静态几何；场景未记录逐实例的原始编译拒绝详情。");
            }
        }
        const auto found = std::lower_bound(frame_->particleLinks.begin(), frame_->particleLinks.end(), r,
            [](const auto& link, auto request) { return link.requestIndex < request; });
        if (found != frame_->particleLinks.end() && found->requestIndex == r) particle = &*found;
    }
    if (particle) {
        if (!page.asset.value) page.asset = particle->asset;
        page.particlesSimulated = particle->simulated; page.hidden |= particle->hidden; page.partial |= particle->partial;
        if (!particle->rejectionCode.empty()) add("particle", particle->rejectionCode, particle->rejectionMessage);
    } else if (!capability.rejectionCode.empty()) add("particle", capability.rejectionCode, {});
    const auto skipped = capability.skippedEmitters ? capability.skippedEmitters->size() : 0;
    if (geosetIssues.size() > UINT32_MAX - base.size() || skipped > UINT32_MAX - base.size() - geosetIssues.size())
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_BUDGET", "诊断总数无法有界编码。");
    const auto geosetEnd = base.size() + geosetIssues.size();
    page.total = static_cast<std::uint32_t>(geosetEnd + skipped); page.offset = offset;
    if (offset > page.total)
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_RANGE", "诊断页起点超出当前实例诊断总数。");
    const auto end = offset + std::min(limit, page.total - offset);
    page.issues.reserve(end - offset);
    for (auto i = offset; i < end; ++i) {
        if (i < base.size()) page.issues.push_back(std::move(base[i]));
        else if (i < geosetEnd) {
            const auto& issue = geosetIssues[i - base.size()];
            page.issues.push_back({"geoset", "GEOA", War3RenderCore::CodeName(issue.diagnostic.code),
                War3RenderCore::DescribeDiagnostic(issue.diagnostic), {}, {}, issue.geosetIndex});
        }
        else {
            const auto& issue = (*capability.skippedEmitters)[i - geosetEnd];
            page.issues.push_back({"skipped-emitter", issue.kind, issue.code, issue.message, issue.sourceIndex, issue.nodeId, {}});
        }
    }
    page.truncated = end < page.total;
    // 保留原始文字而非偷偷裁切。异常宿主输入超过单页 256 KiB 时明确拒绝。
    std::size_t bytes = page.modelPath.size() + page.hostKey.size();
    if (page.source) bytes += page.source->virtualPath.size() + page.source->sourceLayer.size() + page.source->contentDigest.size();
    for (const auto& issue : page.issues) bytes += issue.stage.size() + issue.kind.size() + issue.code.size() + issue.message.size();
    if (bytes > 256 * 1024)
        return Output::Failure("Render.Animation", "ANIMATION_DIAGNOSTIC_BUDGET", "当前诊断页原始文字超过 256 KiB。");
    return Output::Success(std::move(page));
}
} // namespace War3Map::Render
