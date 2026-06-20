#!/usr/bin/env python3
"""
Download a T-Deck firmware artifact from GitHub Actions.

Default behavior is strict: use the current branch and current commit, then
download the successful Firmware CI artifact for that exact SHA. This keeps the
local flash step from accidentally using an older build.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ARTIFACT_BUNDLE_FILES = (
    "bootloader.bin",
    "boot_app0.bin",
    "firmware.bin",
    "firmware.elf",
    "firmware.map",
    "partitions.bin",
    "FLASH_MANIFEST.txt",
    "SIZE_BUDGET.md",
    "SIZE_BUDGET.txt",
    "size-budget.json",
    "tdeck-build.txt",
    "tdeck-size.txt",
)


def run_text(cmd: list[str], cwd: Path) -> str:
    try:
        return subprocess.check_output(cmd, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()
    except FileNotFoundError as exc:
        raise SystemExit(f"missing command: {cmd[0]}") from exc
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.output.strip() or f"command failed: {' '.join(cmd)}") from exc


def git(project_dir: Path, *args: str) -> str:
    return run_text(["git", *args], project_dir)


def current_branch(project_dir: Path) -> str:
    return git(project_dir, "branch", "--show-current")


def current_commit(project_dir: Path) -> str:
    return git(project_dir, "rev-parse", "HEAD")


def repo_from_remote_url(url: str) -> str | None:
    clean = url.strip()
    if clean.endswith(".git"):
        clean = clean[:-4]
    marker = "github.com"
    if marker not in clean:
        return None
    tail = clean.split(marker, 1)[1].lstrip("/:").strip("/")
    parts = tail.split("/")
    if len(parts) >= 2 and parts[0] and parts[1]:
        return f"{parts[0]}/{parts[1]}"
    return None


def tracking_repo(project_dir: Path) -> str | None:
    branch = current_branch(project_dir)
    if not branch:
        return None
    try:
        remote = git(project_dir, "config", "--get", f"branch.{branch}.remote")
        url = git(project_dir, "remote", "get-url", remote)
    except SystemExit:
        return None
    return repo_from_remote_url(url)


def default_repo(project_dir: Path) -> str:
    repo = tracking_repo(project_dir)
    if repo:
        return repo
    try:
        data = json.loads(run_text(["gh", "repo", "view", "--json", "nameWithOwner"], project_dir))
        repo = data.get("nameWithOwner")
        if repo:
            return repo
    except SystemExit:
        pass
    return "ItsLimitlezz/LimitlezzOS"


def load_runs(project_dir: Path, repo: str, workflow: str, branch: str, limit: int) -> list[dict]:
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
            str(limit),
            "--json",
            "databaseId,headSha,status,conclusion,createdAt,url",
        ],
        project_dir,
    )
    return json.loads(data)


def load_artifacts(project_dir: Path, repo: str, run_id: int) -> list[dict]:
    data = run_text(
        ["gh", "api", f"repos/{repo}/actions/runs/{run_id}/artifacts"],
        project_dir,
    )
    return json.loads(data).get("artifacts", [])


def artifact_prefix(env_name: str) -> str:
    if env_name == "tdeck":
        return "tdeck-firmware"
    if env_name == "tdeck-meshcore":
        return "tdeck-meshcore-firmware"
    raise SystemExit(f"Unsupported artifact environment: {env_name}")


def default_out_dir(env_name: str) -> Path:
    leaf = "tdeck-meshcore" if env_name == "tdeck-meshcore" else "tdeck"
    return Path(".pio") / "ci-artifacts" / leaf


def clear_existing_bundle(out_dir: Path) -> None:
    for name in ARTIFACT_BUNDLE_FILES:
        path = out_dir / name
        if path.is_file():
            path.unlink()


def choose_run(runs: list[dict], commit: str, allow_latest_success: bool) -> dict:
    successful = [r for r in runs if r.get("status") == "completed" and r.get("conclusion") == "success"]
    for run in successful:
        if run.get("headSha", "").lower() == commit.lower():
            return run
    if allow_latest_success and successful:
        return successful[0]

    seen = "\n".join(
        f"  {r.get('createdAt')} {r.get('status')}/{r.get('conclusion')} {r.get('headSha')} {r.get('url')}"
        for r in runs[:8]
    )
    raise SystemExit(
        f"No successful Firmware CI run found for commit {commit}.\n"
        "Push the branch and wait for GitHub Actions, then rerun this command.\n"
        f"Recent runs:\n{seen}"
    )


def bundle_dir(out_dir: Path) -> Path:
    required = ("firmware.bin", "bootloader.bin", "partitions.bin")
    if all((out_dir / name).exists() for name in required):
        return out_dir

    parents: set[Path] = set()
    for firmware in out_dir.rglob("firmware.bin"):
        parent = firmware.parent
        if all((parent / name).exists() for name in required):
            parents.add(parent)
    if len(parents) == 1:
        return next(iter(parents))
    if not parents:
        raise SystemExit(f"Downloaded artifact does not contain required files under {out_dir}")
    choices = "\n".join(f"  {p}" for p in sorted(parents))
    raise SystemExit(f"Multiple possible artifact bundle directories found:\n{choices}")


def load_flash_manifest(bundle: Path) -> dict[str, str]:
    manifest = bundle / "FLASH_MANIFEST.txt"
    if not manifest.exists():
        raise SystemExit(f"Downloaded artifact is missing {manifest}")

    values: dict[str, str] = {}
    for raw in manifest.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw or raw.lstrip().startswith("#") or "=" not in raw:
            continue
        key, value = raw.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def validate_flash_manifest(bundle: Path, expected_sha: str, expected_run_id: int, expected_env: str) -> dict[str, str]:
    values = load_flash_manifest(bundle)
    actual_sha = values.get("sha", "")
    actual_run_id = values.get("run_id", "")
    actual_env = values.get("env") or "tdeck"
    if actual_sha.lower() != expected_sha.lower():
        raise SystemExit(
            f"Downloaded artifact SHA mismatch: manifest has {actual_sha or '<missing>'}, "
            f"expected {expected_sha}."
        )
    if actual_run_id != str(expected_run_id):
        raise SystemExit(
            f"Downloaded artifact run mismatch: manifest has {actual_run_id or '<missing>'}, "
            f"expected {expected_run_id}."
        )
    if values.get("budget_status") != "pass":
        raise SystemExit(
            f"Downloaded artifact did not pass the T-Deck budget gate: "
            f"{values.get('budget_status', '<missing>')}"
        )
    if actual_env != expected_env:
        raise SystemExit(
            f"Downloaded artifact environment mismatch: manifest has {actual_env}, "
            f"expected {expected_env}."
        )
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description="Download the current branch T-Deck firmware artifact.")
    parser.add_argument("--project-dir", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--env", default="tdeck", choices=("tdeck", "tdeck-meshcore"),
                        help="Firmware environment artifact to download.")
    parser.add_argument("--repo", help="GitHub repo to read Actions artifacts from, e.g. ItsLimitlezz/LimitlezzOS.")
    parser.add_argument("--workflow", default="Firmware CI")
    parser.add_argument("--branch", help="Branch name. Defaults to the current git branch.")
    parser.add_argument("--commit", help="Commit SHA. Defaults to HEAD.")
    parser.add_argument("--run-id", type=int, help="Download a specific run instead of searching.")
    parser.add_argument("--artifact-name", help="Artifact name. Defaults from --env and SHA.")
    parser.add_argument("--out", help="Output directory. Defaults from --env.")
    parser.add_argument("--allow-latest-success", action="store_true", help="Allow newest successful run if HEAD has none.")
    args = parser.parse_args()

    project_dir = Path(args.project_dir).resolve()
    repo = args.repo or default_repo(project_dir)
    branch = args.branch or current_branch(project_dir)
    commit = args.commit or current_commit(project_dir)
    out_arg = Path(args.out) if args.out else default_out_dir(args.env)
    out_dir = (project_dir / out_arg).resolve() if not out_arg.is_absolute() else out_arg

    if args.run_id is not None:
        run_id = args.run_id
        artifact_sha = commit
        run_url = f"https://github.com/{repo}/actions/runs/{run_id}"
    else:
        runs = load_runs(project_dir, repo, args.workflow, branch, 50)
        chosen = choose_run(runs, commit, args.allow_latest_success)
        run_id = int(chosen["databaseId"])
        artifact_sha = chosen["headSha"]
        run_url = chosen["url"]

    prefix = artifact_prefix(args.env)
    artifact_name = args.artifact_name or f"{prefix}-{artifact_sha}"
    if args.artifact_name is None:
        artifacts = load_artifacts(project_dir, repo, run_id)
        names = [a.get("name", "") for a in artifacts]
        if artifact_name not in names:
            env_names = [name for name in names if name.startswith(f"{prefix}-")]
            if len(env_names) == 1:
                print(
                    f"[artifact] expected {artifact_name}, using run artifact {env_names[0]}",
                    file=sys.stderr,
                )
                artifact_name = env_names[0]
            else:
                available = "\n".join(f"  {name}" for name in names)
                raise SystemExit(
                    f"Artifact {artifact_name} was not found in run {run_id}.\n"
                    f"Available artifacts:\n{available}"
                )
    out_dir.mkdir(parents=True, exist_ok=True)
    clear_existing_bundle(out_dir)

    run_text(
        [
            "gh",
            "run",
            "download",
            str(run_id),
            "--repo",
            repo,
            "--name",
            artifact_name,
            "--dir",
            str(out_dir),
        ],
        project_dir,
    )

    bundle = bundle_dir(out_dir)
    manifest = validate_flash_manifest(bundle, artifact_sha, run_id, args.env)
    print(f"[artifact] run: {run_url}")
    print(f"[artifact] name: {artifact_name}")
    print(f"[artifact] dir: {bundle}")
    print(
        f"[artifact] manifest: sha={manifest.get('sha')} run_id={manifest.get('run_id')} "
        f"env={manifest.get('env') or 'tdeck'} budget={manifest.get('budget_status')}"
    )
    print("[artifact] flash:")
    print(f"  python scripts/tdeck_smoke.py --port COM8 --env {args.env} --no-stub-upload --skip-build --artifact-dir {bundle}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
