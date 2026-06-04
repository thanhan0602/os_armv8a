from __future__ import annotations

import json
import threading
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Any, Callable

from .remote import DesktopConfig, InspectorRemote, SshRemote


class InspectorDesktopApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("OS ARMv8-A Inspector Desktop")
        self.root.geometry("1280x860")

        self.config = DesktopConfig.load()
        self.ssh = SshRemote()
        self.remote: InspectorRemote | None = None

        self._build_ui()
        self._load_config_into_form()

    def _build_ui(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        top = ttk.Frame(self.root, padding=10)
        top.grid(row=0, column=0, sticky="nsew")
        for i in range(4):
            top.columnconfigure(i, weight=1)

        self.host_var = tk.StringVar()
        self.port_var = tk.StringVar(value="22")
        self.user_var = tk.StringVar()
        self.password_var = tk.StringVar()
        self.key_var = tk.StringVar()
        self.repo_var = tk.StringVar()
        self.qemu_var = tk.StringVar()
        self.break_var = tk.StringVar(value="schedule")
        self.walk_va_var = tk.StringVar(value="0x10000")
        self.walk_task_var = tk.StringVar(value="3")
        self.raw_pa_var = tk.StringVar(value="0x40080000")
        self.raw_count_var = tk.StringVar(value="64")

        fields = [
            ("SSH Host", self.host_var),
            ("SSH Port", self.port_var),
            ("Username", self.user_var),
            ("Password", self.password_var),
            ("Key Path", self.key_var),
            ("Repo Path", self.repo_var),
            ("QEMU Bin", self.qemu_var),
        ]
        for idx, (label, variable) in enumerate(fields):
            row = idx // 2
            col = (idx % 2) * 2
            ttk.Label(top, text=label).grid(row=row, column=col, sticky="w", padx=(0, 8), pady=4)
            show = "*" if label == "Password" else ""
            ttk.Entry(top, textvariable=variable, show=show).grid(row=row, column=col + 1, sticky="ew", padx=(0, 16), pady=4)

        buttons = ttk.Frame(top)
        buttons.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(8, 0))
        for i in range(10):
            buttons.columnconfigure(i, weight=1)

        ttk.Button(buttons, text="Connect", command=self.connect).grid(row=0, column=0, sticky="ew", padx=2)
        ttk.Button(buttons, text="Disconnect", command=self.disconnect).grid(row=0, column=1, sticky="ew", padx=2)
        ttk.Button(buttons, text="Build Demos", command=self.build_demos).grid(row=0, column=2, sticky="ew", padx=2)
        ttk.Button(buttons, text="Start Remote Inspector", command=self.start_remote).grid(row=0, column=3, sticky="ew", padx=2)
        ttk.Button(buttons, text="Stop Remote Inspector", command=self.stop_remote).grid(row=0, column=4, sticky="ew", padx=2)
        ttk.Button(buttons, text="Refresh", command=self.refresh_all).grid(row=0, column=5, sticky="ew", padx=2)
        ttk.Button(buttons, text="Pause", command=self.pause_vm).grid(row=0, column=6, sticky="ew", padx=2)
        ttk.Button(buttons, text="Continue", command=self.continue_vm).grid(row=0, column=7, sticky="ew", padx=2)

        controls = ttk.Frame(top)
        controls.grid(row=5, column=0, columnspan=4, sticky="ew", pady=(8, 0))
        for i in range(8):
            controls.columnconfigure(i, weight=1)

        ttk.Label(controls, text="Break symbol / VA").grid(row=0, column=0, sticky="w")
        ttk.Entry(controls, textvariable=self.break_var).grid(row=0, column=1, sticky="ew", padx=4)
        ttk.Button(controls, text="Break + Snapshot", command=self.break_and_snapshot).grid(row=0, column=2, sticky="ew", padx=4)

        ttk.Label(controls, text="Walk VA").grid(row=0, column=3, sticky="w")
        ttk.Entry(controls, textvariable=self.walk_va_var).grid(row=0, column=4, sticky="ew", padx=4)
        ttk.Label(controls, text="Task Idx").grid(row=0, column=5, sticky="w")
        ttk.Entry(controls, textvariable=self.walk_task_var).grid(row=0, column=6, sticky="ew", padx=4)
        ttk.Button(controls, text="Walk", command=self.walk_task).grid(row=0, column=7, sticky="ew", padx=4)

        controls2 = ttk.Frame(top)
        controls2.grid(row=6, column=0, columnspan=4, sticky="ew", pady=(8, 0))
        for i in range(6):
            controls2.columnconfigure(i, weight=1)

        ttk.Label(controls2, text="Raw PA").grid(row=0, column=0, sticky="w")
        ttk.Entry(controls2, textvariable=self.raw_pa_var).grid(row=0, column=1, sticky="ew", padx=4)
        ttk.Label(controls2, text="Count").grid(row=0, column=2, sticky="w")
        ttk.Entry(controls2, textvariable=self.raw_count_var).grid(row=0, column=3, sticky="ew", padx=4)
        ttk.Button(controls2, text="Read Raw Memory", command=self.read_rawmem).grid(row=0, column=4, sticky="ew", padx=4)
        ttk.Button(controls2, text="Read Registers", command=self.read_registers).grid(row=0, column=5, sticky="ew", padx=4)

        notebook = ttk.Notebook(self.root)
        notebook.grid(row=1, column=0, sticky="nsew", padx=10, pady=(0, 10))

        overview = ttk.Frame(notebook, padding=8)
        tasks = ttk.Frame(notebook, padding=8)
        owners = ttk.Frame(notebook, padding=8)
        walk = ttk.Frame(notebook, padding=8)
        logs = ttk.Frame(notebook, padding=8)
        notebook.add(overview, text="Overview")
        notebook.add(tasks, text="Tasks")
        notebook.add(owners, text="Page Owners")
        notebook.add(walk, text="Walk / Debug")
        notebook.add(logs, text="Activity")

        overview.columnconfigure(0, weight=1)
        overview.rowconfigure(0, weight=1)
        self.overview_text = tk.Text(overview, wrap="word")
        self.overview_text.grid(row=0, column=0, sticky="nsew")

        tasks.columnconfigure(0, weight=1)
        tasks.rowconfigure(0, weight=1)
        self.tasks_tree = ttk.Treeview(tasks, columns=("idx", "id", "name", "state", "mm", "root"), show="headings")
        for key, title, width in [
            ("idx", "Idx", 60),
            ("id", "ID", 60),
            ("name", "Name", 140),
            ("state", "State", 100),
            ("mm", "MM", 220),
            ("root", "TTBR0 Root PA", 180),
        ]:
            self.tasks_tree.heading(key, text=title)
            self.tasks_tree.column(key, width=width, anchor="w")
        self.tasks_tree.grid(row=0, column=0, sticky="nsew")

        owners.columnconfigure(0, weight=1)
        owners.rowconfigure(0, weight=1)
        self.owners_tree = ttk.Treeview(owners, columns=("task_idx", "task_name", "va", "pa", "ap", "exec"), show="headings")
        for key, title, width in [
            ("task_idx", "Task Idx", 80),
            ("task_name", "Task", 140),
            ("va", "VA", 160),
            ("pa", "PA", 160),
            ("ap", "AP", 80),
            ("exec", "Exec", 80),
        ]:
            self.owners_tree.heading(key, text=title)
            self.owners_tree.column(key, width=width, anchor="w")
        self.owners_tree.grid(row=0, column=0, sticky="nsew")

        walk.columnconfigure(0, weight=1)
        walk.rowconfigure(0, weight=1)
        self.walk_text = tk.Text(walk, wrap="word")
        self.walk_text.grid(row=0, column=0, sticky="nsew")

        logs.columnconfigure(0, weight=1)
        logs.rowconfigure(0, weight=1)
        self.log_text = tk.Text(logs, wrap="word")
        self.log_text.grid(row=0, column=0, sticky="nsew")

    def _load_config_into_form(self) -> None:
        self.host_var.set(self.config.host)
        self.port_var.set(str(self.config.port))
        self.user_var.set(self.config.username)
        self.key_var.set(self.config.key_path)
        self.repo_var.set(self.config.repo_path)
        self.qemu_var.set(self.config.qemu_bin)

    def _persist_config(self) -> None:
        self.config.host = self.host_var.get().strip()
        self.config.port = int(self.port_var.get().strip() or "22")
        self.config.username = self.user_var.get().strip()
        self.config.key_path = self.key_var.get().strip()
        self.config.repo_path = self.repo_var.get().strip()
        self.config.qemu_bin = self.qemu_var.get().strip()
        self.config.save()

    def _set_remote(self) -> None:
        self.remote = InspectorRemote(
            self.ssh,
            repo_path=self.repo_var.get().strip(),
            qemu_bin=self.qemu_var.get().strip(),
        )

    def _append_log(self, message: str) -> None:
        self.log_text.insert("end", message.rstrip() + "\n")
        self.log_text.see("end")

    def _set_json_text(self, widget: tk.Text, data: Any) -> None:
        widget.delete("1.0", "end")
        widget.insert("1.0", json.dumps(data, indent=2))

    def _run_async(
        self,
        action_name: str,
        worker: Callable[[], Any],
        on_success: Callable[[Any], None] | None = None,
    ) -> None:
        self._append_log(f"[run] {action_name}")

        def target() -> None:
            try:
                result = worker()
            except Exception as exc:
                self.root.after(0, lambda: self._show_error(action_name, exc))
                return
            if on_success is not None:
                self.root.after(0, lambda: on_success(result))

        threading.Thread(target=target, daemon=True).start()

    def _show_error(self, action_name: str, exc: Exception) -> None:
        self._append_log(f"[error] {action_name}: {exc}")
        messagebox.showerror("Inspector Desktop", f"{action_name} failed\n\n{exc}")

    def connect(self) -> None:
        self._persist_config()

        def worker() -> str:
            self.ssh.connect(
                host=self.host_var.get().strip(),
                port=int(self.port_var.get().strip() or "22"),
                username=self.user_var.get().strip(),
                password=self.password_var.get(),
                key_path=self.key_var.get().strip(),
            )
            self._set_remote()
            return f"Connected to {self.host_var.get().strip()}"

        self._run_async("SSH connect", worker, lambda msg: self._append_log(f"[ok] {msg}"))

    def disconnect(self) -> None:
        self.ssh.disconnect()
        self.remote = None
        self._append_log("[ok] SSH disconnected")

    def _require_remote(self) -> InspectorRemote:
        if self.remote is None or not self.ssh.connected:
            raise RuntimeError("Connect SSH first")
        return self.remote

    def build_demos(self) -> None:
        self._run_async(
            "Build demos",
            lambda: self._require_remote().build_demos(),
            lambda result: self._append_log(result.stdout or result.stderr or "build finished"),
        )

    def start_remote(self) -> None:
        self._run_async(
            "Start remote inspector",
            lambda: self._require_remote().start(),
            lambda result: self._append_log(result.stdout or "started"),
        )

    def stop_remote(self) -> None:
        self._run_async(
            "Stop remote inspector",
            lambda: self._require_remote().stop(),
            lambda result: self._append_log(result.stdout or "stopped"),
        )

    def refresh_all(self) -> None:
        def worker() -> dict[str, Any]:
            remote = self._require_remote()
            status = remote.api_get("/api/status")
            gdb = remote.api_get("/api/gdb/status")
            tasks = remote.api_get("/api/tasks")
            owners = remote.api_get("/api/page_owners")
            return {
                "status": status,
                "gdb": gdb,
                "tasks": tasks,
                "owners": owners,
            }

        def on_success(data: dict[str, Any]) -> None:
            self._set_json_text(self.overview_text, data)
            self._populate_tasks(data["tasks"].get("tasks", []))
            self._populate_owners(data["owners"].get("owners", []))
            self._append_log("[ok] refreshed")

        self._run_async("Refresh", worker, on_success)

    def pause_vm(self) -> None:
        self._run_async(
            "Pause VM",
            lambda: self._require_remote().api_post("/api/pause"),
            lambda data: self._append_log(json.dumps(data)),
        )

    def continue_vm(self) -> None:
        self._run_async(
            "Continue VM",
            lambda: self._require_remote().api_post("/api/continue"),
            lambda data: self._append_log(json.dumps(data)),
        )

    def break_and_snapshot(self) -> None:
        symbol = self.break_var.get().strip()

        def worker() -> Any:
            return self._require_remote().api_post(f"/api/gdb/break_and_snapshot?addr={symbol}&timeout=15")

        def on_success(data: Any) -> None:
            self._set_json_text(self.overview_text, data)
            self._populate_tasks(data.get("tasks", []))
            self._populate_owners(data.get("page_owners", []))
            self._append_log(f"[ok] breakpoint snapshot at {symbol}")

        self._run_async("Break and snapshot", worker, on_success)

    def walk_task(self) -> None:
        def worker() -> Any:
            task_idx = int(self.walk_task_var.get().strip())
            va = self.walk_va_var.get().strip()
            return self._require_remote().api_post("/api/walk_task", {"va": va, "task_idx": task_idx})

        self._run_async("Walk task VA", worker, lambda data: self._set_json_text(self.walk_text, data))

    def read_rawmem(self) -> None:
        def worker() -> Any:
            pa = self.raw_pa_var.get().strip()
            count = int(self.raw_count_var.get().strip() or "64")
            return self._require_remote().api_get(f"/api/debug/rawmem?pa={pa}&count={count}")

        self._run_async("Read raw memory", worker, lambda data: self._set_json_text(self.walk_text, data))

    def read_registers(self) -> None:
        self._run_async(
            "Read registers",
            lambda: self._require_remote().api_get("/api/gdb/registers"),
            lambda data: self._set_json_text(self.walk_text, data),
        )

    def _populate_tasks(self, tasks: list[dict[str, Any]]) -> None:
        for item in self.tasks_tree.get_children():
            self.tasks_tree.delete(item)
        for task in tasks:
            self.tasks_tree.insert(
                "",
                "end",
                values=(
                    task.get("idx"),
                    task.get("id"),
                    task.get("name"),
                    task.get("state"),
                    task.get("mm"),
                    task.get("ttbr0_root_pa"),
                ),
            )

    def _populate_owners(self, owners: list[dict[str, Any]]) -> None:
        for item in self.owners_tree.get_children():
            self.owners_tree.delete(item)
        for owner in owners:
            self.owners_tree.insert(
                "",
                "end",
                values=(
                    owner.get("task_idx"),
                    owner.get("task_name"),
                    owner.get("va"),
                    owner.get("pa"),
                    owner.get("ap"),
                    owner.get("exec"),
                ),
            )


def main() -> None:
    root = tk.Tk()
    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except tk.TclError:
        pass
    app = InspectorDesktopApp(root)
    del app
    root.mainloop()


if __name__ == "__main__":
    main()