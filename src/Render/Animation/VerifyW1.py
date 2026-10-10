#!/usr/bin/env python3
"""检查同一次 war3-model-animation 的三帧回执；不替代人工识别真实 Footman。"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path
from typing import Any


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def integer(value: Any) -> int:
    require(isinstance(value, (int, str)) and not isinstance(value, bool), "版本/计数必须为整数或整数字符串")
    text = str(value)
    require(text.isascii() and text.isdecimal() and len(text) <= 20, "无效整数")
    result = int(text)
    require(result <= 2**64 - 1, "整数超过 uint64")
    return result


def read_json(path: Path) -> dict[str, Any]:
    require(path.stat().st_size <= 16 * 1024 * 1024, "回执超过 16 MiB")
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    require(isinstance(data, dict), "回执不是对象")
    return data


def verify(report: Path) -> None:
    data = read_json(report)
    require(data.get("quality") == "PreviewApproximate", "质量标记不是预览近似")
    frames = data.get("captures")
    require(isinstance(frames, list) and len(frames) == 3, "W1 必须在同一报告中有三帧")
    expected = ["initial", "walk_plus33", "repeat"]
    actors: list[dict[int, Any]] = []
    for frame, label in zip(frames, expected):
        require(frame.get("label") == label and frame.get("quality") == "PreviewApproximate", "帧标签/质量不匹配")
        require(read_json(report.parent / f"{label}.json") == frame, "单帧回执与总报告不一致")
        image = report.parent / f"{label}.bmp"
        require(54 < image.stat().st_size <= 17 * 1024 * 1024, "BMP 大小无效")
        image_bytes = image.read_bytes()
        require(image_bytes[:2] == b"BM" and struct.unpack_from("<I", image_bytes, 2)[0] == len(image_bytes), "图像不是完整 BMP")
        width, height, planes, bits = struct.unpack_from("<iiHH", image_bytes, 18)
        require(struct.unpack_from("<I", image_bytes, 10)[0] == 54 and struct.unpack_from("<I", image_bytes, 14)[0] == 40
                and width > 0 and height < 0 and planes == 1 and bits == 32
                and len(image_bytes) == 54 + width * (-height) * 4, "不是此命令的32位顶向下 BMP")
        require(width == integer(frame["width"]) and -height == integer(frame["height"]), "BMP 尺寸与回执不符")
        require(hashlib.sha256(image_bytes).hexdigest() == frame.get("imageSha256"), "图像 SHA 与同帧回执不符")
        require(integer(frame["documentRevision"]) == integer(frame["viewRevision"]), "文档/视图修订不符")
        for name in ("sceneVersion", "contentVersion", "frameCount", "captureTicket", "surfaceEpoch", "viewEpoch", "renderSceneEpoch", "matrixUploadCount"):
            require(integer(frame[name]) > 0, f"{name} 为零")
        require(integer(frame["staticUploadCount"]) == 1, "本 renderer 静态上传不是一次")
        require(integer(frame["skippedInstances"]) == 0 and integer(frame["retainedStaticInstances"]) == 0,
                "有跳过或只保留静态的实例")
        require(integer(frame["untexturedLayers"]) == 0, "真实 Footman 有缺贴图层，需复核后再验收")
        instances = frame.get("instances")
        require(isinstance(instances, list) and len(instances) == 2, "需要两个实例")
        by_id = {integer(item["id"]): item for item in instances}
        require(set(by_id) == {1, 2}, "W1 实例身份必须为 1、2")
        for item in by_id.values():
            require(item.get("animated") is True, "实例没有生成动画")
            require(integer(item["poseEpoch"]) > 0 and integer(item["poseVersion"]) > 0, "姿势身份无效")
        require(by_id[1]["action"].lower().startswith("stand") and by_id[2]["action"].lower().startswith("walk"), "动作不匹配")
        require(by_id[1]["model"] == by_id[2]["model"], "两实例未共享同一模型路径")
        actors.append(by_id)
    first, second, repeat = frames
    for key in ("sceneVersion", "camera", "documentRevision", "sourceIdentity", "assets", "surfaceEpoch", "viewEpoch", "width", "height"):
        require(first[key] == second[key] == repeat[key], f"{key} 在姿势更新中改变")
    require(first["imageSha256"] != second["imageSha256"], "Walk +33ms 未改变图像；不得改用默认图片冒充")
    require(second["imageSha256"] == repeat["imageSha256"], "同时间重复图像不同")
    for index in (1, 2):
        for key in ("contentVersion", "frameCount", "captureTicket", "matrixUploadCount"):
            require(integer(frames[index][key]) > integer(frames[index - 1][key]), f"{key} 没有递增")
        for actor in (1, 2):
            require(integer(actors[index][actor]["poseVersion"]) > integer(actors[index - 1][actor]["poseVersion"]), "姿势版本未递增")
            for key in ("generation", "poseEpoch", "worldTransform", "sequenceIndex", "globalMs"):
                require(actors[index][actor][key] == actors[0][actor][key], f"实例 {actor} 的 {key} 意外改变")
    for key in ("matrixHashFNV1a", "alphaHashFNV1a", "elapsedMs"):
        require(actors[0][1][key] == actors[1][1][key] == actors[2][1][key], f"Stand 的 {key} 被 Walk 推进影响")
        require(actors[1][2][key] == actors[2][2][key], f"Walk 重复帧的 {key} 不同")
    require(integer(actors[1][2]["elapsedMs"]) == integer(actors[0][2]["elapsedMs"]) + 33, "Walk 没有精确推进33ms")
    require(actors[0][2]["matrixHashFNV1a"] != actors[1][2]["matrixHashFNV1a"], "Walk 矩阵未变化")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        verify(args.report)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"W1_RECEIPT_REJECTED: {error}")
        return 1
    print("PASS W1_RECEIPT: two_instances=1 independent=1 step33=1 deterministic=1 static_uploads=1 versions_bound=1")
    print("仍需人工查看真实 Footman、悬崖/装饰物和窗口操作；此检查不证明原版等价。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
