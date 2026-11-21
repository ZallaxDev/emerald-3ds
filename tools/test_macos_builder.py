"""Build, audit and run a native macOS builder using a synthetic payload."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from build_release import GENERATORS, PORT, bundle_builder, local_closure, write_builder_zip
from build_web_payload import make_synthetic
from release_audit import main as audit


def main() -> None:
    if sys.platform != "darwin":
        raise SystemExit("Run this smoke test on macOS")
    with tempfile.TemporaryDirectory(prefix="emerald3ds-macos-") as tmp:
        root = Path(tmp)
        fixture = make_synthetic(root / "fixture")
        release = root / "Emerald3DS-v0.0.0-macOS"
        shutil.copytree(fixture["payload"], release / "payload")
        _, external = local_closure(PORT / "scripts", GENERATORS)
        bundle_builder(release, external, "macOS")
        archive = root / (release.name + ".zip")
        write_builder_zip(release, archive, "macOS")
        if audit(["--zip", str(archive), "--strict", "--rom", str(fixture["rom"])]):
            raise SystemExit("The macOS archive failed the strict audit")

        extracted = root / "extracted"
        subprocess.run(["/usr/bin/unzip", "-q", archive, "-d", extracted], check=True)
        installed = extracted / release.name
        for link in (path for path in release.rglob("*") if path.is_symlink()):
            copied = installed / link.relative_to(release)
            if not copied.is_symlink() or copied.readlink() != link.readlink() or not copied.exists():
                raise SystemExit(f"Runtime symlink did not survive extraction: {link.name}")

        probe = root / "check_builder.py"
        probe.write_text(
            "import tkinter\n"
            "from PIL import Image\n"
            "from emerald3ds_builder.build import default_payload\n"
            "assert tkinter.Tcl().eval('info patchlevel')\n"
            "assert Image.new('RGB', (1, 1)).tobytes() == bytes(3)\n"
            "assert (default_payload() / 'emerald3ds.recipe').is_file()\n",
            encoding="utf-8",
        )
        subprocess.run([installed / "emerald3ds-builder-cli", "--help"], cwd=root, check=True)
        for exe in ("emerald3ds-builder-cli", "Emerald3DS-Builder"):
            subprocess.run([installed / exe, "--run-script", probe], cwd=root, check=True)
        print("The extracted macOS builders found their payload and loaded Tcl and Pillow.")


if __name__ == "__main__":
    main()
