"""通过真实 MCP stdio 验证有界荒芜保存；所有输出留在独立证据目录。"""

import argparse
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


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "cli", "source", "reference", "ydwe-root", "war3-root", "qt-bin", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    paths = {name.replace("_", "-"): value.resolve(strict=True)
             for name, value in vars(args).items() if name != "output"}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    candidate = output / "mcp-blight-candidate.w3x"
    env = dict(os.environ)
    env["PATH"] = str(paths["qt-bin"]) + os.pathsep + env.get("PATH", "")
    env["FORGE_MCP_YDWE_ROOT"] = str(paths["ydwe-root"])
    env["FORGE_MCP_WAR3_ROOT"] = str(paths["war3-root"])
    flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0
    receipt = {"schema": "forge-mcp-blight-stdio-probe-v1", "passed": False,
               "source_sha256": sha(paths["source"]), "reference_sha256": sha(paths["reference"]),
               "server_sha256": sha(paths["server"]), "calls": []}
    process = None

    def extract(archive, member, target):
        done = subprocess.run([str(paths["cli"]), "mpq-extract", str(archive), member, str(target)],
                              env=env, creationflags=flags, timeout=30, capture_output=True)
        require(done.returncode == 0 and target.is_file(), f"MPQ extract failed: {member}")
        return sha(target)

    try:
        terrain_sha = extract(paths["source"], "war3map.w3e", output / "source.w3e")
        receipt["source_terrain_sha256"] = terrain_sha
        with (output / "stderr.log").open("wb") as stderr, (output / "transcript.jsonl").open("w", encoding="utf-8") as log:
            process = subprocess.Popen([str(paths["server"]), "--workspace", str(output),
                                        "--ui-data", str(paths["ydwe-root"])],
                                       env=env, creationflags=flags, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=stderr)
            receipt["pid"] = process.pid
            responses = queue.Queue()

            def reader():
                try:
                    while line := process.stdout.readline(2 * 1024 * 1024 + 1):
                        require(len(line) <= 2 * 1024 * 1024 and line.endswith(b"\n"),
                                "stdio response exceeds line budget")
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
                process.stdin.write((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
                process.stdin.flush()
                response = responses.get(timeout=120)
                if isinstance(response, Exception):
                    raise response
                log.write(json.dumps({"response": response}, ensure_ascii=False) + "\n")
                log.flush()
                require(response.get("id") == sequence and "error" not in response,
                        "JSON-RPC transport failed")
                receipt["calls"].append({"method": method, "name": params.get("name")})
                return response["result"]

            request("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                   "clientInfo": {"name": "forge-blight-probe", "version": "1"}})
            listed = request("tools/list", {})
            require(any(tool["name"] == "save_terrain_blight_candidate" for tool in listed["tools"]),
                    "blight save tool not advertised")
            fields = {"source_map_path": str(paths["source"]), "output_path": str(candidate),
                      "expected_source_sha256": receipt["source_sha256"],
                      "expected_terrain_sha256": terrain_sha,
                      "vertex_x": 10, "vertex_y": 10, "radius": 2, "set_blight": True}

            def call(arguments):
                result = request("tools/call", {"name": "save_terrain_blight_candidate",
                                                "arguments": arguments})
                return result, json.loads(result["content"][0]["text"])

            stale = dict(fields)
            stale["expected_source_sha256"] = "0" * 64
            rejected, detail = call(stale)
            require(rejected.get("isError") is True and detail.get("ok") is False
                    and not candidate.exists(), "stale source identity was not rejected")
            receipt["stale_diagnostics"] = detail.get("diagnostics", [])

            saved, detail = call(fields)
            require(not saved.get("isError") and detail.get("ok") is True and candidate.is_file(),
                    f"MCP candidate save failed: {detail}")
            require(detail.get("source_sha256") == receipt["source_sha256"]
                    and detail.get("candidate_sha256") == sha(candidate)
                    and detail.get("script_rebuilt") is True
                    and detail.get("native_editor_verified") is False
                    and detail.get("game_runtime_verified") is False,
                    "save receipt identity or acceptance claim invalid")
            receipt["saved"] = detail
            process.stdin.close()
            require(process.wait(timeout=10) == 0, "server did not exit cleanly")

        for member in ("war3map.w3e", "war3map.wpm"):
            candidate_sha = extract(candidate, member, output / ("candidate." + member[-3:]))
            reference_sha = extract(paths["reference"], member, output / ("reference." + member[-3:]))
            receipt[member] = {"candidate_sha256": candidate_sha, "reference_sha256": reference_sha}
            require(candidate_sha == reference_sha, f"MCP and CLI {member} differ")
        require(sha(paths["source"]) == receipt["source_sha256"], "source archive changed")
        receipt["passed"] = True
    except Exception as error:
        receipt["error"] = str(error)
        raise
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=10)
            receipt["owned_process_killed"] = True
        (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": receipt["passed"], "candidate": str(candidate),
                      "receipt": str(output / "receipt.json")}))


if __name__ == "__main__":
    main()
