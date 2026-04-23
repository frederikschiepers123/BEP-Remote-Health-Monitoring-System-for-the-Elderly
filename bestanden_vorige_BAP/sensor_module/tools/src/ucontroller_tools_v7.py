#!/usr/bin/env python3
"""
flash_restore_gui_tabs_v4.py

Tabbed GUI with TWO tools:
  - Backup device (robust, file-by-file): copies MicroPython filesystem :/ -> local folder
  - Flash + Restore: esptool write_flash + mpremote restore folder -> :/

v4 fix:
- mpremote "fs ls" prints an echo line like: "ls :/" before listing entries.
  v2 parsed tokens naively, so it treated "ls" as a filename and tried to copy :/ls.
  v3 parses ls output line-by-line and ignores command/connection/status lines.

Build EXE (Windows):
  py -m pip install -U pyinstaller esptool mpremote pyserial
  py -m PyInstaller --onefile --windowed --name flash_restore_gui ^
      --hidden-import serial.tools.list_ports ^
      --collect-all esptool ^
      --collect-all mpremote ^
      flash_restore_gui_tabs_v4.py
"""

import io
import queue
import runpy
import sys
import threading
import time
from pathlib import Path

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from contextlib import redirect_stdout, redirect_stderr


# ------------------- logging to GUI -------------------
class _GuiWriter:
    """stdout/stderr sink compatible with libs that call isatty()/encoding."""
    def __init__(self, q: "queue.Queue[str]"):
        self.q = q

    def write(self, s):
        if not s:
            return
        if isinstance(s, (bytes, bytearray)):
            s = s.decode("utf-8", "replace")
        self.q.put(s)

    def flush(self):
        pass

    def isatty(self):
        return False

    @property
    def encoding(self):
        return "utf-8"


# ------------------- run module CLIs in-process (no "py -m") -------------------
def _run_module(mod: str, argv: list[str]) -> None:
    old_argv = sys.argv
    sys.argv = [mod] + argv
    try:
        try:
            runpy.run_module(mod, run_name="__main__")
        except SystemExit as e:
            code = e.code
            if code not in (0, None):
                raise RuntimeError(f"{mod} exited with code {code}") from None
    finally:
        sys.argv = old_argv


def run_esptool(args: list[str]) -> None:
    _run_module("esptool", args)


def run_mpremote(args: list[str]) -> None:
    _run_module("mpremote", args)


def mpremote_capture(args: list[str]) -> str:
    """Run mpremote and capture stdout+stderr text (for parsing)."""
    buf = io.StringIO()
    with redirect_stdout(buf), redirect_stderr(buf):
        run_mpremote(args)
    return buf.getvalue()


# ------------------- serial/REPL helpers -------------------
def ports_now() -> list[str]:
    try:
        from serial.tools import list_ports
        return [p.device for p in list_ports.comports()]
    except Exception:
        return []


def looks_like_prompt(buf: bytes) -> bool:
    return (b">>>" in buf) or (b"raw REPL; CTRL-B to exit" in buf)


def break_to_prompt(port: str, timeout_s: float = 2.0, baud: int = 115200) -> bool:
    """
    Best-effort: open port, send Ctrl-B (exit raw) and Ctrl-C (interrupt),
    and detect a MicroPython prompt.
    """
    try:
        import serial
        end = time.time() + timeout_s
        with serial.Serial(port, baudrate=baud, timeout=0.2) as s:
            s.reset_input_buffer()
            buf = b""
            s.write(b"\x02\x03\x03\r")
            time.sleep(0.15)
            while time.time() < end:
                s.write(b"\x03")
                time.sleep(0.10)
                buf += s.read(4096)
                if looks_like_prompt(buf):
                    return True
    except Exception:
        return False
    return False


def find_repl_port(prefer: str | None, timeout_s: float = 30.0) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        ps = ports_now()
        ordered: list[str] = []
        if prefer and prefer in ps:
            ordered.append(prefer)
        ordered += [p for p in ps if p != prefer]

        for p in ordered:
            if break_to_prompt(p, timeout_s=1.2):
                return p

        time.sleep(0.6)

    raise RuntimeError(
        "Could not find a MicroPython REPL port.\n\n"
        "Tips:\n"
        " • Make sure the board is NOT in download/bootloader mode (BOOT not held).\n"
        " • If you see two COM ports, try the other one (USB-CDC vs USB-JTAG).\n"
        " • Close Thonny/Serial Monitor so the port is free."
    )


# ------------------- esptool flash -------------------
def esptool_flash(port: str, firmware: Path, baud: int, erase: bool):
    base = ["--chip", "esp32s3", "--port", port, "--baud", str(baud)]
    if erase:
        try:
            run_esptool(base + ["erase-flash"])
        except RuntimeError:
            run_esptool(base + ["erase_flash"])
    run_esptool(base + ["write_flash", "0x0", str(firmware)])


# ------------------- mpremote filesystem operations -------------------
def mpremote_try(args: list[str]) -> bool:
    try:
        run_mpremote(args)
        return True
    except RuntimeError:
        return False


def restore_folder_to_device_root(repl_port: str, src_root: Path):
    """Copy *contents* of src_root into device root :/ (file-by-file; robust on Windows)."""
    base = ["connect", f"port:{repl_port}", "resume"]

    dirs: list[str] = []
    files: list[str] = []
    for p in src_root.rglob("*"):
        rel = p.relative_to(src_root).as_posix()
        if p.is_dir():
            dirs.append(rel)
        elif p.is_file():
            files.append(rel)

    dirs.sort(key=len)
    for d in dirs:
        mpremote_try(base + ["fs", "mkdir", f":/{d}"])

    for f in files:
        local_file = src_root / Path(f)
        run_mpremote(base + ["fs", "cp", str(local_file), f":/{f}"])


def _parse_ls_output(out: str, remote_dir: str) -> list[str]:
    # Normalize mpremote "fs ls" output into a list of entry names.
    #
    # mpremote may output:
    #   ls :/
    #   123 boot.py
    #   4096 lib/
    #   certs/  boot.py  main.py  (column format)
    #
    # We ignore echoed command and connection/status lines.
    # If a line starts with digits (size column), keep ONLY the last token (the name).
    entries: list[str] = []
    remote_dir_norm = remote_dir.strip()

    def ignore_line(line: str) -> bool:
        if not line:
            return True
        if line.startswith("ls "):
            return True
        if line.startswith("cp "):
            return True
        if line.startswith("Connected to MicroPython"):
            return True
        if line.startswith("Use Ctrl-") or line.startswith("raw REPL"):
            return True
        if line.startswith("mpremote:"):
            return True
        if line.startswith("Traceback") or line.startswith("File ") or line.startswith("ValueError") or line.startswith("OSError"):
            return True
        return False

    for raw in out.replace("\r", "\n").splitlines():
        line = raw.strip()
        if ignore_line(line):
            continue

        toks = [t for t in line.split() if t]
        if not toks:
            continue

        # Size+name format: "123 boot.py" or "4096 lib/"
        if toks[0].isdigit():
            if len(toks) >= 2:
                name = toks[-1]
                if name not in (".", "..", remote_dir_norm, remote_dir_norm.rstrip("/")):
                    entries.append(name)
            continue

        # Column format: "boot.py main.py lib/"
        for tok in toks:
            if tok in (".", ".."):
                continue
            if tok == remote_dir_norm or tok == remote_dir_norm.rstrip("/"):
                continue
            if tok.isdigit():
                continue
            entries.append(tok)

    # De-duplicate while preserving order
    seen = set()
    out_entries: list[str] = []
    for e in entries:
        if e not in seen:
            seen.add(e)
            out_entries.append(e)
    return out_entries


def _remote_ls(repl_port: str, remote_dir: str) -> list[str]:
    out = mpremote_capture(["connect", f"port:{repl_port}", "resume", "fs", "ls", remote_dir])
    return _parse_ls_output(out, remote_dir)


def backup_device_root_to_folder(repl_port: str, dest_folder: Path):
    """Robust backup that copies BOTH files and directories from :/."""
    dest_folder.mkdir(parents=True, exist_ok=True)

    def rec(remote_dir: str, local_dir: Path):
        local_dir.mkdir(parents=True, exist_ok=True)
        entries = _remote_ls(repl_port, remote_dir)
        if not entries:
            # Helpful for debugging: show raw output if needed
            pass

        for name in entries:
            if name.endswith("/"):
                sub = name[:-1]
                rec(f"{remote_dir.rstrip('/')}/{sub}", local_dir / sub)
            else:
                remote_file = f"{remote_dir.rstrip('/')}/{name}"
                local_file = local_dir / name
                run_mpremote(["connect", f"port:{repl_port}", "resume", "fs", "cp", remote_file, str(local_file)])

    rec(":/", dest_folder)



# ------------------- upload helpers (mpremote) -------------------
def _normalize_remote_path(p: str) -> str:
    p = (p or "").strip().replace("\\", "/")
    if not p:
        return ":/"
    if p == ":":
        return ":/"
    if p.startswith(":/"):
        pass
    elif p.startswith(":"):
        # ":lib" -> ":/lib"
        p = ":/" + p[1:].lstrip("/")
    else:
        # "lib" -> ":/lib"
        p = ":/" + p.lstrip("/")
    # collapse '//' (except keep ':/' prefix)
    while "//" in p[2:]:
        p = p[:2] + p[2:].replace("//", "/")
    if p == ":":
        p = ":/"
    return p


def _remote_entry_type(repl_port: str, remote_path: str) -> str | None:
    # Return 'dir', 'file', or None (not found).
    rp = _normalize_remote_path(remote_path)
    if rp in (":", ":/"):
        return "dir"

    parent, name = rp.rsplit("/", 1)
    if parent == ":":
        parent = ":/"
    if not name:
        return "dir"

    entries = _remote_ls(repl_port, parent)
    if f"{name}/" in entries:
        return "dir"
    if name in entries:
        return "file"
    return None


def _ensure_remote_dir(repl_port: str, remote_dir: str) -> None:
    rd = _normalize_remote_path(remote_dir)
    if rd in (":", ":/"):
        return
    # build incrementally: :/a, :/a/b, ...
    parts = [p for p in rd[2:].split("/") if p]
    cur = ":/"
    base = ["connect", f"port:{repl_port}", "resume"]
    for part in parts:
        cur = cur.rstrip("/") + "/" + part
        mpremote_try(base + ["fs", "mkdir", cur])


def _remove_remote_tree(repl_port: str, remote_path: str) -> None:
    rp = _normalize_remote_path(remote_path)
    base = ["connect", f"port:{repl_port}", "resume"]
    t = _remote_entry_type(repl_port, rp)
    if t is None:
        return
    if t == "file":
        mpremote_try(base + ["fs", "rm", rp])
        return

    # directory
    entries = _remote_ls(repl_port, rp)
    for e in entries:
        child = rp.rstrip("/") + "/" + e.rstrip("/")
        if e.endswith("/"):
            _remove_remote_tree(repl_port, child)
        else:
            mpremote_try(base + ["fs", "rm", child])

    # remove the dir itself (ignore errors for root)
    if rp not in (":", ":/"):
        mpremote_try(base + ["fs", "rmdir", rp])


def _clear_remote_dir_contents(repl_port: str, remote_dir: str) -> None:
    rd = _normalize_remote_path(remote_dir)
    if _remote_entry_type(repl_port, rd) != "dir":
        return
    entries = _remote_ls(repl_port, rd)
    for e in entries:
        child = rd.rstrip("/") + "/" + e.rstrip("/")
        _remove_remote_tree(repl_port, child)


def upload_file_to_device(repl_port: str, local_file: Path, remote_dest: str) -> None:
    # Upload a single file. Overwrites same-name files; removes conflicting dirs.
    rd = _normalize_remote_path(remote_dest)

    # If destination is a folder path, append filename
    if rd.endswith("/") or rd in (":", ":/") or _remote_entry_type(repl_port, rd) == "dir":
        remote_file = rd.rstrip("/") + "/" + local_file.name
    else:
        remote_file = rd

    parent = remote_file.rsplit("/", 1)[0] or ":/"
    _ensure_remote_dir(repl_port, parent)

    if _remote_entry_type(repl_port, remote_file) == "dir":
        _remove_remote_tree(repl_port, remote_file)

    run_mpremote(["connect", f"port:{repl_port}", "resume", "fs", "cp", str(local_file), remote_file])


def upload_folder_to_device(repl_port: str, src_root: Path, remote_dest: str, clear_first: bool = False) -> None:
    # Upload an entire folder tree into remote_dest. Overwrites files; removes conflicting dirs/files.
    rd = _normalize_remote_path(remote_dest).rstrip("/")
    if rd == ":":
        rd = ":/"
    if rd == "":
        rd = ":/"

    # Ensure destination is a directory
    t = _remote_entry_type(repl_port, rd)
    if t == "file":
        _remove_remote_tree(repl_port, rd)
        t = None
    _ensure_remote_dir(repl_port, rd)
    if t is None:
        mpremote_try(["connect", f"port:{repl_port}", "resume", "fs", "mkdir", rd])

    if clear_first:
        _clear_remote_dir_contents(repl_port, rd)

    # Collect dirs/files
    dirs: list[str] = []
    files: list[str] = []
    for p in src_root.rglob("*"):
        rel = p.relative_to(src_root).as_posix()
        if p.is_dir():
            dirs.append(rel)
        elif p.is_file():
            files.append(rel)

    dirs.sort(key=len)
    base = ["connect", f"port:{repl_port}", "resume"]

    for d in dirs:
        mpremote_try(base + ["fs", "mkdir", f"{rd}/{d}"])

    for f in files:
        local_path = src_root / Path(f)
        remote_path = f"{rd}/{f}"
        parent = remote_path.rsplit("/", 1)[0] or ":/"
        _ensure_remote_dir(repl_port, parent)

        if _remote_entry_type(repl_port, remote_path) == "dir":
            _remove_remote_tree(repl_port, remote_path)

        run_mpremote(base + ["fs", "cp", str(local_path), remote_path])


# ------------------- GUI -------------------
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("MicroPython Tools")
        self.geometry("940x700")

        self.log_q: "queue.Queue[str]" = queue.Queue()
        self._worker: threading.Thread | None = None

        self.last_backup_path = tk.StringVar(value="")

        self.backup_port = tk.StringVar(value="")
        self.backup_dest_base = tk.StringVar(value="")

        self.flash_port = tk.StringVar(value="")
        self.target_repl_port = tk.StringVar(value="")
        self.firmware_path = tk.StringVar(value="")
        self.restore_src_path = tk.StringVar(value="")

        # Upload tab
        self.upload_port = tk.StringVar(value="")
        self.upload_mode = tk.StringVar(value="folder")  # "file" or "folder"
        self.upload_src_path = tk.StringVar(value="")
        self.upload_remote_dest = tk.StringVar(value=":/")
        self.upload_clear_first = tk.BooleanVar(value=False)
        self.upload_reset_after = tk.BooleanVar(value=False)

        self.erase = tk.BooleanVar(value=True)
        self.reset_after = tk.BooleanVar(value=True)
        self.baud = tk.StringVar(value="460800")
        self.wait = tk.StringVar(value="30")
        self.do_flash = tk.BooleanVar(value=True)
        self.do_restore = tk.BooleanVar(value=True)

        self._build_ui()
        self._refresh_ports()
        self.after(80, self._poll_log_queue)

    def _build_ui(self):
        root = ttk.Frame(self, padding=12)
        root.pack(fill="both", expand=True)

        nb = ttk.Notebook(root)
        nb.pack(fill="x", pady=(0, 10))

        self.tab_backup = ttk.Frame(nb, padding=10)
        self.tab_flash = ttk.Frame(nb, padding=10)
        self.tab_upload = ttk.Frame(nb, padding=10)
        nb.add(self.tab_flash, text="Flash + Restore")
        nb.add(self.tab_backup, text="Backup from device")
        nb.add(self.tab_upload, text="Upload to device")

        self._build_backup_tab(self.tab_backup)
        self._build_flash_tab(self.tab_flash)
        self._build_upload_tab(self.tab_upload)

        logf = ttk.LabelFrame(root, text="Log", padding=10)
        logf.pack(fill="both", expand=True)
        self.txt = tk.Text(logf, height=18, wrap="word")
        self.txt.pack(fill="both", expand=True)

    def _build_backup_tab(self, parent: ttk.Frame):
        ports = ttk.LabelFrame(parent, text="Source device", padding=10)
        ports.pack(fill="x", pady=(0, 10))

        ttk.Label(ports, text="REPL port:").grid(row=0, column=0, sticky="w")
        self.cb_backup = ttk.Combobox(ports, textvariable=self.backup_port, width=14, state="readonly")
        self.cb_backup.grid(row=0, column=1, sticky="w", padx=(8, 18))

        ttk.Button(ports, text="Refresh ports", command=self._refresh_ports).grid(row=0, column=2, sticky="w")

        dest = ttk.LabelFrame(parent, text="Backup destination", padding=10)
        dest.pack(fill="x", pady=(0, 10))
        ttk.Label(dest, text="Base folder:").grid(row=0, column=0, sticky="w")
        ttk.Entry(dest, textvariable=self.backup_dest_base).grid(row=0, column=1, sticky="ew", padx=(8, 8))
        ttk.Button(dest, text="Browse…", command=self._browse_backup_dest).grid(row=0, column=2, sticky="w")
        dest.columnconfigure(1, weight=1)

        res = ttk.Frame(parent)
        res.pack(fill="x", pady=(0, 10))
        ttk.Label(res, text="Last backup folder:").pack(side="left")
        ttk.Entry(res, textvariable=self.last_backup_path, state="readonly").pack(side="left", fill="x", expand=True, padx=8)
        ttk.Button(res, text="Use for restore", command=self._use_last_backup_for_restore).pack(side="left")

        actions = ttk.Frame(parent)
        actions.pack(fill="x")
        self.btn_backup = ttk.Button(actions, text="Run Backup", command=self._start_backup)
        self.btn_backup.pack(side="left")
        ttk.Button(actions, text="Clear log", command=self._clear_log).pack(side="left", padx=10)

        ttk.Label(parent, text="Tip: Close Thonny/serial monitors first so the COM port is free.").pack(anchor="w", pady=(10, 0))

    def _build_flash_tab(self, parent: ttk.Frame):
        ports = ttk.LabelFrame(parent, text="Target device ports", padding=10)
        ports.pack(fill="x", pady=(0, 10))

        ttk.Label(ports, text="Flash port (esptool):").grid(row=0, column=0, sticky="w")
        self.cb_flash = ttk.Combobox(ports, textvariable=self.flash_port, width=14, state="readonly")
        self.cb_flash.grid(row=0, column=1, sticky="w", padx=(8, 18))

        ttk.Label(ports, text="REPL port (optional):").grid(row=0, column=2, sticky="w")
        self.cb_repl = ttk.Combobox(ports, textvariable=self.target_repl_port, width=14, state="readonly")
        self.cb_repl.grid(row=0, column=3, sticky="w", padx=(8, 18))

        ttk.Button(ports, text="Refresh ports", command=self._refresh_ports).grid(row=0, column=4, sticky="w")

        files = ttk.LabelFrame(parent, text="Files", padding=10)
        files.pack(fill="x", pady=(0, 10))

        ttk.Label(files, text="Firmware .bin:").grid(row=0, column=0, sticky="w")
        ttk.Entry(files, textvariable=self.firmware_path).grid(row=0, column=1, sticky="ew", padx=(8, 8))
        ttk.Button(files, text="Browse…", command=self._browse_firmware).grid(row=0, column=2, sticky="w")

        ttk.Label(files, text="Restore folder:").grid(row=1, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(files, textvariable=self.restore_src_path).grid(row=1, column=1, sticky="ew", padx=(8, 8), pady=(8, 0))
        ttk.Button(files, text="Browse…", command=self._browse_restore_src).grid(row=1, column=2, sticky="w", pady=(8, 0))
        ttk.Button(files, text="Use last backup", command=self._use_last_backup_for_restore).grid(row=1, column=3, sticky="w", pady=(8, 0))

        files.columnconfigure(1, weight=1)

        opts = ttk.LabelFrame(parent, text="Options", padding=10)
        opts.pack(fill="x", pady=(0, 10))

        ttk.Checkbutton(opts, text="Flash firmware", variable=self.do_flash).grid(row=0, column=0, sticky="w")
        ttk.Checkbutton(opts, text="Erase flash before writing", variable=self.erase).grid(row=0, column=1, sticky="w", padx=(16, 0))

        ttk.Checkbutton(opts, text="Restore files after flash", variable=self.do_restore).grid(row=1, column=0, sticky="w", pady=(6, 0))
        ttk.Checkbutton(opts, text="Reset after restore", variable=self.reset_after).grid(row=1, column=1, sticky="w", padx=(16, 0), pady=(6, 0))

        ttk.Label(opts, text="esptool baud:").grid(row=2, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(opts, textvariable=self.baud, width=10).grid(row=2, column=0, sticky="w", padx=(92, 0), pady=(8, 0))

        ttk.Label(opts, text="REPL detect wait (s):").grid(row=2, column=1, sticky="w", pady=(8, 0))
        ttk.Entry(opts, textvariable=self.wait, width=10).grid(row=2, column=1, sticky="w", padx=(150, 0), pady=(8, 0))

        actions = ttk.Frame(parent)
        actions.pack(fill="x")
        self.btn_run = ttk.Button(actions, text="Run Flash + Restore", command=self._start_flash_restore)
        self.btn_run.pack(side="left")
        ttk.Button(actions, text="Clear log", command=self._clear_log).pack(side="left", padx=10)

        ttk.Label(parent, text="Tip: If flashing fails, hold BOOT and tap RESET to enter download mode.").pack(anchor="w", pady=(10, 0))


    def _build_upload_tab(self, parent: ttk.Frame):
        ports = ttk.LabelFrame(parent, text="Target device", padding=10)
        ports.pack(fill="x", pady=(0, 10))

        ttk.Label(ports, text="REPL port:").grid(row=0, column=0, sticky="w")
        self.cb_upload = ttk.Combobox(ports, textvariable=self.upload_port, width=14, state="readonly")
        self.cb_upload.grid(row=0, column=1, sticky="w", padx=(8, 18))
        ttk.Button(ports, text="Refresh ports", command=self._refresh_ports).grid(row=0, column=2, sticky="w")

        srcf = ttk.LabelFrame(parent, text="What to upload", padding=10)
        srcf.pack(fill="x", pady=(0, 10))

        ttk.Radiobutton(srcf, text="Single file", variable=self.upload_mode, value="file").grid(row=0, column=0, sticky="w")
        ttk.Radiobutton(srcf, text="Folder", variable=self.upload_mode, value="folder").grid(row=0, column=1, sticky="w", padx=(16, 0))

        ttk.Label(srcf, text="Source path:").grid(row=1, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(srcf, textvariable=self.upload_src_path).grid(row=1, column=1, sticky="ew", padx=(8, 8), pady=(8, 0))
        ttk.Button(srcf, text="Browse…", command=self._browse_upload_src).grid(row=1, column=2, sticky="w", pady=(8, 0))
        srcf.columnconfigure(1, weight=1)

        destf = ttk.LabelFrame(parent, text="Destination on device", padding=10)
        destf.pack(fill="x", pady=(0, 10))
        ttk.Label(destf, text="Remote path (folder or file):").grid(row=0, column=0, sticky="w")
        ttk.Entry(destf, textvariable=self.upload_remote_dest).grid(row=0, column=1, sticky="ew", padx=(8, 8))
        destf.columnconfigure(1, weight=1)

        opts = ttk.LabelFrame(parent, text="Options", padding=10)
        opts.pack(fill="x", pady=(0, 10))
        ttk.Checkbutton(opts, text="Clear destination folder first (folder upload only)", variable=self.upload_clear_first).grid(row=0, column=0, sticky="w")
        ttk.Checkbutton(opts, text="Reset after upload", variable=self.upload_reset_after).grid(row=1, column=0, sticky="w", pady=(6, 0))

        actions = ttk.Frame(parent)
        actions.pack(fill="x")
        self.btn_upload = ttk.Button(actions, text="Upload to device", command=self._start_upload)
        self.btn_upload.pack(side="left")
        ttk.Button(actions, text="Clear log", command=self._clear_log).pack(side="left", padx=10)

        ttk.Label(
            parent,
            text="Remote path examples: :/ (root), :/lib, :/main.py. "
                 "Single-file uploads: if the remote path is a folder, the filename is kept. "
                 "Existing files with the same name are overwritten.",
        ).pack(anchor="w", pady=(10, 0))

    def _browse_upload_src(self):
        mode = self.upload_mode.get()
        if mode == "file":
            p = filedialog.askopenfilename(title="Select file to upload", filetypes=[("All files", "*.*")])
        else:
            p = filedialog.askdirectory(title="Select folder to upload")
        if p:
            self.upload_src_path.set(p)

    def _start_upload(self):
        if self._busy():
            messagebox.showinfo("Busy", "A job is already running.")
            return

        port = self.upload_port.get().strip()
        if not port:
            messagebox.showerror("Missing", "Select a REPL port.")
            return

        srcp = self.upload_src_path.get().strip()
        if not srcp:
            messagebox.showerror("Missing", "Select a source file or folder.")
            return
        src_path = Path(srcp).expanduser().resolve()

        mode = self.upload_mode.get()
        if mode == "file" and (not src_path.exists() or not src_path.is_file()):
            messagebox.showerror("Invalid", "Source must be a file.")
            return
        if mode == "folder" and (not src_path.exists() or not src_path.is_dir()):
            messagebox.showerror("Invalid", "Source must be a folder.")
            return

        remote_dest = self.upload_remote_dest.get().strip() or ":/"
        clear_first = bool(self.upload_clear_first.get()) and (mode == "folder")
        reset_after = bool(self.upload_reset_after.get())

        wait_s = float(self.wait.get().strip() or "30")

        self._clear_log()
        self._lock_buttons(True)
        self._worker = threading.Thread(
            target=self._run_upload_job,
            args=(port, src_path, mode, remote_dest, clear_first, reset_after, wait_s),
            daemon=True,
        )
        self._worker.start()

    def _run_upload_job(self, port_pref: str, src_path: Path, mode: str, remote_dest: str,
                        clear_first: bool, reset_after: bool, wait_s: float):
        old_out, old_err = sys.stdout, sys.stderr
        writer = _GuiWriter(self.log_q)
        sys.stdout = writer
        sys.stderr = writer

        def ui_error(msg: str):
            self.after(0, lambda: messagebox.showerror("Error", msg))

        try:
            print("== Upload: locating REPL port ==")
            repl = find_repl_port(port_pref, timeout_s=wait_s)
            print(f"Using REPL port: {repl}")
            break_to_prompt(repl, timeout_s=2.5)

            if mode == "file":
                print(f"== Uploading file: {src_path} ==")
                upload_file_to_device(repl, src_path, remote_dest)
            else:
                print(f"== Uploading folder: {src_path} ==")
                if clear_first:
                    print(f"Clearing destination folder first: {remote_dest}")
                upload_folder_to_device(repl, src_path, remote_dest, clear_first=clear_first)

            if reset_after:
                print("== Resetting device ==")
                run_mpremote(["connect", f"port:{repl}", "reset"])

            print("Upload completed.")
        except Exception as e:
            ui_error(str(e))
        finally:
            sys.stdout = old_out
            sys.stderr = old_err
            self.after(0, lambda: self._lock_buttons(False))

    def _clear_log(self):
        self.txt.delete("1.0", "end")

    def _poll_log_queue(self):
        try:
            while True:
                s = self.log_q.get_nowait()
                self.txt.insert("end", s)
                self.txt.see("end")
        except queue.Empty:
            pass
        self.after(80, self._poll_log_queue)

    def _refresh_ports(self):
        ps = ports_now()
        values = [""] + ps

        # Update comboboxes if they exist
        for cb_name in ("cb_backup", "cb_flash", "cb_repl", "cb_upload"):
            cb = getattr(self, cb_name, None)
            if cb is not None:
                try:
                    cb["values"] = values
                except Exception:
                    pass

        # Pick a default port if none selected yet
        if ps:
            if hasattr(self, "backup_port") and not self.backup_port.get():
                self.backup_port.set(ps[0])
            if hasattr(self, "flash_port") and not self.flash_port.get():
                self.flash_port.set(ps[0])
            if hasattr(self, "upload_port") and not self.upload_port.get():
                self.upload_port.set(ps[0])

    def _browse_backup_dest(self):
        p = filedialog.askdirectory(title="Select base folder for backups")
        if p:
            self.backup_dest_base.set(p)

    def _browse_firmware(self):
        p = filedialog.askopenfilename(
            title="Select firmware .bin",
            filetypes=[("Firmware bin", "*.bin"), ("All files", "*.*")]
        )
        if p:
            self.firmware_path.set(p)

    def _browse_restore_src(self):
        p = filedialog.askdirectory(title="Select restore source folder")
        if p:
            self.restore_src_path.set(p)

    def _use_last_backup_for_restore(self):
        p = self.last_backup_path.get().strip()
        if p:
            self.restore_src_path.set(p)
        else:
            messagebox.showinfo("No backup yet", "Run a backup first (Backup device tab).")

    def _busy(self) -> bool:
        return bool(self._worker and self._worker.is_alive())

    def _lock_buttons(self, locked: bool):
        state = "disabled" if locked else "normal"
        self.btn_backup.configure(state=state)
        self.btn_run.configure(state=state)
        if hasattr(self, "btn_upload"):
            self.btn_upload.configure(state=state)

    def _start_backup(self):
        if self._busy():
            messagebox.showinfo("Busy", "A job is already running.")
            return

        port = self.backup_port.get().strip()
        if not port:
            messagebox.showerror("Missing", "Select a REPL port.")
            return

        base = self.backup_dest_base.get().strip()
        if not base:
            messagebox.showerror("Missing", "Select a backup destination base folder.")
            return
        basep = Path(base).expanduser().resolve()

        wait_s = float(self.wait.get().strip() or "30")

        self._clear_log()
        self._lock_buttons(True)
        self._worker = threading.Thread(target=self._run_backup_job, args=(port, basep, wait_s), daemon=True)
        self._worker.start()

    def _run_backup_job(self, port_pref: str, basep: Path, wait_s: float):
        old_out, old_err = sys.stdout, sys.stderr
        writer = _GuiWriter(self.log_q)
        sys.stdout = writer
        sys.stderr = writer

        def ui_error(msg: str):
            self.after(0, lambda: messagebox.showerror("Error", msg))

        try:
            print("== Backup: locating REPL port ==")
            repl = find_repl_port(port_pref, timeout_s=wait_s)
            print(f"Using REPL port: {repl}")

            ts = time.strftime("%Y%m%d_%H%M%S")
            outdir = basep / f"backup_{repl}_{ts}"
            print(f"== Copying :/ (files + folders) to {outdir} ==")
            break_to_prompt(repl, timeout_s=2.5)
            backup_device_root_to_folder(repl, outdir)
            print("Backup completed.")

            self.after(0, lambda: self.last_backup_path.set(str(outdir)))
        except Exception as e:
            ui_error(str(e))
        finally:
            sys.stdout = old_out
            sys.stderr = old_err
            self.after(0, lambda: self._lock_buttons(False))

    def _start_flash_restore(self):
        if self._busy():
            messagebox.showinfo("Busy", "A job is already running.")
            return

        do_flash = self.do_flash.get()
        do_restore = self.do_restore.get()

        flash_port = self.flash_port.get().strip()
        repl_pref = self.target_repl_port.get().strip() or None

        if do_flash and not flash_port:
            messagebox.showerror("Missing", "Select a flash port (esptool).")
            return

        fw = self.firmware_path.get().strip()
        firmware = Path(fw).expanduser().resolve() if fw else None
        if do_flash and (not firmware or not firmware.exists()):
            messagebox.showerror("Missing", "Select a valid firmware .bin file.")
            return

        srcp = self.restore_src_path.get().strip()
        restore_src = Path(srcp).expanduser().resolve() if srcp else None
        if do_restore and (not restore_src or not restore_src.exists() or not restore_src.is_dir()):
            messagebox.showerror("Missing", "Select a valid restore folder (or click 'Use last backup').")
            return

        try:
            baud = int(self.baud.get().strip() or "460800")
            wait_s = float(self.wait.get().strip() or "30")
        except Exception:
            messagebox.showerror("Invalid", "Baud and wait must be numbers.")
            return

        self._clear_log()
        self._lock_buttons(True)
        self._worker = threading.Thread(
            target=self._run_flash_restore_job,
            args=(do_flash, do_restore, flash_port, repl_pref, firmware, restore_src, baud, wait_s, self.erase.get(), self.reset_after.get()),
            daemon=True,
        )
        self._worker.start()

    def _run_flash_restore_job(self, do_flash: bool, do_restore: bool, flash_port: str, repl_pref: str | None,
                              firmware: Path | None, restore_src: Path | None, baud: int, wait_s: float,
                              erase: bool, reset_after: bool):
        old_out, old_err = sys.stdout, sys.stderr
        writer = _GuiWriter(self.log_q)
        sys.stdout = writer
        sys.stderr = writer

        def ui_error(msg: str):
            self.after(0, lambda: messagebox.showerror("Error", msg))

        try:
            if do_flash:
                assert firmware is not None
                print("== Flashing firmware ==")
                try:
                    esptool_flash(flash_port, firmware, baud, erase)
                except Exception:
                    print("\nFlashing failed.")
                    print("Tip: Put the board in download mode: hold BOOT, tap RESET, then retry.")
                    raise

            if do_restore:
                assert restore_src is not None
                print("\n== Finding MicroPython REPL port ==")
                time.sleep(2)
                pref = repl_pref or (flash_port if flash_port else "")
                repl = find_repl_port(pref, timeout_s=wait_s)
                print(f"Using REPL port: {repl}")

                print("\n== Restoring filesystem to device root (:/) ==")
                restore_folder_to_device_root(repl, restore_src)

                if reset_after:
                    print("\n== Resetting device ==")
                    run_mpremote(["connect", f"port:{repl}", "reset"])

            print("\nDone.")
        except Exception as e:
            ui_error(str(e))
        finally:
            sys.stdout = old_out
            sys.stderr = old_err
            self.after(0, lambda: self._lock_buttons(False))


def main():
    App().mainloop()


if __name__ == "__main__":
    main()
