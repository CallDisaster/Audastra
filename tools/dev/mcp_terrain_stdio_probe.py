"""用导出的原创夹具验证真正的 MCP stdio；只创建独立证据目录。"""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inventory(root):
    return {p.relative_to(root).as_posix(): sha(p)
            for p in sorted(root.rglob("*")) if p.is_file()}


def require(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--fixture-exporter", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qt-bin", required=True, type=Path)
    args = parser.parse_args()
    server = args.server.resolve(strict=True)
    exporter = args.fixture_exporter.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env["PATH"] = str(args.qt_bin.resolve(strict=True)) + os.pathsep + env.get("PATH", "")
    flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0
    receipt = {"schema": "forge-mcp-terrain-stdio-probe-v1", "passed": False,
               "synthetic_fixture": True, "server": str(server), "server_sha256": sha(server),
               "exporter_sha256": sha(exporter), "calls": []}
    process = None
    try:
        fixture = subprocess.run([str(exporter), "--write-fixture", str(output / "fixture")],
                                 env=env, creationflags=flags, timeout=30, capture_output=True, check=True)
        (output / "fixture-export.stdout").write_bytes(fixture.stdout)
        (output / "fixture-export.stderr").write_bytes(fixture.stderr)
        paths = json.loads(fixture.stdout)
        workspace, ui = Path(paths["workspace"]), Path(paths["ui_data"])
        before = inventory(output / "fixture")
        receipt["source_before"] = before
        with (output / "stderr.log").open("wb") as stderr, (output / "transcript.jsonl").open("w", encoding="utf-8") as log:
            process = subprocess.Popen([str(server), "--workspace", str(workspace), "--ui-data", str(ui)],
                                       env=env, creationflags=flags, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=stderr)
            receipt["pid"] = process.pid
            responses = queue.Queue()

            def reader():
                try:
                    while line := process.stdout.readline(2 * 1024 * 1024 + 1):
                        require(len(line) <= 2 * 1024 * 1024 and line.endswith(b"\n"), "stdio response exceeds line budget")
                        responses.put(json.loads(line))
                    responses.put(RuntimeError("stdio closed before expected response"))
                except Exception as error:
                    responses.put(error)

            threading.Thread(target=reader, daemon=True).start()
            sequence = 0

            def request(method, params):
                nonlocal sequence
                sequence += 1
                message = {"jsonrpc": "2.0", "id": sequence, "method": method, "params": params}
                log.write(json.dumps({"request": message}, ensure_ascii=False) + "\n")
                log.flush()
                process.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
                process.stdin.flush()
                response = responses.get(timeout=30)
                if isinstance(response, Exception):
                    raise response
                log.write(json.dumps({"response": response}, ensure_ascii=False) + "\n")
                log.flush()
                require(response.get("id") == sequence and "error" not in response, "JSON-RPC response failed")
                receipt["calls"].append({"id": sequence, "method": method, "name": params.get("name")})
                return response["result"]

            def tool(name, arguments, expected_error=None):
                result = request("tools/call", {"name": name, "arguments": arguments})
                data = json.loads(result["content"][0]["text"])
                if expected_error:
                    require(result.get("isError") is True and data.get("ok") is False, "expected tool rejection")
                    require(expected_error in [d["code"] for d in data["diagnostics"]], "wrong rejection code")
                    require("vertices" not in data and "cells" not in data, "rejection leaked partial block")
                else:
                    require(not result.get("isError"), "tool failed")
                return data

            request("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                    "clientInfo": {"name": "forge-terrain-probe", "version": "1"}})
            tools = request("tools/list", {})
            names = {t["name"] for t in tools["tools"]}
            require({"get_terrain_snapshot", "query_terrain_block"} <= names, "terrain tools not advertised")
            revision = tool("get_revision", {})["revision"]
            snapshot = tool("get_terrain_snapshot", {"expected_revision": revision})
            query = {"expected_revision": revision, "expected_terrain_binding": snapshot["terrain_binding"],
                     "tiles": {"x": 1, "y": 1, "width": 2, "height": 2}}
            block = tool("query_terrain_block", query)
            vertices, cells = block["vertices"], block["cells"]
            require(len(vertices) == 9 and len(cells) == 4, "wrong rectangle dimensions")
            require(vertices[0]["source_index"] == 7 and vertices[0]["position_world"] == [-64.25, 192.5, 3], "wrong world coordinates")
            require(vertices[4]["unknown_water_flags"] == 32768, "unknown water bits lost")
            require(cells[0]["source_vertices_sw_se_nw_ne"] == [7, 8, 13, 14], "wrong global corners")
            require(cells[0]["local_vertices_sw_se_nw_ne"] == [0, 1, 3, 4], "wrong local corners")
            stale = copy.deepcopy(query)
            stale["expected_terrain_binding"]["revision"] = "9007199254740993"
            tool("query_terrain_block", stale, "TERRAIN_INSPECT_STALE")
            tiny = copy.deepcopy(query)
            tiny["limits"] = {"max_json_bytes": 1}
            tool("query_terrain_block", tiny, "MCP_TERRAIN_JSON_BUDGET")
            require(tool("get_revision", {})["revision"] == revision, "query changed revision")
            process.stdin.close()
            require(process.wait(timeout=10) == 0, "server did not exit cleanly")
            receipt["exit_code"] = process.returncode
        receipt["source_after"] = inventory(output / "fixture")
        require(receipt["source_after"] == before, "query modified fixture")
        require(sha(server) == receipt["server_sha256"] and sha(exporter) == receipt["exporter_sha256"], "binary changed during probe")
        receipt["passed"] = True
    except Exception as error:
        receipt["error"] = str(error)
        raise
    finally:
        if process is not None and process.poll() is None:
            # 仅处理本脚本创建且仍持有句柄的专用进程。
            process.kill()
            process.wait(timeout=10)
            receipt["owned_process_killed"] = True
        (output / "receipt.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps({"passed": receipt["passed"], "calls": len(receipt["calls"]), "receipt": str(output / "receipt.json")}))


if __name__ == "__main__":
    main()
