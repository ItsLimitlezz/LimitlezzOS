#!/usr/bin/env python3
"""
Print a release evidence checklist for a T-Deck firmware PR.

This helper keeps slow local hosts on the intended path: use local checks for
native/simulator confidence, use GitHub Actions for the T-Deck build, then
flash the exact Actions artifact on the hardware port.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path


REQUIRED_FLASH_FILES = ("bootloader.bin", "boot_app0.bin", "partitions.bin", "firmware.bin")


def default_artifact_dir(env_name: str) -> Path:
    leaf = "tdeck-meshcore" if env_name == "tdeck-meshcore" else "tdeck"
    return Path(".pio") / "ci-artifacts" / leaf


def run_text(cmd: list[str], cwd: Path, *, required: bool = False) -> str | None:
    try:
        return subprocess.check_output(cmd, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()
    except FileNotFoundError as exc:
        if required:
            raise SystemExit(f"missing command: {cmd[0]}") from exc
    except subprocess.CalledProcessError as exc:
        if required:
            raise SystemExit(exc.output.strip() or f"command failed: {' '.join(cmd)}") from exc
    return None


def git(project_dir: Path, *args: str) -> str:
    return run_text(["git", *args], project_dir, required=True) or ""


def default_repo(project_dir: Path) -> str:
    data = run_text(["gh", "repo", "view", "--json", "nameWithOwner"], project_dir)
    if data:
        try:
            repo = json.loads(data).get("nameWithOwner")
            if repo:
                return repo
        except json.JSONDecodeError:
            pass

    remote = run_text(["git", "remote", "get-url", "origin"], project_dir)
    if remote and "github.com" in remote:
        cleaned = remote.rstrip("/").removesuffix(".git")
        for marker in ("github.com:", "github.com/"):
            if marker in cleaned:
                return cleaned.split(marker, 1)[1]
    return "ItsLimitlezz/LimitlezzOS"


def default_port() -> str:
    return os.environ.get("LZ_SERIAL_PORT") or ("COM8" if os.name == "nt" else "/dev/ttyACM0")


def load_runs(project_dir: Path, repo: str, workflow: str, branch: str) -> list[dict] | None:
    data = run_text(
        [
            "gh",
            "run",
            "list",
            "--repo",
            repo,
            "--workflow",
            workflow,
            "--branch",
            branch,
            "--limit",
            "20",
            "--json",
            "databaseId,headSha,status,conclusion,createdAt,url",
        ],
        project_dir,
    )
    if not data:
        return None
    try:
        return json.loads(data)
    except json.JSONDecodeError:
        return None


def choose_run(runs: list[dict] | None, commit: str) -> dict | None:
    if not runs:
        return None
    for run in runs:
        if run.get("headSha", "").lower() == commit.lower():
            return run
    return None


def find_manifest(artifact_dir: Path) -> Path | None:
    direct = artifact_dir / "FLASH_MANIFEST.txt"
    if direct.exists():
        return direct
    candidates = sorted(artifact_dir.rglob("FLASH_MANIFEST.txt")) if artifact_dir.exists() else []
    if not candidates:
        return None
    return candidates[0]


def parse_manifest(path: Path | None) -> dict[str, str]:
    if path is None:
        return {}
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in raw:
            continue
        key, value = raw.split("=", 1)
        key = key.strip()
        if key and all(c.isalnum() or c == "_" for c in key):
            values[key] = value.strip()
    return values


def present_flash_files(artifact_dir: Path) -> list[str]:
    return [name for name in REQUIRED_FLASH_FILES if (artifact_dir / name).exists()]


def md_code(value: object) -> str:
    return f"`{value}`"


def run_line(run: dict | None, commit: str) -> str:
    if run is None:
        return f"not found yet for {md_code(commit)}"
    status = run.get("status") or "unknown"
    conclusion = run.get("conclusion") or "pending"
    run_id = run.get("databaseId") or "unknown"
    url = run.get("url") or ""
    suffix = f" ([run {run_id}]({url}))" if url else f" (run {run_id})"
    return f"{md_code(status + '/' + conclusion)}{suffix}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Print a T-Deck release evidence checklist.")
    parser.add_argument("--project-dir", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--repo", help="GitHub repo, e.g. ItsLimitlezz/LimitlezzOS.")
    parser.add_argument("--workflow", default="Firmware CI")
    parser.add_argument("--branch", help="Branch name. Defaults to the current git branch.")
    parser.add_argument("--commit", help="Commit SHA. Defaults to HEAD.")
    parser.add_argument("--env", default="tdeck", choices=("tdeck", "tdeck-meshcore"),
                        help="Firmware artifact environment.")
    parser.add_argument("--artifact-dir", help="Downloaded artifact directory. Defaults from --env.")
    parser.add_argument("--port", default=default_port())
    args = parser.parse_args()

    project_dir = Path(args.project_dir).resolve()
    branch = args.branch or git(project_dir, "branch", "--show-current")
    commit = args.commit or git(project_dir, "rev-parse", "HEAD")
    short_commit = commit[:12]
    repo = args.repo or default_repo(project_dir)
    artifact_dir = Path(args.artifact_dir) if args.artifact_dir else default_artifact_dir(args.env)
    if not artifact_dir.is_absolute():
        artifact_dir = (project_dir / artifact_dir).resolve()

    run = choose_run(load_runs(project_dir, repo, args.workflow, branch), commit)
    manifest_path = find_manifest(artifact_dir)
    manifest = parse_manifest(manifest_path)
    files = present_flash_files(artifact_dir)
    manifest_env = manifest.get("env") or "tdeck"
    manifest_sha = manifest.get("sha", "")
    manifest_matches = (
        bool(manifest_path) and
        manifest_sha.lower() == commit.lower() and
        manifest_env == args.env
    )
    actions_status = "passed" if run and run.get("status") == "completed" and run.get("conclusion") == "success" else "pending"
    artifact_status = "downloaded" if manifest_matches and len(files) == len(REQUIRED_FLASH_FILES) else "pending"

    print("## T-Deck Release Evidence")
    print()
    print(f"- Repository: {md_code(repo)}")
    print(f"- Branch: {md_code(branch)}")
    print(f"- Commit: {md_code(commit)}")
    print(f"- Environment: {md_code(args.env)}")
    print(f"- Workflow: {md_code(args.workflow)}")
    print(f"- Actions run: {run_line(run, commit)}")
    print(f"- Artifact dir: {md_code(artifact_dir)}")
    print(f"- Artifact manifest: {md_code(manifest_path) if manifest_path else 'not downloaded yet'}")
    print(f"- Manifest SHA: {md_code(manifest_sha or 'not recorded')}")
    print(f"- Manifest env: {md_code(manifest_env)}")
    print(f"- Manifest matches request: {md_code('yes' if manifest_matches else 'no')}")
    print(f"- Flash bundle files: {md_code(', '.join(files) if files else 'not downloaded yet')}")
    print(f"- Hardware port: {md_code(args.port)}")
    print()
    print("### Local Sanity")
    print()
    print("- [ ] `python -m py_compile scripts/tdeck_smoke.py scripts/fetch_tdeck_artifact.py scripts/release_evidence.py`")
    print("- [ ] `pio run -e native`")
    print("- [ ] `.pio/build/native/program --selftest`")
    print("- [ ] `.pio/build/native/program --simtest`")
    print()
    print("### GitHub Actions Build")
    print()
    print(f"- [ ] Push branch: `git push fork HEAD:{branch}`")
    print(f"- [ ] Wait for exact-commit Actions success: `gh run watch --repo {repo} <run-id> --exit-status --interval 10`")
    print(
        "- [ ] Fetch exact artifact: "
        f"`python scripts/fetch_tdeck_artifact.py --env {args.env} --repo {repo} --branch {branch} --commit {commit} --out {artifact_dir}`"
    )
    print("- [ ] Confirm `FLASH_MANIFEST.txt` `sha=` matches the PR head commit.")
    if args.env == "tdeck-meshcore":
        print("- [ ] Confirm `FLASH_MANIFEST.txt` has `env=tdeck-meshcore` and `meshcore_enabled=1`.")
    print()
    print("### COM8 Hardware Smoke")
    print()
    print(
        "- [ ] Flash exact artifact: "
        f"`python scripts/tdeck_smoke.py --port {args.port} --env {args.env} --no-stub-upload --skip-build "
        f"--artifact-dir {artifact_dir} --open-timeout 60 --boot-timeout 60 --timeout 30`"
    )
    print("- [ ] Record flash chip, MAC, byte counts, and hash verification.")
    print("- [ ] Record serial `id`, `sys`, `net`, `rf`, `stats`, `wifi`, and `companion test` output.")
    if args.env == "tdeck-meshcore":
        print("- [ ] Run split-airtime smoke: "
              f"`python scripts/tdm_airtime_smoke.py --port {args.port} --open-timeout 60 --boot-timeout 60 --timeout 30`")
        print("- [ ] Record 60/40, 50/50, 40/60 dwell checks and switch-count motion.")
    print("- [ ] Manually verify display, touch, keyboard, trackball, SD/appfs browsing, Wi-Fi state, and sleep/wake.")
    print()
    print("### PR Evidence Snippet")
    print()
    print(f"- Local native checks: pending for {md_code(short_commit)}.")
    print(f"- GitHub Actions `{args.workflow}`: {actions_status} for {md_code(short_commit)}.")
    print(f"- Artifact: {artifact_status} at {md_code(artifact_dir)}.")
    print("- Hardware: pending COM8 exact-artifact flash/smoke.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
