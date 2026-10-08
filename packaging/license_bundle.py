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
`copyright_complete` is retained as audit metadata, not an exhaustive-inventory
release gate; genuine applicable notices and explicit unresolved duties remain.

source: {url, corresponding_to: [original ELF sha256, ...], archives:
[{path, sha256}], recipe: {path, sha256}, build_materials: [{path, sha256}]}.
Hash-bound recipe_ancillary_files, embedded_configuration and
build_evidence_archive are also copied as build materials. Strict mode requires
actual GPL/LGPL source archives mapped to the ELF hashes plus exact build
recipe/configuration. A remote immutable URL is optional for local delivery.
MPL-1.1/2.0 require mapped Covered Software source archives and a hash-bound
source.availability_notice identifying the MPL source and delivered archive
filenames (or the package NOTICE.txt archive list). MPL-1.1 also requires its
covered build/install scripts in recipe or covered_build_scripts [{path, sha256}].
MPL-2.0 does not require a GNU whole-work recipe. Evidence-backed `licenses`
overrides select component/elected scope; OR and unknown scopes stay unresolved,
and known WITH exceptions never automatically waive underlying source duties.
Missing/unpinned optional origins and false optional correspondence are
nonblocking provenance diagnostics, never changed into positive attestations.
Nonstandard grants may supply license_scopes [{license, scope, delivery,
terms: {path, sha256}}], explicitly identifying included components and reviewed
notice-only or source-required duties. Artistic-1.0 may explicitly elect
standard-version delivery under section 4(a), with actual source archives and
source.availability_notice directions, not invented downstream correspondence.
Exact original grant terms are delivered, not relabeled MIT. These attestations
cannot downgrade known GNU/MPL duties.
Local mode records missing delivery duties. Claimed paths, hashes, URL/revision
syntax and ELF mappings remain mandatory in both modes. Explicit provenance
and a passing conditional-delivery gate are not legal clearance.
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
_SCOPED_GRANT = re.compile(r"permission is (?:hereby )?granted|permission to (?:use|copy).{0,160}granted|authors hereby grant permission to use, copy, modify|redistribution and use|public domain|free software.{0,100}redistribut|licen[cs]ed under|may.{0,40}(?:use|redistribut|copy)|(?:hereby )?grants.{0,256}patent\s+licen[cs]e|distribute and use freely;\s*there are no restrictions on further\s+dissemination and usage", re.IGNORECASE | re.DOTALL)
_OPTIONAL_CREDIT_GRANT = re.compile(r"use this source code in any fashion you see fit.{0,160}giving me credit.{0,160}(?:is\s+)?optional", re.IGNORECASE | re.DOTALL)
_SOURCE_DUTY = re.compile(r"GNU (?:Lesser |Affero )?General Public License|Mozilla Public License|(?:must|shall|required).{0,100}(?:supply|provide|deliver|available).{0,50}source|source.{0,50}(?:must|shall).{0,100}(?:supply|provide|deliver|available)", re.IGNORECASE | re.DOTALL)
_PUBLIC_DOMAIN_GRANT = re.compile(r"(?:is|are)\s+(?:now\s+)?in\s+(?:the\s+)?public domain|(?:dedicat|releas|plac)\w*.{0,160}public domain", re.IGNORECASE | re.DOTALL)
_PUBLIC_DOMAIN_IDS = {"Public-Domain", "LicenseRef-PublicDomain", "LicenseRef-Public-Domain", "CC0-1.0", "Unlicense"}
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".rs", ".s", ".S", ".f", ".f90", ".m", ".mm", ".py", ".pl"}
_NOTICE_ONLY = {
    "MIT", "MIT-0", "MIT-open-group", "X11", "0BSD", "BSD-1-Clause", "BSD-2-Clause", "BSD-3-Clause",
    "BSD-4-Clause", "BSD-4-Clause-UC", "BSD-3-Clause-Clear", "ISC", "Apache-2.0", "Zlib", "CC0-1.0",
    "Unlicense", "HPND", "HPND-sell-variant", "Unicode-DFS-2016", "Unicode-3.0",
    "NAIST-2003", "SMLNJ", "FTL", "IJG", "libpng-2.0", "libtiff", "SunPro",
    "Public-Domain", "LicenseRef-PublicDomain", "LicenseRef-Public-Domain",
}
# These retain the underlying grant's duties; no blanket WITH source exemption.
_KNOWN_EXCEPTIONS = {"GCC-exception-3.1", "LLVM-exception", "Linux-syscall-note", "PCRE2-exception"}


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


def _http_source_url(url: object) -> bool:
    if not isinstance(url, str) or not url or re.search(r"[\s\x00-\x1f\x7f]", url):
        return False
    try:
        parsed = urlsplit(url)
        return parsed.scheme in {"https", "http"} and bool(parsed.hostname) and (parsed.port is None or 0 < parsed.port <= 65535)
    except ValueError:
        return False


def _source_url(source: dict, version: str) -> str:
    url = source.get("url")
    if not _http_source_url(url):
        raise ValueError("source.url must be an explicit versioned HTTP(S) source URL")
    decoded = unquote(url)
    revision = source.get("revision")
    if "revision" in source:
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


def _archive_has_copyright(path: Path) -> bool:
    """Find original source-form headers, not a generic COPYING document."""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for item in archive.infolist():
                if not item.is_dir() and Path(item.filename).suffix in _SOURCE_EXTENSIONS:
                    with archive.open(item) as stream:
                        if _COPYRIGHT.search(stream.read(131072).decode("utf-8", errors="replace")):
                            return True
    else:
        with tarfile.open(path, "r:*") as archive:
            for item in archive:
                if item.isfile() and Path(item.name).suffix in _SOURCE_EXTENSIONS:
                    with archive.extractfile(item) as stream:
                        if _COPYRIGHT.search(stream.read(131072).decode("utf-8", errors="replace")):
                            return True
    return False


def _resolve(entry: dict, members: list[dict], base: Path, strict: bool, *, project_notice: str = "") -> tuple[dict, list[tuple[str, Path]], str]:
    obligations = []
    diagnostics = []
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
    scopes = entry.get("license_scopes", [])
    if not isinstance(scopes, list):
        raise ValueError("license_scopes must be a list")
    scoped_grants = {}
    scoped_notices = set()
    scoped_optional_credit = {}
    scoped_source_required = False
    standard_version_required = False
    for value in scopes:
        if not isinstance(value, dict):
            raise ValueError("license_scopes entries must be objects")
        label = value.get("license")
        scope = value.get("scope")
        delivery = value.get("delivery")
        if not isinstance(label, str) or not label.strip() or not any(label == component for expression in licenses for component in re.split(r"\s+AND\s+", expression)):
            raise ValueError("license_scopes must name an exact declared component grant")
        if not isinstance(scope, str) or not scope.strip() or not isinstance(delivery, str) or delivery not in {"notice-only", "source-required", "standard-version"}:
            raise ValueError("license_scopes requires actual component scope and supported delivery duty")
        if delivery == "standard-version" and label != "Artistic-1.0":
            raise ValueError("standard-version delivery is only supported for explicit Artistic-1.0 section 4(a) scope")
        path = _asset(value.get("terms"), base, hashed=True)
        text = path.read_text(encoding="utf-8")
        # Match presentation, never rewrite the hash-bound delivered terms.
        grant_text = " ".join(re.sub(r"(?m)^[ \t]*\* ?", "", text).split())
        if not (_SCOPED_GRANT.search(grant_text) or _OPTIONAL_CREDIT_GRANT.search(grant_text)):
            raise ValueError("license_scopes terms do not establish an actual grant or waiver")
        if delivery == "notice-only" and (re.search(r"\b(?:A?GPL|LGPL|MPL)(?:[-0-9]|$)", label) or _SOURCE_DUTY.search(grant_text)):
            raise ValueError("notice-only license scope cannot waive GNU/MPL or explicit source-delivery terms")
        scoped_grants[label] = value
        scoped_optional_credit[label] = scoped_optional_credit.get(label, True) and bool(_OPTIONAL_CREDIT_GRANT.search(grant_text))
        scoped_notices.add(path)
        scoped_source_required |= delivery == "source-required"
        standard_version_required |= delivery == "standard-version"
    gnu = []
    gnu_required = any(re.search(r"(?:A?GPL|LGPL)(?:[-0-9]|$)", expression) for expression in licenses)
    mpl_required = any(re.search(r"\bMPL-(?:1\.1|2\.0)\b", expression) for expression in licenses)
    mpl11_required = any(re.search(r"\bMPL-1\.1\b", expression) for expression in licenses)
    source_required = gnu_required or mpl_required or scoped_source_required
    for expression in licenses:
        # Flat scope only: callers must explicitly elect OR alternatives.
        for component in re.split(r"\s+AND\s+", expression):
            if " OR " in component or "(" in component or ")" in component:
                obligations.append("license alternative/component scope requires an evidence-backed licenses override: " + expression)
                continue
            grant, separator, exception = component.partition(" WITH ")
            if separator and exception not in _KNOWN_EXCEPTIONS:
                obligations.append("license exception scope is not established: " + component)
            if grant not in _NOTICE_ONLY and grant not in scoped_grants and grant not in {"MPL-1.1", "MPL-2.0"} and not re.fullmatch(r"(?:GPL|LGPL)(?:[-0-9][A-Za-z0-9.+-]*|[0-9]*)", grant):
                obligations.append("custom or unsupported license duties require exact component/grant scope: " + grant)
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
    notice_paths = set(scoped_notices)
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
    # Validated project authorship is separate from upstream/FSF attribution.
    copyright_found = bool(project_notice)
    license_found = bool(gnu) or bool(scoped_grants)
    public_domain_found = False
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
            public_domain_found |= not generic and bool(_PUBLIC_DOMAIN_GRANT.search(text))
            license_found |= bool(_LICENSE_TERMS.search(text))
            normalized = " ".join(text.lower().split())
            zlib_terms = "the origin of this software must not be misrepresented" in normalized and "altered source versions must be plainly marked" in normalized
            if zlib_terms and "MIT" in licenses and "Zlib" not in licenses:
                obligations.append("declared MIT conflicts with supplied zlib-style license terms; resolve using exact component evidence")
            assets.append(("notice", path))
        except OSError as error:
            integrity.append(str(error))
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
        diagnostics.append(str(error))
        if "url" in source and not _http_source_url(url):
            integrity.append("malformed claimed source URL")
        revision = source.get("revision")
        if "revision" in source and (not isinstance(revision, str) or not re.fullmatch(r"[0-9a-fA-F]{40,64}", revision) or not isinstance(url, str) or revision not in unquote(url)):
            integrity.append("invalid or mismatched claimed source revision")
    corresponding = _strings(source.get("corresponding_to"), "source.corresponding_to")
    if any(not _SHA256.fullmatch(digest) for digest in corresponding):
        integrity.append("source.corresponding_to contains invalid ELF sha256")
    member_hashes = {member["sha256"] for member in members}
    missing = sorted(member_hashes - set(corresponding))
    if source_required and missing:
        obligations.append("source correspondence unverified for original ELF sha256: " + ", ".join(missing))
    if corresponding and (missing or set(corresponding) - member_hashes):
        integrity.append("claimed corresponding_to hashes do not match all original bundled ELF hashes")
    if source.get("correspondence_attested") is False:
        message = "supplied upstream sources are not attested as exact downstream corresponding sources"
        (obligations if source_required else diagnostics).append(message)
    archives = source.get("archives", [])
    if not isinstance(archives, list):
        raise ValueError("source.archives must be a list")
    if (source_required or standard_version_required) and not archives:
        obligations.append("selected source-delivery scope requires supplied source archives with sha256, not URL-only offers")
    source_paths = []
    for value in archives:
        try:
            path = _asset(value, base, hashed=True)
            if not _archive_has_source(path):
                raise ValueError("claimed source archive has no regular source files or is not tar/zip: " + str(path))
            assets.append(("source", path))
            source_paths.append(path)
            if source_required and not copyright_found:
                copyright_found |= _archive_has_copyright(path)
        except (ValueError, OSError, tarfile.TarError, zipfile.BadZipFile) as error:
            integrity.append(str(error))
    if source.get("recipe") is not None:
        try:
            assets.append(("recipe", _asset(source["recipe"], base, hashed=True)))
        except (ValueError, OSError) as error:
            integrity.append(str(error))
    elif gnu_required or scoped_source_required:
        obligations.append("GNU/scoped-source exact build recipe/configuration missing")
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
    covered_scripts = source.get("covered_build_scripts", [])
    if not isinstance(covered_scripts, list):
        raise ValueError("source.covered_build_scripts must be a list")
    for value in covered_scripts:
        try:
            assets.append(("build-material", _asset(value, base, hashed=True)))
        except (ValueError, OSError) as error:
            integrity.append(str(error))
    if mpl11_required and source.get("recipe") is None and not covered_scripts:
        obligations.append("MPL-1.1 covered build/install scripts missing from recipe or covered_build_scripts")
    availability = ""
    if source.get("availability_notice") is not None:
        try:
            path = _asset(source["availability_notice"], base, hashed=True)
            availability = path.read_text(encoding="utf-8")
            assets.append(("source-availability-notice", path))
        except (ValueError, OSError, UnicodeError) as error:
            integrity.append(str(error))
    if (mpl_required or standard_version_required) and (
        not re.search(r"\bsource(?:\s+code)?\b", availability, re.IGNORECASE)
        or not re.search(r"\bobtain\b|\bextract\b|\bsupplied\b|\bavailable\b", availability, re.IGNORECASE)
        or (mpl_required and not re.search(r"Mozilla Public License|\bMPL-(?:1\.1|2\.0)\b", availability, re.IGNORECASE))
        or (standard_version_required and not re.search(r"\bStandard Version\b|\bArtistic-1\.0\b", availability, re.IGNORECASE))
        or not source_paths
        or not ("NOTICE.txt" in availability or all(path.name in availability for path in source_paths))
    ):
        obligations.append("MPL/Artistic recipient source-availability notice must identify the applicable source/grant and delivered archives or NOTICE.txt")
    if not notice_paths and not copyright_found:
        obligations.append("exact package license/copyright notices missing; standard SPDX text is insufficient")
    declared_grants = [component for expression in licenses for component in re.split(r"\s+AND\s+", expression)]
    if any(grant in _PUBLIC_DOMAIN_IDS for grant in declared_grants) and not public_domain_found:
        obligations.append("public-domain declaration requires genuine software dedication/release notice")
    attribution_optional = bool(declared_grants) and all(
        (grant in _PUBLIC_DOMAIN_IDS and public_domain_found) or scoped_optional_credit.get(grant, False)
        for grant in declared_grants
    )
    if not copyright_found and not attribution_optional:
        obligations.append("project authorship or upstream copyright notice not established in delivered notices/source headers")
    if not license_found:
        obligations.append("exact package license terms not established in supplied notices")
    for field in ("missing_inputs", "evidence_gaps", "unresolved_public_obligations", "redistribution_obligations"):
        obligations.extend(_strings(entry.get(field), field))
    if integrity or (strict and obligations):
        raise ValueError("; ".join(integrity + obligations))
    source_record = {
        "url": url, "immutable_origin": pinned,
        "correspondence": "attested" if source_paths and corresponding and not missing and not obligations and source.get("correspondence_attested") is not False else "unverified",
        "corresponding_to": sorted(set(corresponding)),
        "provenance_diagnostics": list(dict.fromkeys(diagnostics)),
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
    if diagnostics:
        notice += "Nonblocking provenance limitations:\n" + "".join("- " + item + "\n" for item in source_record["provenance_diagnostics"])
    if availability:
        notice += "Recipient source availability:\n" + availability + "\n"
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
    supplied_notices = entry.get("notices", [])
    if not isinstance(supplied_notices, list):
        raise ValueError("project notices must be a list of exact notice file paths")
    licensed = dict(entry, licenses=[grant["license"]], notices=[*supplied_notices, grant["text"]], origin="newly authored native project implementation")
    record, assets, body = _resolve(licensed, members, base, strict, project_notice=notice)
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

    strict=True rejects missing license-scoped delivery duties, not optional
    scientific provenance limitations or a false copyright inventory attestation.
    Local strict=False records missing duties without inventing rights. Neither
    mode establishes legal clearance. Invalid identities, selectors, claimed
    files, hashes, URLs/revisions and ELF mappings remain fatal.
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
        raise ValueError("Unresolved bundled package obligations:\n- " + "\n- ".join(failures) + "\nSupply exact applicable notices and resolve license/component scope; GNU/MPL source duties require hash-verified mapped archives, GNU build materials and MPL recipient source directions.")
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


def _check_conditional_delivery() -> None:
    """Exercise the policy consumer with real, temporary delivered materials."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        binary = root / "libconditional.so"
        binary.write_bytes(b"conditional boundary fixture")
        digest = _sha256(binary)
        elf = [{"source": str(binary.resolve()), "packaged": "usr/lib/libconditional.so", "sha256": digest}]
        notice = root / "NOTICE"
        notice.write_text("Copyright 2026 Conditional fixture author.\nPermission is hereby granted to use this fixture under its declared license.\n", encoding="utf-8")
        code = root / "fixture.c"
        code.write_text("/* Copyright 2026 Conditional fixture author. */\nint fixture(void) { return 1; }\n", encoding="utf-8")
        archive = root / "covered.tar.gz"
        with tarfile.open(archive, "w:gz") as stream:
            stream.add(code, arcname=code.name)
        recipe = root / "build.sh"
        recipe.write_text("cc -shared fixture.c -o libconditional.so\n", encoding="utf-8")
        directions = root / "SOURCE-NOTICE"
        directions.write_text("MPL-2.0 Covered Software source code is supplied in covered.tar.gz, listed in this package's NOTICE.txt. Extract it to obtain the source under the Mozilla Public License.\n", encoding="utf-8")
        hashed = lambda path: {"path": path.name, "sha256": _sha256(path)}
        entry = {"files": [binary.name], "package": "conditional-fixture", "version": "1.0", "licenses": ["MIT"], "origin": "temporary policy fixture", "notices": [hashed(notice)], "copyright_complete": False}
        complete_source = {"archives": [hashed(archive)], "corresponding_to": [digest], "recipe": hashed(recipe)}
        count = 0

        def deliver(value: dict, accepted: bool = True, *, strict: bool = True, project: dict | None = None) -> list[dict]:
            nonlocal count
            count += 1
            manifest = root / "provenance.json"
            manifest.write_text(json.dumps({"version": 1, "packages": [value] if project is None else [], "project_author": project or {}}), encoding="utf-8")
            target = root / f"delivered-{count}"
            records = None
            try:
                records = bundle_licenses(elf, manifest, target, strict=strict)
            except ValueError:
                if accepted:
                    raise
                assert not target.exists(), "Rejected delivery left a recipient bundle"
            else:
                assert accepted, "Incomplete or invalid claimed delivery was accepted"
                assert records and all((target / item["file"]).is_file() for item in records[0]["files"])
            return records or []

        for license_id in ("MIT", "BSD-2-Clause", "BSD-3-Clause-Clear", "Unicode-3.0", "Apache-2.0", "Zlib"):
            records = deliver({**entry, "licenses": [license_id]})
            assert not records[0]["copyright_complete"] and not records[0]["redistribution_obligations"]
            assert not records[0]["source"]["immutable_origin"]
        optional = {**entry, "source": {"url": "https://example.org/homepage", "correspondence_attested": False}}
        records = deliver(optional)
        assert records[0]["source"]["correspondence"] == "unverified"
        assert records[0]["source"]["provenance_diagnostics"]
        deliver({**entry, "notices": []}, False)
        for strict in (False, True):
            for update in (
                {"notices": [{"path": notice.name, "sha256": "0" * 64}]},
                {"notices": ["missing-notice"]},
                {"notices": "not-a-list"},
                {"source": {"url": "file:///source"}},
                {"source": {"url": ""}},
                {"source": {"url": "https://example.org/1.0", "revision": "bad"}},
                {"source": {"url": "https://example.org/1.0", "revision": None}},
                {"source": {"url": "https://example.org:not-a-port/1.0"}},
                {"source": {"url": "https://example.org/1.0 bad"}},
                {"source": {"corresponding_to": ["0" * 64]}},
                {"source": {"corresponding_to": [digest, "0" * 64]}},
                {"source": {"corresponding_to": "not-a-list"}},
                {"source": {"archives": [{"path": archive.name, "sha256": "0" * 64}]}},
                {"source": {"archives": "not-a-list"}},
                {"source": {"recipe": {"path": "missing-recipe", "sha256": "0" * 64}}},
            ):
                deliver({**entry, **update}, False, strict=strict)
        for licenses in (["MIT OR GPL-2.0-only"], ["MIT WITH unknown-exception"], ["custom"], ["LicenseRef-Unknown"]):
            deliver({**entry, "licenses": licenses}, False)
        scoped = {**entry, "licenses": ["LicenseRef-Fixture"], "license_scopes": [{"license": "LicenseRef-Fixture", "scope": "the included fixture.c component", "delivery": "notice-only", "terms": hashed(notice)}]}
        deliver(scoped)
        imperative = root / "IMPERATIVE-GRANT"
        imperative.write_text("/*\n * Distribute and use freely; there are no restrictions on further\n * dissemination and usage except those imposed by the laws of your\n * country of residence.\n */\n", encoding="utf-8")
        deliver({**scoped, "license_scopes": [{**scoped["license_scopes"][0], "terms": hashed(imperative)}]})
        tcl_grant = root / "TCL-GRANT"
        tcl_grant.write_text("The authors hereby grant permission to use, copy, modify,\n * distribute, and license this software and its documentation for any purpose, provided that existing copyright notices are retained in all copies and that this notice is included verbatim in any distributions.\n", encoding="utf-8")
        deliver({**scoped, "license_scopes": [{**scoped["license_scopes"][0], "terms": hashed(tcl_grant)}]})
        patent_grant = root / "PATENT-GRANT"
        patent_grant.write_text("Google hereby grants to you a perpetual, worldwide, non-exclusive,\nno-charge, irrevocable (except as stated in this section) patent\nlicense to make, have made, use, offer to sell, sell, import,\ntransfer, and otherwise run, modify and propagate the contents of this implementation.\n", encoding="utf-8")
        deliver({**scoped, "license_scopes": [{**scoped["license_scopes"][0], "terms": hashed(patent_grant)}]})
        wrapped_gnu = root / "WRAPPED-GNU-GRANT"
        wrapped_gnu.write_text("/*\n * This component is free software; you can redistribute it\n * under the GNU General Public\n * License, version 3.\n */\n", encoding="utf-8")
        for strict in (False, True):
            deliver({**scoped, "license_scopes": [{**scoped["license_scopes"][0], "terms": hashed(wrapped_gnu)}]}, False, strict=strict)
        for strict in (False, True):
            deliver({**scoped, "license_scopes": [{**scoped["license_scopes"][0], "terms": {"path": imperative.name, "sha256": "0" * 64}}]}, False, strict=strict)
        deliver({**scoped, "notices": []})
        deliver({**scoped, "license_scopes": [*scoped["license_scopes"], {**scoped["license_scopes"][0], "scope": "a second distinct included component sharing the original custom label"}]})
        for strict in (False, True):
            for scope in (
                {**scoped["license_scopes"][0], "terms": {"path": notice.name, "sha256": "0" * 64}},
                {**scoped["license_scopes"][0], "terms": {"path": "missing-terms", "sha256": "0" * 64}},
                {**scoped["license_scopes"][0], "scope": ""},
                {**scoped["license_scopes"][0], "license": "LicenseRef-Unselected"},
                {**scoped["license_scopes"][0], "delivery": "waive-everything"},
            ):
                deliver({**scoped, "license_scopes": [scope]}, False, strict=strict)
            deliver({**entry, "licenses": ["GPL-3.0-only"], "license_scopes": [{**scoped["license_scopes"][0], "license": "GPL-3.0-only"}]}, False, strict=strict)
        pd = root / "PUBLIC-DOMAIN"
        pd.write_text("This fixture software is now in the public domain.\n", encoding="utf-8")
        deliver({**entry, "licenses": ["LicenseRef-PublicDomain"], "notices": [hashed(pd)]})
        deliver({**entry, "licenses": ["LicenseRef-PublicDomain"], "notices": [hashed(notice)]}, False)
        optional_credit = root / "OPTIONAL-CREDIT"
        optional_credit.write_text("By Conditional fixture author (2026).\nUse this source code in any fashion you see fit. Giving me credit where credit is due is optional.\n", encoding="utf-8")
        optional_scope = {"license": "LicenseRef-OptionalCredit", "scope": "included fixture source with explicit optional attribution", "delivery": "notice-only", "terms": hashed(optional_credit)}
        optional_grant = {**entry, "licenses": ["LicenseRef-PublicDomain", optional_scope["license"]], "notices": [hashed(pd)], "license_scopes": [optional_scope]}
        deliver(optional_grant)
        deliver({**optional_grant, "licenses": [optional_scope["license"]], "notices": []})
        deliver({**optional_grant, "licenses": [*optional_grant["licenses"], "MIT"]}, False)
        mandatory_credit = root / "MANDATORY-CREDIT"
        mandatory_credit.write_text("Permission is hereby granted, provided copyright notice is retained.\n", encoding="utf-8")
        deliver({**optional_grant, "license_scopes": [optional_scope, {**optional_scope, "scope": "separate included fixture with mandatory retained notices", "terms": hashed(mandatory_credit)}]}, False)
        deliver({**entry, "licenses": ["MIT"], "notices": [hashed(pd)]}, False)
        scoped_source = {**scoped, "license_scopes": [{**scoped["license_scopes"][0], "delivery": "source-required"}], "source": complete_source}
        deliver(scoped_source)
        for missing in ("archives", "corresponding_to", "recipe"):
            deliver({**scoped_source, "source": {key: value for key, value in complete_source.items() if key != missing}}, False)
        grantless = root / "grantless.txt"
        grantless.write_text("An inventory of component filenames, without any grant.\n", encoding="utf-8")
        for strict in (False, True):
            deliver({**scoped, "license_scopes": [{**scoped["license_scopes"][0], "terms": hashed(grantless)}]}, False, strict=strict)
        standard_notice = root / "STANDARD-VERSION"
        standard_notice.write_text("Artistic-1.0 Standard Version source code is supplied in covered.tar.gz. Extract it to obtain the Standard Version.\n", encoding="utf-8")
        standard = {**entry, "licenses": ["Artistic-1.0"], "license_scopes": [{"license": "Artistic-1.0", "scope": "included unmodified fixture, executable distribution under section 4(a)", "delivery": "standard-version", "terms": hashed(notice)}], "source": {"archives": [hashed(archive)], "availability_notice": hashed(standard_notice), "correspondence_attested": False}}
        records = deliver(standard)
        assert records[0]["source"]["correspondence"] == "unverified"
        for missing in ("archives", "availability_notice"):
            deliver({**standard, "source": {key: value for key, value in standard["source"].items() if key != missing}}, False)
        for missing in ("archives", "corresponding_to", "recipe"):
            deliver({**entry, "licenses": ["GPL-3.0-only"], "source": {key: value for key, value in complete_source.items() if key != missing}}, False)
        gnu = {**entry, "licenses": ["GPL-3.0-only"], "source": complete_source}
        records = deliver(gnu)
        assert not records[0]["source"]["immutable_origin"] and not records[0]["copyright_complete"]
        assert records[0]["source"]["correspondence"] == "attested"
        for grant in ("GPL-3.0-only WITH GCC-exception-3.1", "GPL-2.0-or-later WITH Linux-syscall-note"):
            deliver({**gnu, "licenses": [grant]})
            deliver({**entry, "licenses": [grant]}, False)
        deliver({**entry, "licenses": ["Apache-2.0 WITH LLVM-exception AND BSD-3-Clause"]})
        deliver({**gnu, "source": {**complete_source, "correspondence_attested": False}}, False)
        # Original source headers can supply copyright without a duplicate inventory.
        grant_path = _gnu_licenses(["GPL-3.0-only"])[0][1]
        deliver({**gnu, "notices": [str(grant_path)]})
        mpl_source = {key: value for key, value in complete_source.items() if key != "recipe"}
        mpl_source["availability_notice"] = hashed(directions)
        mpl = {**entry, "licenses": ["MPL-2.0"], "source": mpl_source}
        deliver({**mpl, "source": {"url": "https://example.org/source-1.0.tar.gz"}}, False)
        for missing in ("archives", "corresponding_to", "availability_notice"):
            deliver({**mpl, "source": {key: value for key, value in mpl_source.items() if key != missing}}, False)
        deliver(mpl)
        deliver({**mpl, "source": {**mpl_source, "availability_notice": hashed(notice)}}, False)
        for strict in (False, True):
            deliver({**mpl, "source": {**mpl_source, "availability_notice": {"path": directions.name, "sha256": "0" * 64}}}, False, strict=strict)
        directions.write_text("MPL-1.1 Covered Software source code is supplied in covered.tar.gz. Extract it to obtain the source under the Mozilla Public License.\n", encoding="utf-8")
        mpl11 = {**mpl, "licenses": ["MPL-1.1"], "source": {**mpl_source, "availability_notice": hashed(directions)}}
        deliver(mpl11, False)
        deliver({**mpl11, "source": {**mpl11["source"], "covered_build_scripts": [hashed(recipe)]}})
        for field in ("missing_inputs", "evidence_gaps", "unresolved_public_obligations", "redistribution_obligations"):
            deliver({**entry, field: ["genuine unresolved duty"]}, False)
        native = root / "pusu-game"
        native.write_bytes(binary.read_bytes())
        elf[0].update(source=str(native.resolve()), packaged="usr/bin/pusu-game")
        license_path = root / "GPL"
        license_path.write_bytes(grant_path.read_bytes())
        project = {"executables": ["usr/bin/pusu-game"], "package": "pusu-native", "version": "1.0", "notice": "Newly authored native implementation by the project author; original assets and third-party material excluded.", "notices": [hashed(notice)], "copyright_complete": False, "source": complete_source, "license_grant": {"license": "GPL-3.0-only", "scope": "newly-authored-native-implementation", "text": hashed(license_path)}}
        records = deliver({}, project=project)
        assert records[0]["source"]["kind"] == "licensed-project-source"
        assert records[0]["supplied_provenance"]["notices"] == [hashed(notice), hashed(license_path)]
        # Project-specific human attribution needs no invented upstream author.
        deliver({}, project={**project, "notices": []})


if __name__ == "__main__":
    _check_mapping_and_invalid_manifest()
    _check_conditional_delivery()
