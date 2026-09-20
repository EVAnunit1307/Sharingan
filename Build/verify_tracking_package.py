"""Verify the actual Quest APK, including configuration and cooked IoStore assets.

Uses Python's standard library, Android aapt, and UE 5.7 UnrealPak. No headset,
editor, network, build, or installation is needed. See PACKAGE_VERIFICATION.md.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from zipfile import BadZipFile, ZipFile


PROJECT = Path(__file__).resolve().parents[1]
PACKAGE = "com.wallhack.questhud.greentest"
LABEL = "Wallhack Green Test"
RENDERER = "/Script/Engine.RendererSettings"
ANCHOR_SETTINGS = "/Script/OculusXRHMD.OculusXRHMDRuntimeSettings"
PAK_ROOT = "HandoffQuestHUD/Content/Paks/"
PAK_NAME = "HandoffQuestHUD-Android_ASTC"


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()


def ini_values(text: str, section: str, key: str) -> list[str]:
    """Handle UE's repeated sections without dumping unrelated config values."""
    active = ""
    values = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            active = line[1:-1].casefold()
        elif active == section.casefold() and "=" in line:
            name, value = line.split("=", 1)
            if name.strip().casefold() == key.casefold():
                values.append(value.strip())
    return values


def metadata_values(xmltree: str, name: str) -> list[int | str]:
    values = []
    blocks = re.split(r"(?m)^\s*E: meta-data[^\n]*\n", xmltree)[1:]
    for block in blocks:
        # Metadata has no child elements; stop before the next element.
        block = re.split(r"(?m)^\s*E:", block, maxsplit=1)[0]
        if f'="{name}"' not in block:
            continue
        integer = re.search(r"A: android:value[^=]*=\(type 0x1[012]\)0x([0-9a-fA-F]+)", block)
        quoted = re.search(r'A: android:value[^=]*="([^"]*)"', block)
        values.append(int(integer[1], 16) if integer else quoted[1] if quoted else "unreadable")
    return values


def run_tool(executable: Path, arguments: list[str], cwd: Path, timeout: int = 90) -> str:
    # A list bypasses shell interpolation, including for paths with spaces.
    result = subprocess.run(
        [str(executable), *arguments], cwd=cwd, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=timeout,
    )
    if result.returncode:
        # Keep private/config contents out of output. Temporary raw logs vanish.
        raise RuntimeError(f"{executable.name} returned {result.returncode}; verify its path and that the APK/container is readable and unencrypted")
    return result.stdout + result.stderr


def copy_entry(archive: ZipFile, name: str, destination: Path) -> None:
    # Only exact known entries are copied; archive paths never choose a target.
    with archive.open(name) as source, destination.open("wb") as target:
        shutil.copyfileobj(source, target)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--navigation", action="store_true", help="Verify the fully cooked navigation build and its scene permission/material")
    parser.add_argument("--apk", type=Path, default=Path.home() / "Downloads/Quest2Setup/WallhackSpatialTracking.apk")
    sdk = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData/Local"))) / "Android/Sdk"
    parser.add_argument("--aapt", type=Path, default=sdk / "build-tools/35.0.1/aapt.exe")
    parser.add_argument("--unrealpak", type=Path, default=Path("C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealPak.exe"))
    parser.add_argument("--report", type=Path, default=PROJECT / "Saved/TrackingVerification/package-verification.json")
    args = parser.parse_args()
    expected_package = "com.wallhack.questhud.navigation" if args.navigation else PACKAGE
    expected_label = "Wallhack Navigation" if args.navigation else LABEL
    for argument in ("apk", "aapt", "unrealpak", "report"):
        setattr(args, argument, getattr(args, argument).resolve())
    if args.report in (args.apk, args.aapt, args.unrealpak):
        parser.error("--report must not overwrite an input APK or tool")
    checks: list[dict] = []
    report = {
        "checked_at_utc": datetime.now(timezone.utc).isoformat(),
        "apk": str(args.apk), "checks": checks,
        "scope": "APK metadata, archive integrity, packaged configuration and cooked asset presence",
        "limitations": [
            "Does not establish headset stereo visibility, physical anchoring, controller behavior or performance.",
            "Cooked asset presence does not establish material shader output or rendered appearance.",
            "Does not establish signing-certificate compatibility or that native code matches the latest source.",
        ],
    }

    def check(name: str, passed: bool, evidence: str, fix: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "evidence": evidence, **({} if passed else {"fix": fix})})
        print(f"{'PASS' if passed else 'FAIL'} {name}: {evidence}")
        if not passed:
            print(f"  Fix: {fix}")

    try:
        for name, path in (("APK", args.apk), ("aapt", args.aapt), ("UnrealPak", args.unrealpak)):
            if not path.is_file():
                raise RuntimeError(f"{name} is missing at {path}; supply its path with the matching command-line option")
        before = args.apk.stat()
        report["apk_sha256"] = sha256(args.apk)
        report["apk_size_bytes"] = before.st_size
        report["apk_modified_utc"] = datetime.fromtimestamp(before.st_mtime, timezone.utc).isoformat()
        with tempfile.TemporaryDirectory(prefix="wallhack-package-") as temp:
            work = Path(temp).resolve()
            badging = run_tool(args.aapt, ["dump", "badging", str(args.apk)], work)
            xmltree = run_tool(args.aapt, ["dump", "xmltree", str(args.apk), "AndroidManifest.xml"], work)
            package = re.search(r"(?m)^package: name='([^']+)'", badging)
            label = re.search(r"(?m)^application-label:'([^']*)'", badging)
            check("Package identity", bool(package and package[1] == expected_package), package[1] if package else "missing",
                  f"Set GreenTestGradle app applicationId to {PACKAGE}; do not change its Java namespace.")
            check("App label", bool(label and label[1] == expected_label), label[1] if label else "missing",
                  f"Restore app_name to {LABEL} in the Green Test Gradle workspace.")
            alpha = metadata_values(xmltree, "com.epicgames.unreal.GameActivity.PropagateAlpha")
            check("Android scene alpha", alpha == [1], f"manifest PropagateAlpha={alpha}",
                  "Set r.Mobile.PropagateAlpha=1 in [/Script/Engine.RendererSettings], run Unreal packaging, and refresh the GreenTestGradle manifest before assembly.")
            permission = "uses-permission: name='com.oculus.permission.USE_ANCHOR_API'" in badging
            if args.navigation:
                check("Scene and depth permission", "uses-permission: name='com.oculus.permission.USE_SCENE'" in badging,
                      "USE_SCENE declaration checked", "Enable scene support and fully repackage the application.")
            check("Anchor API permission", permission, "declared" if permission else "missing",
                  "Enable bAnchorSupportEnabled, run Unreal packaging, and refresh the GreenTestGradle manifest.")
            check("Arm64 ABI", "native-code: 'arm64-v8a'" in badging, "arm64-v8a" if "native-code: 'arm64-v8a'" in badging else "missing or unexpected ABI",
                  "Refresh the arm64-v8a libUnreal.so in GreenTestGradle and assemble again.")

            with ZipFile(args.apk) as apk:
                bad_entry = apk.testzip()
                check("APK ZIP integrity", bad_entry is None, "all entry CRCs valid" if bad_entry is None else f"bad entry: {bad_entry}",
                      "Reassemble the APK; do not verify a file while it is still being copied.")
                with apk.open("lib/arm64-v8a/libUnreal.so") as native:
                    header = native.read(64)
                with apk.open("lib/arm64-v8a/libUnreal.so") as native:
                    report["native_sha256"] = hashlib.file_digest(native, "sha256").hexdigest().upper()
                elf_ok = len(header) == 64 and header[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", header, 18)[0] == 183
                check("Native library architecture", elf_ok, "64-bit little-endian AArch64 ELF" if elf_ok else "invalid ELF or architecture",
                      "Package the Android arm64 libUnreal.so, not a desktop library.")
                obb_path = work / "main.obb.png"
                copy_entry(apk, "assets/main.obb.png", obb_path)

            with ZipFile(obb_path) as obb:
                bad_entry = obb.testzip()
                check("Embedded OBB integrity", bad_entry is None, "all entry CRCs valid" if bad_entry is None else f"bad entry: {bad_entry}",
                      "Refresh assets/main.obb.png from a successful Unreal packaging run.")
                for extension in ("pak", "utoc", "ucas"):
                    copy_entry(obb, PAK_ROOT + f"{PAK_NAME}.{extension}", work / f"{PAK_NAME}.{extension}")

            pak = work / f"{PAK_NAME}.pak"
            listing = run_tool(args.unrealpak, [str(pak), "-List", f"-abslog={work / 'pak-list.log'}"], work)
            entry_names = re.findall(r'(?m)^LogPakFile: Display: "([^"]+)" offset:', listing)
            # Validate paths before delegating extraction to UnrealPak.
            paths_safe = all(not Path(name).is_absolute() and ".." not in name.replace("\\", "/").split("/") and ":" not in name for name in entry_names)
            if not entry_names or not paths_safe:
                raise RuntimeError("Pak index is missing or contains unsafe paths; inspect the container before extraction")
            config_entry = "HandoffQuestHUD/Config/DefaultEngine.ini"
            if config_entry not in entry_names:
                raise RuntimeError("Packaged DefaultEngine.ini is missing; recook/package the project and refresh the embedded OBB")
            extract = work / "config"
            extract.mkdir()
            run_tool(args.unrealpak, [str(pak), "-Extract", str(extract), f"-Filter={config_entry}", f"-abslog={work / 'pak-extract.log'}"], work)
            configs = list(extract.rglob("DefaultEngine.ini"))
            if len(configs) != 1:
                raise RuntimeError("Could not extract exactly one packaged DefaultEngine.ini; check UnrealPak compatibility")
            configuration = configs[0].read_text(encoding="utf-8-sig")
            if args.navigation:
                values = ini_values(configuration, ANCHOR_SETTINGS, "bSceneSupportEnabled")
                check("Packaged scene support", bool(values) and all(v.casefold() == "true" for v in values),
                      f"bSceneSupportEnabled={values}", "Recook with scene support enabled.")
            for name, section, key, expected in (
                *(([
                    ("Packaged mobile forward renderer", RENDERER, "r.Mobile.ShadingPath", "0"),
                    ("Packaged mobile HDR disabled", RENDERER, "r.MobileHDR", "false"),
                    ("Packaged mobile MSAA", RENDERER, "r.Mobile.AntiAliasing", "3"),
                    ("Packaged mobile multiview", "SystemSettings", "vr.MobileMultiView", "1"),
                ]) if args.navigation else []),
                ("Packaged mobile alpha", RENDERER, "r.Mobile.PropagateAlpha", "1"),
                ("Packaged passthrough alpha", "SystemSettings", "r.PostProcessing.PropagateAlpha", "1"),
                ("Packaged anchor support", ANCHOR_SETTINGS, "bAnchorSupportEnabled", "true"),
                ("Packaged passthrough support", ANCHOR_SETTINGS, "bInsightPassthroughEnabled", "true"),
            ):
                values = ini_values(configuration, section, key)
                check(name, bool(values) and all(value.casefold() == expected for value in values), f"[{section}] {key}={values}",
                      "Correct the project configuration, then recook/package and refresh GreenTestGradle assets/main.obb.png; changing only local Config or the manifest is insufficient.")

            container_csv = work / "container.csv"
            run_tool(args.unrealpak, [f"-ListContainer={work / (PAK_NAME + '.utoc')}", f"-Csv={container_csv}", f"-abslog={work / 'container.log'}"], work)
            with container_csv.open(encoding="utf-8-sig", newline="") as stream:
                rows = [{key.strip(): value.strip() for key, value in row.items()} for row in csv.DictReader(stream, skipinitialspace=True)]
            for name, filename in (
                *(([("Cooked navigation trail material", "../../../HandoffQuestHUD/Content/Materials/M_WallhackTrail.uasset")]) if args.navigation else []),
                *(([("Cooked human mesh", "../../../HandoffQuestHUD/Content/People/SM_HumanSilhouette.uasset"),
                    ("Cooked human material", "../../../HandoffQuestHUD/Content/Materials/M_HumanSilhouette.uasset"),
                    ("Cooked person label material", "../../../HandoffQuestHUD/Content/Materials/M_PersonLabel.uasset"),
                    ("Cooked linear label default", "../../../HandoffQuestHUD/Content/Materials/T_PersonLabelDefault.uasset")]) if args.navigation else []),
                ("Cooked dot material", "../../../HandoffQuestHUD/Content/Materials/M_WallhackContact.uasset"),
                ("Cooked sphere mesh", "../../../Engine/Content/BasicShapes/Sphere.uasset"),
            ):
                assets = [row for row in rows if row.get("Filename") == filename and row.get("ChunkType") == "ExportBundleData"]
                present = len(assets) == 1 and int(assets[0]["Size"]) > 0 and int(assets[0]["CompressedSize"]) > 0
                check(name, present, f"{len(assets)} cooked ExportBundleData entry; " + (f"{assets[0]['Size']} bytes" if assets else "missing"),
                      "Keep the mesh/material hard references, recook successfully, and refresh the OBB. A loose source .uasset in the OBB does not prove the material was cooked.")
                if assets:
                    report.setdefault("cooked_assets", []).append({key: assets[0][key] for key in ("Filename", "ChunkId", "Size", "CompressedSize", "Hash")})
            shader_libraries = [row for row in rows if row.get("ChunkType") == "ShaderCodeLibrary" and "SF_VULKAN_ES31_ANDROID" in row.get("Filename", "")]
            check("Mobile Vulkan shader libraries", len(shader_libraries) >= 2,
                  f"{len(shader_libraries)} Vulkan ES3.1 shader library entries", "Complete the Android ASTC shader cook and refresh the OBB.")

        after = args.apk.stat()
        stable = before.st_size == after.st_size and before.st_mtime_ns == after.st_mtime_ns and report["apk_sha256"] == sha256(args.apk)
        check("APK unchanged during verification", stable, "stable" if stable else "changed while being checked",
              "Wait for assembly/copy to finish, then rerun verification.")
    except (OSError, RuntimeError, ValueError, KeyError, BadZipFile, csv.Error, subprocess.TimeoutExpired) as error:
        check("Verification completed", False, str(error), "Resolve the indicated artifact/tool problem and rerun; missing evidence never passes.")
    report["passed"] = bool(checks) and all(item["passed"] for item in checks)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"\n{'PASS' if report['passed'] else 'FAIL'} package verification; report: {args.report}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
