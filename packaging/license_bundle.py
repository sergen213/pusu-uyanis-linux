"""Build-time, stdlib-only license closure; never infer missing legal provenance.

provenance.json: {"version": 1, "packages": [...], "project_author": {...}}.
Optional root `expected_elf_sha256` maps relative original-file paths to
SHA256 identity pins; mismatch is fatal independently of source completeness.
Each package selects original ELF paths with `files` and/or `globs` (paths
relative to this JSON), and supplies package, version, licenses, origin,
notices, copyright_files, and source. An installed pacman package supplies
its own name/version/licenses and exact /usr/share/licenses/<package> files;
an override must agree with that installed name/version. Explicit `licenses`
may refine ambiguous Arch IDs; installed declarations remain in the manifest.
`notices` and `copyright_files` contain paths or {path, sha256} objects, never
SPDX text substituted for upstream notices. GNU IDs must specify their version.
Public/strict mode requires `copyright_complete: true` for private packages;
one header's notice alone is not a complete matched compiled-source inventory.

source: {url, corresponding_to: [original ELF sha256, ...], archives:
[{path, sha256}], recipe: {path, sha256}, build_materials: [{path, sha256}]}.
Hash-bound recipe_ancillary_files, embedded_configuration and
build_evidence_archive are also copied as build materials. Strict mode requires
immutable version/revision origin and actual GPL/LGPL source archives plus exact
build recipe/configuration, not homepage offers.
Local mode copies available genuine materials and records missing public
redistribution obligations; asserted paths/hashes/identities remain mandatory.
`correspondence_attested: false` records unverified downstream correspondence.
Explicit provenance is a maintainer attestation, not a legal determination.
Project authorship is not a license grant. An optional project_author.license_grant
requires {license: "GPL-3.0-only" or "GPL-3.0-or-later", scope:
"newly-authored-native-implementation", text: {path, sha256}}. The text must
match the genuine GNU GPLv3 license; this prospective grant excludes original
assets and third-party material and does not waive source/evidence requirements.
"""

from __future__ import annotations

import fnmatch
import hashlib
import json
import re
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


_GNU = {
    "GPL-1.0": ("GPL-1.0", "GPL"), "GPL-2.0": ("GPL-2.0", "GPL2"),
    "GPL-3.0": ("GPL-3.0", "GPL3"), "LGPL-2.0": ("LGPL-2.0", "LGPL"),
    "LGPL-2.1": ("LGPL-2.1", "LGPL2.1"), "LGPL-3.0": ("LGPL-3.0", "LGPL3"),
    "GPL1": ("GPL-1.0", "GPL"), "GPL2": ("GPL-2.0", "GPL2"),
    "GPL3": ("GPL-3.0", "GPL3"), "LGPL2": ("LGPL-2.0", "LGPL"),
    "LGPL2.1": ("LGPL-2.1", "LGPL2.1"), "LGPL3": ("LGPL-3.0", "LGPL3"),
}
_COPYRIGHT = re.compile(r"(?:copyright\s*:?\s*(?:\(c\)|©)?|©|\(c\))\s*\d{4}", re.IGNORECASE)
_LICENSE_TERMS = re.compile(r"permission is (?:hereby )?granted|redistribution and use|licen[cs]e|public domain|waiver", re.IGNORECASE)
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".rs", ".s", ".S", ".f", ".f90", ".m", ".mm", ".py", ".pl"}


def _sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _fields(path: Path) -> dict[str, list[str]]:
    """Decode pacman sections; empty sections carry no value and are absent."""
    fields: dict[str, list[str]] = {}
    key = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("%") and line.endswith("%"):
            key = line[1:-1]
            fields[key] = []
        elif line and key:
            fields[key].append(line)
    return {key: values for key, values in fields.items() if values}


def _installed() -> tuple[dict[str, dict], dict[str, str]]:
    packages = {}
    owners = {}
    for desc in sorted(Path("/var/lib/pacman/local").glob("*/desc")):
        metadata = _fields(desc)
        name = metadata.get("NAME", [""])[0]
        if not name:
            continue
        files_path = desc.parent / "files"
        files = _fields(files_path).get("FILES", []) if files_path.is_file() else []
        packages[name] = {
            "package": name,
            "version": metadata.get("VERSION", [""])[0],
            "licenses": metadata.get("LICENSE", []),
            "origin": "Installed pacman package " + name,
            "upstream_url": metadata.get("URL", [""])[0],
        }
        for filename in files:
            if not filename.endswith("/"):
                owners["/" + filename] = name
    return packages, owners


def _strings(value: object, field: str, *, required: bool = False) -> list[str]:
    if value is None and not required:
        return []
    if not isinstance(value, list) or any(not isinstance(item, str) or not item.strip() for item in value):
        raise ValueError(field + " must be a list of nonempty strings")
    if required and not value:
        raise ValueError(field + " must not be empty")
    return value


def _matches(entry: dict, source: Path, base: Path) -> bool:
    files = _strings(entry.get("files"), "files")
    globs = _strings(entry.get("globs"), "globs")
    return any((base / item).resolve() == source for item in files) or any(
        fnmatch.fnmatchcase(str(source), str((base / pattern).resolve())) for pattern in globs
    )


def _asset(value: object, base: Path, *, hashed: bool = False) -> Path:
    if isinstance(value, str) and not hashed:
        value = {"path": value}
    if not isinstance(value, dict) or not isinstance(value.get("path"), str) or not value["path"].strip():
        raise ValueError("file entry requires path" + (" and sha256" if hashed else ""))
    path = (base / value["path"]).resolve(strict=True)
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError("missing or empty file: " + str(path))
    expected = value.get("sha256")
    if hashed or expected is not None:
        if not isinstance(expected, str) or not _SHA256.fullmatch(expected):
            raise ValueError("invalid or missing sha256 for " + str(path))
        if _sha256(path) != expected:
            raise ValueError("sha256 mismatch for " + str(path))
    return path


def _gnu_licenses(licenses: list[str]) -> list[tuple[str, Path]]:
    result = []
    for expression in licenses:
        for token in re.findall(r"[A-Za-z0-9][A-Za-z0-9.+-]*", expression):
            if not re.match(r"(?:A?GPL|LGPL)(?:[-0-9]|$)", token):
                continue
            plain = re.sub(r"-(?:only|or-later)$", "", token).rstrip("+")
            identity = _GNU.get(plain)
            if identity is None:
                raise ValueError("GNU license version is ambiguous or unsupported: " + token)
            canonical, legacy = identity
            suffix = "-or-later" if token.endswith(("-or-later", "+")) else "-only"
            filename = canonical + suffix + ".txt"
            candidates = [
                Path("/usr/share/licenses/spdx") / filename,
                Path("/usr/share/licenses/common") / legacy / "license.txt",
                Path(__file__).resolve().parent / "licenses/common" / filename,
            ]
            path = next((candidate for candidate in candidates if candidate.is_file() and candidate.stat().st_size), None)
            if path is None:
                raise ValueError("exact GNU common license missing: " + filename)
            text = path.read_text(encoding="utf-8")
            version = canonical.split("-", 1)[1].removesuffix(".0")
            if not re.search(r"Version\s+" + re.escape(version) + r"(?![0-9.])", text, re.IGNORECASE):
                raise ValueError("GNU common text does not match declared version: " + str(path))
            pair = (token, path)
            if pair not in result:
                result.append(pair)
            if canonical.startswith("LGPL-"):
                reference = "GPL-3.0-only" if canonical == "LGPL-3.0" else "GPL-2.0-only"
                for companion in _gnu_licenses([reference]):
                    if companion not in result:
                        result.append(companion)
    return result


def _source_url(source: dict, version: str) -> str:
    url = source.get("url")
    if not isinstance(url, str) or urlsplit(url).scheme not in {"https", "http"} or not urlsplit(url).netloc:
        raise ValueError("source.url must be an explicit versioned HTTP(S) source URL")
    decoded = unquote(url)
    revision = source.get("revision")
    if revision is not None:
        if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-fA-F]{40,64}", revision) or revision not in decoded:
            raise ValueError("source.revision must be a full immutable revision present in source.url")
    else:
        upstream = version.split(":", 1)[-1].rsplit("-", 1)[0]
        if not upstream or not re.search(r"(?<![A-Za-z0-9])" + re.escape(upstream) + r"(?![A-Za-z0-9])", decoded):
            raise ValueError("source.url does not identify package version " + version)
    return url


def _archive_has_source(path: Path) -> bool:
    """Recognize program source, including source-only Perl build helpers."""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            return any(not item.is_dir() and item.file_size and Path(item.filename).suffix in _SOURCE_EXTENSIONS for item in archive.infolist())
    try:
        with tarfile.open(path, "r:*") as archive:
            return any(item.isfile() and item.size and Path(item.name).suffix in _SOURCE_EXTENSIONS for item in archive)
    except tarfile.TarError:
        return False


def _resolve(entry: dict, members: list[dict], base: Path, strict: bool) -> tuple[dict, list[tuple[str, Path]], str]:
    obligations = []
    integrity = []
    assets: list[tuple[str, Path]] = []
    package = entry.get("package")
    version = entry.get("version")
    if not isinstance(package, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]*", package):
        raise ValueError("valid package identity required")
    if not isinstance(version, str) or not version.strip():
        raise ValueError("exact package version required for attributed packages")
    licenses = _strings(entry.get("licenses"), "licenses")
    if not licenses:
        obligations.append("declared package licenses missing")
    gnu = []
    gnu_required = any(re.search(r"(?:A?GPL|LGPL)(?:[-0-9]|$)", expression) for expression in licenses)
    for expression in licenses:
        try:
            for item in _gnu_licenses([expression]):
                if item not in gnu:
                    gnu.append(item)
        except ValueError as error:
            obligations.append(str(error))
            if not strict:
                # Catalogue candidates are references, not guessed declarations.
                families = set(re.findall(r"\b(?:GPL|LGPL)\b", expression))
                references = []
                if "GPL" in families:
                    references.extend(["GPL-1.0-only", "GPL-2.0-only", "GPL-3.0-only"])
                if "LGPL" in families:
                    references.extend(["LGPL-2.0-only", "LGPL-2.1-only", "LGPL-3.0-only"])
                for identifier in references:
                    try:
                        for item in _gnu_licenses([identifier]):
                            if item not in gnu:
                                gnu.append(item)
                    except ValueError as reference_error:
                        obligations.append("GNU catalogue reference unavailable: " + str(reference_error))
    assets.extend(("gnu", path) for _, path in gnu)
    notice_paths = set()
    installed_dir = Path("/usr/share/licenses") / package
    if entry.get("installed") and installed_dir.is_dir():
        notice_paths.update(path.resolve() for path in installed_dir.resolve().rglob("*") if path.is_file())
    for field in ("notices", "copyright_files"):
        values = entry.get(field, [])
        if not isinstance(values, list):
            integrity.append(field + " must be a list of exact notice file paths")
            continue
        for value in values:
            try:
                notice_paths.add(_asset(value, base))
            except (ValueError, OSError) as error:
                integrity.append(str(error))
    copyright_found = False
    if not entry.get("installed") and entry.get("copyright_complete") is not True:
        obligations.append("complete compiled-source copyright inventory not attested")
    license_found = bool(gnu)
    common_hashes = {_sha256(path) for _, path in gnu}
    for path in sorted(notice_paths):
        try:
            data = path.read_bytes()
            if not data.strip():
                obligations.append("empty installed notice: " + str(path))
            text = data.decode("utf-8", errors="replace")
            generic = hashlib.sha256(data).hexdigest() in common_hashes or any(
                root in path.parents for root in (
                    Path("/usr/share/licenses/common"), Path("/usr/share/licenses/spdx"),
                    Path(__file__).resolve().parent / "licenses/common",
                )
            )
            copyright_found |= not generic and bool(_COPYRIGHT.search(text))
            license_found |= bool(_LICENSE_TERMS.search(text))
            normalized = " ".join(text.lower().split())
            zlib_terms = "the origin of this software must not be misrepresented" in normalized and "altered source versions must be plainly marked" in normalized
            if zlib_terms and "MIT" in licenses and "Zlib" not in licenses:
                obligations.append("declared MIT conflicts with supplied zlib-style license terms; resolve using exact component evidence")
            assets.append(("notice", path))
        except OSError as error:
            integrity.append(str(error))
    if not notice_paths:
        obligations.append("exact package license/copyright notices missing; standard SPDX text is insufficient")
    if not copyright_found:
        obligations.append("upstream copyright notice not established in exact package notice files")
    if not license_found:
        obligations.append("exact package license terms not established in supplied notices")
    origin = entry.get("origin")
    if not isinstance(origin, str) or not origin.strip():
        raise ValueError("substantiated package origin required")
    source = entry.get("source", {})
    if not isinstance(source, dict):
        raise ValueError("source provenance must be an object")
    url = source.get("url", "")
    pinned = False
    try:
        url = _source_url(source, version)
        pinned = True
    except ValueError as error:
        obligations.append(str(error))
        if url and (not isinstance(url, str) or urlsplit(url).scheme not in {"https", "http"} or not urlsplit(url).netloc):
            integrity.append("malformed claimed source URL")
        revision = source.get("revision")
        if revision is not None and (not isinstance(revision, str) or not re.fullmatch(r"[0-9a-fA-F]{40,64}", revision) or not isinstance(url, str) or revision not in url):
            integrity.append("invalid or mismatched claimed source revision")
    corresponding = _strings(source.get("corresponding_to"), "source.corresponding_to")
    if any(not _SHA256.fullmatch(digest) for digest in corresponding):
        integrity.append("source.corresponding_to contains invalid ELF sha256")
    missing = sorted({member["sha256"] for member in members} - set(corresponding))
    if gnu_required and missing:
        obligations.append("source correspondence unverified for original ELF sha256: " + ", ".join(missing))
    if corresponding and missing:
        integrity.append("claimed corresponding_to hashes do not include all original bundled ELF hashes")
    if source.get("correspondence_attested") is False:
        obligations.append("supplied upstream sources are not attested as exact downstream corresponding sources")
    archives = source.get("archives", [])
    if not isinstance(archives, list):
        raise ValueError("source.archives must be a list")
    if gnu_required and not archives:
        obligations.append("GPL/LGPL requires supplied corresponding source archives with sha256, not URL-only offers")
    for value in archives:
        try:
            path = _asset(value, base, hashed=True)
            if not _archive_has_source(path):
                raise ValueError("claimed source archive has no regular source files or is not tar/zip: " + str(path))
            assets.append(("source", path))
        except (ValueError, OSError, zipfile.BadZipFile) as error:
            integrity.append(str(error))
    if source.get("recipe") is not None:
        try:
            assets.append(("recipe", _asset(source["recipe"], base, hashed=True)))
        except (ValueError, OSError) as error:
            integrity.append(str(error))
    elif gnu_required:
        obligations.append("GPL/LGPL exact build recipe/configuration missing")
    build_materials = []
    for field in ("build_materials", "recipe_ancillary_files"):
        values = source.get(field, [])
        if not isinstance(values, list):
            raise ValueError("source." + field + " must be a list")
        build_materials.extend(values)
    for field in ("embedded_configuration", "build_evidence_archive"):
        if source.get(field) is not None:
            build_materials.append(source[field])
    for value in build_materials:
        try:
            assets.append(("build-material", _asset(value, base, hashed=True)))
        except (ValueError, OSError) as error:
            integrity.append(str(error))
    for field in ("missing_inputs", "evidence_gaps", "unresolved_public_obligations", "redistribution_obligations"):
        obligations.extend(_strings(entry.get(field), field))
    if integrity or (strict and obligations):
        raise ValueError("; ".join(integrity + obligations))
    source_record = {
        "url": url, "immutable_origin": pinned,
        "correspondence": "attested" if pinned and corresponding and not missing and not obligations and source.get("correspondence_attested") is not False else "unverified",
        "corresponding_to": sorted(set(corresponding)),
    }
    if source.get("revision"):
        source_record["revision"] = source["revision"]
    if entry.get("upstream_url"):
        source_record["upstream_url"] = entry["upstream_url"]
    source_record["notice_status"] = "incomplete-public-redistribution" if obligations else "declared-materials-supplied"
    if entry.get("evidence"):
        source_record["evidence"] = entry["evidence"]
    record = {
        "package": package, "version": version, "license": licenses,
        "origin": origin, "source": source_record, "elf": members,
        "redistribution_obligations": list(dict.fromkeys(obligations)),
        "copyright_complete": entry.get("copyright_complete") is True,
    }
    record["supplied_provenance"] = {
        key: value for key, value in entry.items()
        if key not in {"installed", "installed_licenses"}
    }
    if entry.get("installed"):
        record["installed_licenses"] = entry.get("installed_licenses", licenses)
    notice = (
        f"Package: {package}\nVersion: {version}\nOrigin: {origin}\n"
        f"Declared licenses: {'; '.join(licenses) or 'not established'}\n"
        f"Source URL: {url or 'not established'}\n"
        f"Known upstream homepage (not an immutable source offer): {entry.get('upstream_url') or 'not recorded'}\n"
        f"Immutable origin established: {pinned}\n"
        f"Source correspondence: {source_record['correspondence']} (provenance attestation, not a reproducible-build verification)\n"
    )
    if obligations:
        notice += "Not established as complete for public redistribution. Remaining obligations:\n" + "".join("- " + item + "\n" for item in record["redistribution_obligations"])
    else:
        notice += "Declared notice/source/build-material obligations are supplied; this is not an automatic legal determination.\n"
    return record, assets, notice


def _resolve_project(entry: dict, members: list[dict], base: Path, strict: bool) -> tuple[dict, list[tuple[str, Path]], str]:
    notice = entry.get("notice")
    package = entry.get("package")
    if not isinstance(notice, str) or not notice.strip() or not isinstance(package, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]*", package):
        raise ValueError("project authorship requires an actual notice and safe package name")
    grant = entry.get("license_grant")
    if grant is None:
        obligations = ["New native implementation has an authorship notice only; no public redistribution license grant is established."]
        if strict:
            raise ValueError(obligations[0])
        record = {"package": package, "version": entry["version"], "license": [], "origin": "newly authored native project implementation", "source": {"kind": "project-authorship"}, "elf": members, "redistribution_obligations": obligations}
        return record, [], notice + "\nPublic redistribution license grant: not established.\n"
    if not isinstance(grant, dict) or grant.get("license") not in ("GPL-3.0-only", "GPL-3.0-or-later") or grant.get("scope") != "newly-authored-native-implementation":
        raise ValueError("project license grant requires explicit GNU GPLv3 terms and newly-authored-native-implementation scope")
    path = _asset(grant.get("text"), base, hashed=True)
    canonical = _gnu_licenses([grant["license"]])[0][1]
    if _sha256(path) != _sha256(canonical):
        raise ValueError("project license grant text does not match genuine GNU GPLv3 terms")
    licensed = dict(entry, licenses=[grant["license"]], notices=[grant["text"]], origin="newly authored native project implementation")
    record, assets, body = _resolve(licensed, members, base, strict)
    record["source"]["kind"] = "licensed-project-source"
    return record, assets, notice + "\nLicense grant scope: newly authored native implementation only; original assets and third-party material retain their own terms.\n" + body


def _copy_package(record: dict, assets: list[tuple[str, Path]], notice: str, target: Path) -> None:
    directory = target / record.get("storage_directory", record["package"])
    directory.mkdir()
    copied = []
    for index, (kind, path) in enumerate(assets):
        category = "source" if kind in {"source", "recipe", "build-material"} else "notices"
        destination = directory / category / f"{index:03d}-{path.name}"
        destination.parent.mkdir(exist_ok=True)
        shutil.copyfile(path, destination)
        item = {"kind": kind, "file": destination.relative_to(target).as_posix(), "sha256": _sha256(destination), "original": str(path)}
        copied.append(item)
        if kind in {"source", "recipe", "build-material"}:
            notice += f"{kind}: {item['file']}  SHA256 {item['sha256']}\n"
    notice_path = directory / "NOTICE.txt"
    notice_path.write_text(notice, encoding="utf-8")
    record["notice"] = {"file": notice_path.relative_to(target).as_posix(), "sha256": _sha256(notice_path)}
    record["source"]["notice"] = record["notice"]
    record["files"] = copied
    record["source"]["archives"] = [item for item in copied if item["kind"] == "source"]
    record["source"]["build_materials"] = [item for item in copied if item["kind"] == "build-material"]
    recipes = [item for item in copied if item["kind"] == "recipe"]
    if recipes:
        record["source"]["recipe"] = recipes[0]


def _check_elf_identity(source: Path, digest: str, pins: dict[Path, str]) -> None:
    if source in pins and digest != pins[source]:
        raise ValueError("expected_elf_sha256 original binary identity mismatch: " + str(source))


def bundle_licenses(elf_records: list[dict], provenance_path: Path, licenses_dir: Path, *, strict: bool = True) -> list[dict]:
    """Bundle genuine available materials and aggregate unresolved obligations.

    strict=True rejects incomplete public redistribution provenance. Local
    strict=False records redistribution_obligations without inventing rights.
    Invalid identities, selectors, claimed files and hashes remain fatal.
    Returned paths are relative to an absent/empty licenses_dir.
    """
    provenance_path = provenance_path.resolve(strict=True)
    base = provenance_path.parent
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    if not isinstance(provenance, dict) or provenance.get("version") != 1:
        raise ValueError("provenance must be an object with version 1")
    identity_pins = provenance.get("expected_elf_sha256", {})
    if not isinstance(identity_pins, dict):
        raise ValueError("expected_elf_sha256 must map relative original-file paths to SHA256")
    resolved_pins = {}
    for relative, digest in identity_pins.items():
        if not isinstance(relative, str) or not relative.strip() or "\0" in relative or Path(relative).is_absolute() or Path(relative).name in {"", ".", ".."}:
            raise ValueError("expected_elf_sha256 requires relative original-file paths")
        if not isinstance(digest, str) or not _SHA256.fullmatch(digest):
            raise ValueError("expected_elf_sha256 contains invalid SHA256 for " + relative)
        original = (base / relative).resolve()
        if original.is_dir() or (original in resolved_pins and resolved_pins[original] != digest):
            raise ValueError("expected_elf_sha256 has a directory or conflicting original-file identity: " + relative)
        resolved_pins[original] = digest
    overrides = provenance.get("packages", [])
    if not isinstance(overrides, list) or any(not isinstance(item, dict) for item in overrides):
        raise ValueError("provenance.packages must be a list of objects")
    for entry in overrides:
        if set(entry) & {"installed", "installed_licenses", "authored", "unattributed", "storage_directory"}:
            raise ValueError("package provenance cannot override internal identity or storage")
        if not _strings(entry.get("files"), "files") and not _strings(entry.get("globs"), "globs"):
            raise ValueError("every provenance package needs exact files or explicit globs")
    project = provenance.get("project_author", {})
    if not isinstance(project, dict):
        raise ValueError("project_author must be an object")
    project_paths = _strings(project.get("executables"), "project_author.executables")
    permitted = {"usr/bin/pusu-installer", "usr/bin/pusu-launcher", "usr/bin/pusu-game"}
    if set(project_paths) - permitted:
        raise ValueError("project authorship is restricted to the three newly authored native executables")
    installed, owners = _installed()
    groups: dict[tuple[str, str], tuple[dict, list[dict]]] = {}
    failures = []
    packaged_seen = set()
    for elf in elf_records:
        label = str(elf.get("packaged", "invalid ELF record")) if isinstance(elf, dict) else "invalid ELF record"
        try:
            if not isinstance(elf, dict):
                raise ValueError("ELF records must be objects")
            source = Path(elf["source"])
            packaged = PurePosixPath(elf["packaged"])
            if not source.is_absolute() or source != source.resolve(strict=True):
                raise ValueError("ELF source must be a resolved absolute original path")
            native_layout = len(packaged.parts) == 3 and packaged.parts[:2] in {("usr", "bin"), ("usr", "lib")} and packaged.name not in {".", ".."}
            if packaged.is_absolute() or not (native_layout or str(packaged) == "AppImage/runtime") or str(packaged) != elf["packaged"]:
                raise ValueError("ELF packaged path must be usr/bin/name, usr/lib/name, or AppImage/runtime")
            if str(packaged) in packaged_seen:
                raise ValueError("duplicate packaged ELF path")
            packaged_seen.add(str(packaged))
            if not isinstance(elf.get("sha256"), str) or not _SHA256.fullmatch(elf["sha256"]) or _sha256(source) != elf["sha256"]:
                raise ValueError("ELF original source sha256 missing or mismatched")
            _check_elf_identity(source, elf["sha256"], resolved_pins)
            matches = [entry for entry in overrides if _matches(entry, source, base)]
            if len(matches) > 1:
                raise ValueError("ambiguous overlapping provenance file/glob overrides")
            owner = owners.get(str(source))
            entry = dict(installed[owner]) if owner else {}
            if owner:
                entry["installed"] = True
            if matches:
                override = matches[0]
                if owner and any(override.get(key, entry[key]) != entry[key] for key in ("package", "version")):
                    raise ValueError("override disagrees with installed package name/version")
                entry["installed_licenses"] = entry.get("licenses", [])
                entry.update(override)
            authored = str(packaged) in project_paths and source.name == packaged.name and not owner and not matches
            if authored:
                entry = dict(project)
                entry["authored"] = True
            if not entry:
                if strict:
                    raise ValueError("no installed pacman owner or explicit private provenance")
                storage = "unattributed-" + hashlib.sha256(str(source).encode("utf-8")).hexdigest()[:24]
                entry = {"unattributed": True, "storage_directory": storage}
                name, version = storage, ""
            else:
                name, version = entry.get("package"), entry.get("version")
                if not isinstance(name, str) or not name or not isinstance(version, str) or not version:
                    raise ValueError("claimed package name/version missing")
            key = (name, version)
            if key not in groups:
                groups[key] = (entry, [])
            elif groups[key][0] != entry:
                raise ValueError("conflicting provenance for one package/version")
            groups[key][1].append({"source": str(source), "packaged": str(packaged), "sha256": elf["sha256"]})
        except (KeyError, TypeError, ValueError, OSError) as error:
            failures.append(label + ": " + str(error))
    resolved = []
    names = set()
    for (package, version), (entry, members) in sorted(groups.items()):
        try:
            if package in names:
                raise ValueError("multiple versions share the same license output package directory")
            names.add(package)
            if entry.get("authored"):
                resolved.append(_resolve_project(entry, members, base, strict))
            elif entry.get("unattributed"):
                obligations = ["Package ownership, version, licenses, copyright inventory and corresponding-source/build obligations have not been established for the original ELF."]
                record = {"package": None, "version": None, "license": [], "origin": "Original local ELF; no installed owner or explicit package identity established", "source": {"kind": "unattributed-original-elf", "correspondence": "unverified"}, "elf": members, "redistribution_obligations": obligations, "storage_directory": entry["storage_directory"]}
                notice = record["origin"] + "\n" + obligations[0] + "\n"
                for member in members:
                    notice += f"Original ELF: {member['source']}  SHA256 {member['sha256']}\nPackaged ELF: {member['packaged']}\n"
                resolved.append((record, [], notice))
            else:
                resolved.append(_resolve(entry, members, base, strict))
        except (ValueError, OSError) as error:
            failures.append(f"{package} {version}: {error}")
    if failures:
        raise ValueError("Unresolved bundled package obligations:\n- " + "\n- ".join(failures) + "\nSupply exact notices/copyrights and version-pinned source provenance; GNU packages also need hash-verified corresponding archives and build recipe/configuration.")
    licenses_dir = licenses_dir.absolute()
    if licenses_dir.exists() and (not licenses_dir.is_dir() or any(licenses_dir.iterdir())):
        raise ValueError("license output must be absent or empty: " + str(licenses_dir))
    licenses_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".pusu-licenses-", dir=licenses_dir.parent) as temporary:
        staging = Path(temporary) / "licenses"
        staging.mkdir()
        for record, assets, notice in resolved:
            _copy_package(record, assets, notice, staging)
        if licenses_dir.exists():
            licenses_dir.rmdir()
        staging.rename(licenses_dir)
    return [record for record, _, _ in resolved]


def _check_mapping_and_invalid_manifest() -> None:
    """Small runnable boundary check; no host dependency or package build."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        desc = root / "desc"
        desc.write_text("%NAME%\npackage\n\n%VERSION%\n\n%URL%\n\n%FILES%\n\n%LICENSE%\nMIT\nBSD-2-Clause\n", encoding="utf-8")
        metadata = _fields(desc)
        assert metadata == {"NAME": ["package"], "LICENSE": ["MIT", "BSD-2-Clause"]}
        assert metadata.get("URL", [""])[0] == ""
        assert metadata.get("VERSION", [""])[0] == ""
        assert metadata.get("FILES", []) == []
        helper = root / "helper.pl"
        helper.write_text('print "source";\n', encoding="utf-8")
        archive_path = root / "helper.tar.gz"
        with tarfile.open(archive_path, "w:gz") as archive:
            archive.add(helper, arcname="helper.pl")
        assert _archive_has_source(archive_path), "Perl build-helper source was rejected"
        documentation = root / "documentation.zip"
        with zipfile.ZipFile(documentation, "w") as archive:
            archive.writestr("manual.html", "<p>Documentation, not program source.</p>")
        assert not _archive_has_source(documentation), "Documentation was claimed as program source"
        source = root / "libsample.so"
        source.write_bytes(b"sample")
        assert _matches({"files": ["libsample.so"]}, source.resolve(), root)
        assert _matches({"globs": ["lib*.so"]}, source.resolve(), root)
        assert _matches({"globs": ["../" + root.name + "/lib*.so"]}, source.resolve(), root)
        assert _COPYRIGHT.search("Copyright: 2012-2020 Red Hat, Inc.")
        assert not _COPYRIGHT.search("The above copyright notice shall be included.")
        assert not _matches({"files": ["different.so"]}, source.resolve(), root)
        manifest = root / "provenance.json"
        manifest.write_text('{"version": 1, "packages": [{"files": []}]}', encoding="utf-8")
        try:
            bundle_licenses([], manifest, root / "licenses")
        except ValueError as error:
            assert "exact files or explicit globs" in str(error)
        else:
            raise AssertionError("selector-less provenance accepted")
        notice = root / "notice.txt"
        notice.write_text("Copyright 2026 Boundary fixture author. Permission is hereby granted.\n", encoding="utf-8")
        entry = {"package": "boundary-fixture", "version": "1.0", "licenses": ["MIT"], "origin": "temporary assertion fixture", "notices": ["notice.txt"], "copyright_complete": True, "source": {"url": "https://example.org/source-1.0.tar.xz"}, "evidence_gaps": ["exact downstream source correspondence not established"]}
        members = [{"source": str(source), "packaged": "usr/lib/libsample.so", "sha256": _sha256(source)}]
        record, _, _ = _resolve(entry, members, root, False)
        assert entry["evidence_gaps"][0] in record["redistribution_obligations"]
        assert record["supplied_provenance"]["evidence_gaps"] == entry["evidence_gaps"]
        entry["source"].update({
            "recipe_ancillary_files": [{"path": "helper.pl", "sha256": _sha256(helper)}],
            "embedded_configuration": {"path": "notice.txt", "sha256": _sha256(notice)},
            "build_evidence_archive": {"path": "helper.tar.gz", "sha256": _sha256(archive_path)},
        })
        _, assets, _ = _resolve(entry, members, root, False)
        assert all(("build-material", path.resolve()) in assets for path in (helper, notice, archive_path))
        bad_material = {**entry, "source": {**entry["source"], "recipe_ancillary_files": [{"path": "helper.pl", "sha256": "0" * 64}]}}
        try:
            _resolve(bad_material, members, root, False)
        except ValueError as error:
            assert "sha256 mismatch" in str(error)
        else:
            raise AssertionError("tampered ancillary source material accepted")
        try:
            _resolve(entry, members, root, True)
        except ValueError as error:
            assert entry["evidence_gaps"][0] in str(error)
        else:
            raise AssertionError("strict mode accepted an explicit evidence gap")
        project = {"package": "pusu-native", "version": "1.0", "notice": "Newly authored native implementation; original assets and third-party materials are excluded."}
        record, _, _ = _resolve_project(project, members, root, False)
        assert record["license"] == [] and "authorship notice only" in record["redistribution_obligations"][0]
        license_text = root / "LICENSE"
        license_text.write_bytes(_gnu_licenses(["GPL-3.0-only"])[0][1].read_bytes())
        grant = {"license": "GPL-3.0-only", "scope": "newly-authored-native-implementation", "text": {"path": "LICENSE", "sha256": _sha256(license_text)}}
        project["license_grant"] = grant
        record, assets, _ = _resolve_project(project, members, root, False)
        assert record["license"] == ["GPL-3.0-only"] and ("notice", license_text.resolve()) in assets
        assert any("source correspondence unverified" in item for item in record["redistribution_obligations"])
        assert not any("authorship notice only" in item for item in record["redistribution_obligations"])
        try:
            _resolve_project(project, members, root, True)
        except ValueError as error:
            assert "source correspondence unverified" in str(error)
        else:
            raise AssertionError("project license grant bypassed source obligations")
        for invalid in (
            {**grant, "scope": "all-original-art-and-dependencies"},
            {**grant, "license": "MIT"},
            {**grant, "license": ["GPL-3.0-only"]},
            {**grant, "text": {"path": "LICENSE", "sha256": "0" * 64}},
        ):
            try:
                _resolve_project({**project, "license_grant": invalid}, members, root, False)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid or tampered project grant accepted")
        entry["notices"] = [{"path": "notice.txt", "sha256": "0" * 64}]
        for strict in (False, True):
            try:
                _resolve(entry, members, root, strict)
            except ValueError as error:
                assert "sha256 mismatch" in str(error)
            else:
                raise AssertionError("tampered notice hash accepted")
        for strict in (False, True):
            try:
                # The production check is unconditional in both modes.
                _check_elf_identity(source.resolve(), members[0]["sha256"], {source.resolve(): "0" * 64})
            except ValueError as error:
                assert "original binary identity mismatch" in str(error)
            else:
                raise AssertionError("original binary identity pin mismatch accepted")


if __name__ == "__main__":
    _check_mapping_and_invalid_manifest()
