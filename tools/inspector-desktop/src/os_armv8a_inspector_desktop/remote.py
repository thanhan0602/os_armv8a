from __future__ import annotations

import json
import shlex
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import paramiko


CONFIG_PATH = Path.home() / ".os_armv8a_inspector_desktop.json"


@dataclass
class DesktopConfig:
    host: str = ""
    port: int = 22
    username: str = ""
    key_path: str = ""
    repo_path: str = "/home/a/Learn/os_armv8a"
    qemu_bin: str = ""
    build_with_demos: bool = True

    @classmethod
    def load(cls) -> "DesktopConfig":
        if not CONFIG_PATH.exists():
            return cls()
        try:
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except Exception:
            return cls()
        defaults = asdict(cls())
        for key, value in data.items():
            if key in defaults:
                defaults[key] = value
        return cls(**defaults)

    def save(self) -> None:
        CONFIG_PATH.write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")


@dataclass
class CommandResult:
    stdout: str
    stderr: str
    exit_code: int


class SshRemote:
    def __init__(self) -> None:
        self._client: paramiko.SSHClient | None = None

    @property
    def connected(self) -> bool:
        return self._client is not None

    def connect(
        self,
        host: str,
        port: int,
        username: str,
        password: str = "",
        key_path: str = "",
    ) -> None:
        self.disconnect()
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        connect_kwargs: dict[str, Any] = {
            "hostname": host,
            "port": port,
            "username": username,
            "look_for_keys": not bool(password),
            "allow_agent": True,
            "timeout": 10,
        }
        if key_path:
            connect_kwargs["key_filename"] = key_path
        elif password:
            connect_kwargs["password"] = password
        client.connect(**connect_kwargs)
        self._client = client

    def disconnect(self) -> None:
        if self._client is not None:
            self._client.close()
            self._client = None

    def exec(self, command: str, timeout: float = 30.0) -> CommandResult:
        if self._client is None:
            raise RuntimeError("SSH is not connected")
        stdin, stdout, stderr = self._client.exec_command(command, timeout=timeout)
        del stdin
        out = stdout.read().decode("utf-8", errors="replace")
        err = stderr.read().decode("utf-8", errors="replace")
        code = stdout.channel.recv_exit_status()
        return CommandResult(stdout=out, stderr=err, exit_code=code)


class InspectorRemote:
    def __init__(self, ssh: SshRemote, repo_path: str, qemu_bin: str = "") -> None:
        self._ssh = ssh
        self.repo_path = repo_path
        self.qemu_bin = qemu_bin.strip()

    def _env_prefix(self) -> str:
        if not self.qemu_bin:
            return ""
        return f"QEMU_BIN={shlex.quote(self.qemu_bin)} "

    def _run_repo(self, command: str, timeout: float = 60.0) -> CommandResult:
        full = f"cd {shlex.quote(self.repo_path)} && {command}"
        return self._ssh.exec(full, timeout=timeout)

    def build_demos(self) -> CommandResult:
        return self._run_repo("make clean all RUN_OS_DEMOS=1", timeout=600.0)

    def start(self) -> CommandResult:
        return self._run_repo(f"{self._env_prefix()}bash tools/qemu-inspector/start.sh", timeout=60.0)

    def stop(self) -> CommandResult:
        return self._run_repo("bash tools/qemu-inspector/start.sh --stop", timeout=30.0)

    def status_script(self) -> CommandResult:
        return self._run_repo("bash tools/qemu-inspector/start.sh --status", timeout=30.0)

    def api_get(self, path: str, timeout: float = 30.0) -> Any:
        cmd = (
            "curl -sS --fail "
            + shlex.quote(f"http://127.0.0.1:8888{path}")
        )
        result = self._run_repo(cmd, timeout=timeout)
        if result.exit_code != 0:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip() or f"curl failed ({result.exit_code})")
        return json.loads(result.stdout)

    def api_post(self, path: str, body: dict[str, Any] | None = None, timeout: float = 30.0) -> Any:
        payload = json.dumps(body or {})
        cmd = (
            "curl -sS --fail -X POST -H 'Content-Type: application/json' "
            + "-d " + shlex.quote(payload) + " "
            + shlex.quote(f"http://127.0.0.1:8888{path}")
        )
        result = self._run_repo(cmd, timeout=timeout)
        if result.exit_code != 0:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip() or f"curl failed ({result.exit_code})")
        return json.loads(result.stdout)