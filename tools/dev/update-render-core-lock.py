"""为已经提交且干净的唯一共享上游生成完整字节锁，不修改上游。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

PUBLIC_REPOSITORY = "https://github.com/CallDisaster/Audastra-RenderCore.git"


def main():
    forge = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=forge.parent / "War3RenderCore")
    parser.add_argument("--repository", default=PUBLIC_REPOSITORY, help="公开取源地址，写入锁供 tools/fetch-render-core.ps1 使用")
    args = parser.parse_args()
    shared = args.source_dir.resolve(strict=True)

    def git(*arguments):
        return subprocess.check_output(["git", "-C", str(shared), *arguments])

    if git("status", "--porcelain", "--untracked-files=normal").strip():
        raise SystemExit("共享工作树有未提交内容；先提交，不能把脏字节登记为已锁版本。")
    version = re.search(r"project\(War3RenderCore\s+VERSION\s+([0-9.]+)",
                        (shared / "CMakeLists.txt").read_text(encoding="utf-8"))
    if version is None:
        raise SystemExit("共享项目没有明确版本号。")
    paths = sorted(p.decode("utf-8") for p in git("ls-files", "-z").split(b"\0") if p)
    files = []
    for relative in paths:
        source = (shared / relative).resolve(strict=True)
        if not source.is_relative_to(shared) or not source.is_file():
            raise SystemExit("共享锁定文件必须是仓库内普通文件：" + relative)
        files.append(dict(path=relative, sha256=hashlib.sha256(source.read_bytes()).hexdigest()))
    lock = dict(schema=1, version=version.group(1), repository=args.repository,
                commit=git("rev-parse", "HEAD").decode().strip(), files=files)
    (forge / "War3RenderCore.lock.json").write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"version={lock['version']} commit={lock['commit']} tracked_file_hashes={len(files)}")


if __name__ == "__main__":
    main()
