"""The builder's window: pick the ROM (and the game language when the ROM
makes more than one), pick the SD card, Install."""

from __future__ import annotations

import queue
import tempfile
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from . import __version__
from .build import LANGUAGE_NAMES, Payload, build_pack, default_payload
from .errors import BuilderError
from .install import APP_DIR, find_sd_cards, install
from .rom import load_rom

TITLE = "Emerald 3DS Builder"


class App:
    def __init__(self, root: tk.Tk, payload: Payload):
        self.root = root
        self.payload = payload
        self.events: queue.Queue = queue.Queue()
        self.warnings: list[str] = []   # non-fatal, shown with the result (main thread only)
        self.busy = False
        root.title("%s %s" % (TITLE, __version__))
        root.resizable(False, False)
        frame = ttk.Frame(root, padding=16)
        frame.grid(sticky="nsew")

        ttk.Label(frame, text="Pokemon Emerald ROM (your own cartridge dump)").grid(
            row=0, column=0, columnspan=2, sticky="w")
        self.rom = tk.StringVar()
        ttk.Entry(frame, textvariable=self.rom, width=56).grid(row=1, column=0, sticky="we")
        ttk.Button(frame, text="Browse...", command=self.pick_rom).grid(row=1, column=1, padx=(8, 0))
        self.rom_status = ttk.Label(frame, text="")
        self.rom_status.grid(row=2, column=0, columnspan=2, sticky="w", pady=(2, 10))

        # The English ROM makes the English game and the Portuguese
        # translation: the player picks which (hidden for other ROMs).
        self.language_label = ttk.Label(frame, text="Game language")
        self.language_label.grid(row=3, column=0, sticky="w", pady=(0, 10))
        self.language = tk.StringVar()
        self.language_box = ttk.Combobox(frame, textvariable=self.language, state="readonly", width=30)
        self.language_box.grid(row=3, column=0, sticky="e", pady=(0, 10))
        self.language_codes: list[str] = []
        self.show_languages([])

        ttk.Label(frame, text="Install to (the root of your 3DS SD card)").grid(
            row=4, column=0, columnspan=2, sticky="w")
        self.sd = tk.StringVar()
        self.sd_box = ttk.Combobox(frame, textvariable=self.sd, width=53)
        self.sd_box.grid(row=5, column=0, sticky="we")
        ttk.Button(frame, text="Browse...", command=self.pick_sd).grid(row=5, column=1, padx=(8, 0))
        self.sd_status = ttk.Label(frame, text="")
        self.sd_status.grid(row=6, column=0, columnspan=2, sticky="w", pady=(2, 10))

        self.bar = ttk.Progressbar(frame, length=420, maximum=1000)
        self.bar.grid(row=7, column=0, columnspan=2, sticky="we")
        self.step = ttk.Label(frame, text="")
        self.step.grid(row=8, column=0, columnspan=2, sticky="w", pady=(2, 10))

        self.button = ttk.Button(frame, text="Install", command=self.start)
        self.button.grid(row=9, column=0, columnspan=2)
        ttk.Label(frame, text="The ROM never leaves this computer and is not copied; only the "
                              "generated data pack is written.", foreground="#555",
                  wraplength=460).grid(row=10, column=0, columnspan=2, pady=(12, 0))

        self.rom.trace_add("write", lambda *a: self.check_rom())
        self.sd.trace_add("write", lambda *a: self.check_sd())
        self.refresh_cards()
        root.after(100, self.poll)

    # -- inputs -------------------------------------------------------------
    def pick_rom(self):
        path = filedialog.askopenfilename(title="Pokemon Emerald ROM",
                                          filetypes=[("GBA ROM", "*.gba *.zip"), ("All files", "*.*")])
        if path:
            self.rom.set(path)

    def pick_sd(self):
        path = filedialog.askdirectory(title="SD card or output folder")
        if path:
            self.sd.set(path)

    def refresh_cards(self):
        cards = [str(c) for c in find_sd_cards()]
        self.sd_box["values"] = cards
        if cards and not self.sd.get():
            self.sd.set(cards[0])

    def check_rom(self):
        path = self.rom.get().strip()
        if not path:
            self.rom_status.config(text="", foreground="")
            return
        try:
            rom = load_rom(Path(path))
        except BuilderError as exc:
            self.rom_status.config(text="X  " + exc.message, foreground="#b00020")
            self.show_languages([])
            return
        self.rom_status.config(text="OK  %s - SHA-1 verified" % rom.title, foreground="#1b7a1b")
        self.show_languages(self.payload.languages(rom.sha1))

    def show_languages(self, codes: list[str]):
        """The game language choice, shown only when the ROM makes more than one."""
        self.language_codes = codes
        if len(codes) < 2:
            self.language.set("")
            self.language_label.grid_remove()
            self.language_box.grid_remove()
            return
        self.language_box["values"] = [LANGUAGE_NAMES.get(code, code) for code in codes]
        self.language_box.current(0)
        self.language_label.grid()
        self.language_box.grid()

    def chosen_language(self) -> str | None:
        index = self.language_box.current() if len(self.language_codes) > 1 else -1
        return self.language_codes[index] if index >= 0 else None

    def check_sd(self):
        path = Path(self.sd.get().strip()) if self.sd.get().strip() else None
        if path is None:
            self.sd_status.config(text="")
        elif (path / "Nintendo 3DS").is_dir():
            self.sd_status.config(text="OK  Nintendo 3DS SD card detected", foreground="#1b7a1b")
        elif path.is_dir():
            self.sd_status.config(text="This folder is not a 3DS SD card; the files will be written "
                                       "into it for you to copy.", foreground="#8a6d00")
        else:
            self.sd_status.config(text="X  Folder not found", foreground="#b00020")

    # -- work -----------------------------------------------------------------
    def start(self):
        if self.busy:
            return
        rom, sd = self.rom.get().strip(), self.sd.get().strip()
        if not rom or not sd:
            messagebox.showwarning(TITLE, "Choose the ROM and the destination first.")
            return
        dest = Path(sd) / APP_DIR
        if not messagebox.askokcancel(TITLE, "Emerald 3DS will be installed to:\n\n%s\n\nContinue?" % dest):
            return
        self.busy = True
        self.warnings = []
        self.button.state(["disabled"])
        threading.Thread(target=self.work, args=(Path(rom), Path(sd), self.chosen_language()),
                         daemon=True).start()

    def work(self, rom: Path, sd: Path, language: str | None):
        def progress(fraction, message):
            self.events.put(("progress", fraction, message))
        try:
            self.payload.check()
            with tempfile.TemporaryDirectory(prefix="emerald3ds-") as tmp:
                pak_path = Path(tmp) / "emerald3ds.pak"
                info = build_pack(rom, self.payload, pak_path, lambda f, m: progress(f * 0.9, m),
                                  need_executables=True, language=language)
                files = {exe.name: exe for exe in info["payload"].executables()}
                files["emerald3ds.pak"] = pak_path
                dest = install(sd, files, lambda f: progress(0.9 + 0.1 * f, "Copying to the SD card"),
                               warn=lambda message: self.events.put(("warning", message)))
            self.events.put(("done", str(dest)))
        except BuilderError as exc:
            self.events.put(("error", str(exc)))
        except Exception as exc:  # the player gets a message, never a traceback
            self.events.put(("error", "Unexpected error: %s" % exc))

    def poll(self):
        try:
            while True:
                event = self.events.get_nowait()
                if event[0] == "progress":
                    self.bar["value"] = int(event[1] * 1000)
                    self.step.config(text=event[2])
                elif event[0] == "warning":
                    self.warnings.append(event[1])
                elif event[0] == "done":
                    self.finish()
                    text = ("Done. Emerald 3DS is installed in\n%s\n\n"
                            "Start it from the Homebrew Launcher." % event[1])
                    if self.warnings:
                        messagebox.showwarning(TITLE, text + "\n\nWarning:\n" + "\n\n".join(self.warnings))
                    else:
                        messagebox.showinfo(TITLE, text)
                    self.warnings = []
                elif event[0] == "error":
                    self.finish()
                    messagebox.showerror(TITLE, event[1])
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def finish(self):
        self.busy = False
        self.button.state(["!disabled"])
        self.step.config(text="")
        self.bar["value"] = 0


def main(payload: Path | None = None) -> int:
    root = tk.Tk()
    App(root, Payload(payload or default_payload()))
    root.mainloop()
    return 0
