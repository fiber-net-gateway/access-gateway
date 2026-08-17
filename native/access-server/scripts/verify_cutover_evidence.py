#!/usr/bin/env python3

"""Fail-closed verifier for private Access Gateway cutover evidence.

The verifier never prints artifact paths or artifact content. Exit status 0 means
all required evidence is structurally valid and passed, 1 means a valid record
is incomplete or contains a failed gate, and 2 means the record is invalid.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import stat
import sys
from dataclasses import dataclass
from typing import Any


EVIDENCE_SCHEMA = "access-gateway-cutover-evidence/v1"
CORPUS_SCHEMA = "access-gateway-corpus/v1"
GATE_REPORT_SCHEMA = "access-gateway-gate-report/v1"
RESULT_SCHEMA = "access-gateway-cutover-result/v1"
JAVA_BASELINE = "22c2bf543b96b52c0ccecd4ceb07d4911c502f45"

REQUIRED_GATES = (
    "production_config_compile",
    "p0_p1_compatibility",
    "java_cpp_request_differential",
    "config_hot_update",
    "slow_request_body",
    "connection_disconnect",
    "request_timeout",
    "ordered_shutdown",
    "latency_and_allocation",
    "connection_reuse",
    "memory_stability",
    "fd_stability",
    "activation_evidence",
    "canary_rollout",
    "rollback_drill",
)
WORKLOAD_FIELDS = (
    "projectConfigs",
    "routeItems",
    "requests",
    "lifecycleScenarios",
    "rolloutInstances",
)

HEX_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
REVISION = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
SAFE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}\Z")
TIMESTAMP = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z\Z")
MAX_ERRORS = 64
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_REPORT_BYTES = 1024 * 1024
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024
PROJECTS_DATA_ID = "ploto.unified-access.projects"
ROUTE_DATA_ID_PREFIX = "ploto.unified-access.route."
ROUTE_GROUP = "ACCESS-SERVER"
GRAY_DATA_ID = "ploto.unified-access.gray-match"
GRAY_GROUP = "DEFAULT_GROUP"


@dataclass(frozen=True)
class ValidationIssue:
    code: str
    field: str


class DuplicateJsonKey(ValueError):
    pass


def strict_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKey
        result[key] = value
    return result


def reject_json_constant(_: str) -> None:
    raise ValueError


def canonical_json_bytes(value: object) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")


def canonical_result(
    status: str,
    record_id: str | None,
    passed: list[str],
    failed: list[str],
    not_run: list[str],
    blockers: list[str],
    issues: list[ValidationIssue],
) -> dict[str, Any]:
    return {
        "schema": RESULT_SCHEMA,
        "recordId": record_id,
        "status": status,
        "requiredGateCount": len(REQUIRED_GATES),
        "passedGates": passed,
        "failedGates": failed,
        "notRunGates": not_run,
        "blockers": blockers,
        "errors": [
            {
                "code": issue.code,
                "field": issue.field,
            }
            for issue in issues
        ],
    }


class CutoverEvidenceVerifier:
    def __init__(
        self,
        evidence_root: pathlib.Path,
        expected_gateway_revision: str,
        expected_fiber_revision: str,
    ) -> None:
        self._root_input = evidence_root
        self._root: pathlib.Path | None = None
        self._expected_gateway_revision = expected_gateway_revision
        self._expected_fiber_revision = expected_fiber_revision
        self._issues: list[ValidationIssue] = []
        self._blockers: set[str] = set()
        self._file_cache: dict[str, tuple[pathlib.Path, str, int]] = {}
        self._record_id: str | None = None
        self._exported_at: datetime.datetime | None = None
        self._manifest_revisions: dict[str, str] = {}
        self._corpus_digest: str | None = None
        self._passed: set[str] = set()
        self._failed: set[str] = set()
        self._not_run: set[str] = set()

    def verify(self, manifest_path: pathlib.Path) -> tuple[dict[str, Any], int]:
        self._prepare_root()
        manifest = self._read_standalone_json(manifest_path, "$", MAX_MANIFEST_BYTES)
        if manifest is not None:
            self._verify_manifest(manifest)

        passed = [gate for gate in REQUIRED_GATES if gate in self._passed]
        failed = [gate for gate in REQUIRED_GATES if gate in self._failed]
        not_run = [gate for gate in REQUIRED_GATES if gate in self._not_run]
        if self._issues:
            status = "INVALID"
            exit_status = 2
        elif self._blockers:
            status = "NOT_MET"
            exit_status = 1
        else:
            status = "MET"
            exit_status = 0
        return (
            canonical_result(
                status,
                self._record_id,
                passed,
                failed,
                not_run,
                sorted(self._blockers),
                self._issues,
            ),
            exit_status,
        )

    def _issue(self, code: str, field: str) -> None:
        if len(self._issues) < MAX_ERRORS:
            self._issues.append(ValidationIssue(code=code, field=field))

    def _block(self, code: str) -> None:
        self._blockers.add(code)

    def _prepare_root(self) -> None:
        try:
            root_stat = self._root_input.lstat()
            if stat.S_ISLNK(root_stat.st_mode):
                self._issue("symlink_not_allowed", "evidenceRoot")
                return
            if not stat.S_ISDIR(root_stat.st_mode):
                self._issue("not_a_directory", "evidenceRoot")
                return
            self._root = self._root_input.resolve(strict=True)
        except OSError:
            self._issue("unreadable", "evidenceRoot")

    def _read_standalone_json(
        self,
        path: pathlib.Path,
        field: str,
        maximum_bytes: int,
    ) -> dict[str, Any] | None:
        try:
            path_stat = path.lstat()
            if stat.S_ISLNK(path_stat.st_mode):
                self._issue("symlink_not_allowed", field)
                return None
            if not stat.S_ISREG(path_stat.st_mode):
                self._issue("not_a_file", field)
                return None
            if path_stat.st_size <= 0 or path_stat.st_size > maximum_bytes:
                self._issue("invalid_file_size", field)
                return None
            content = path.read_bytes()
        except OSError:
            self._issue("unreadable", field)
            return None
        return self._parse_json(content, field)

    def _parse_json(self, content: bytes, field: str) -> dict[str, Any] | None:
        try:
            decoded = content.decode("utf-8")
            value = json.loads(
                decoded,
                object_pairs_hook=strict_json_object,
                parse_constant=reject_json_constant,
            )
        except (UnicodeDecodeError, json.JSONDecodeError, DuplicateJsonKey, ValueError):
            self._issue("invalid_json", field)
            return None
        if not isinstance(value, dict):
            self._issue("expected_object", field)
            return None
        return value

    def _object(
        self,
        value: Any,
        field: str,
        required: set[str],
        optional: set[str] | None = None,
    ) -> dict[str, Any] | None:
        if not isinstance(value, dict):
            self._issue("expected_object", field)
            return None
        optional = optional or set()
        keys = set(value)
        for missing in sorted(required - keys):
            self._issue("missing_field", f"{field}.{missing}")
        for _ in sorted(keys - required - optional):
            self._issue("unknown_field", f"{field}.*")
        return value

    def _safe_string(self, value: Any, field: str) -> str | None:
        if not isinstance(value, str) or SAFE_ID.fullmatch(value) is None:
            self._issue("invalid_safe_string", field)
            return None
        return value

    def _digest(self, value: Any, field: str) -> str | None:
        if not isinstance(value, str) or HEX_SHA256.fullmatch(value) is None:
            self._issue("invalid_sha256", field)
            return None
        return value

    def _revision(self, value: Any, field: str) -> str | None:
        if not isinstance(value, str) or REVISION.fullmatch(value) is None:
            self._issue("invalid_revision", field)
            return None
        return value

    def _timestamp(self, value: Any, field: str) -> datetime.datetime | None:
        if not isinstance(value, str) or TIMESTAMP.fullmatch(value) is None:
            self._issue("invalid_timestamp", field)
            return None
        try:
            return datetime.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(
                tzinfo=datetime.timezone.utc
            )
        except ValueError:
            self._issue("invalid_timestamp", field)
            return None

    def _non_negative_integer(self, value: Any, field: str) -> int | None:
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            self._issue("invalid_non_negative_integer", field)
            return None
        return value

    def _verify_manifest(self, manifest: dict[str, Any]) -> None:
        required = {
            "schema",
            "recordId",
            "source",
            "revisions",
            "corpus",
            "workload",
            "gates",
            "approvedDifferences",
        }
        self._object(manifest, "$", required)
        if manifest.get("schema") != EVIDENCE_SCHEMA:
            self._issue("unsupported_schema", "$.schema")

        record_id = self._safe_string(manifest.get("recordId"), "$.recordId")
        if record_id is not None:
            self._record_id = record_id

        self._verify_source(manifest.get("source"))
        self._verify_revisions(manifest.get("revisions"))
        workload = self._verify_workload(manifest.get("workload"))
        self._verify_corpus(manifest.get("corpus"), workload)
        approvals = self._verify_approvals(manifest.get("approvedDifferences"))
        self._verify_gates(manifest.get("gates"), approvals)

    def _verify_source(self, value: Any) -> None:
        source = self._object(
            value,
            "$.source",
            {"environment", "exportedAt", "redactionRevision"},
        )
        if source is None:
            return
        environment = self._safe_string(source.get("environment"), "$.source.environment")
        if environment is not None and environment != "production":
            self._block("source_not_production")
        self._exported_at = self._timestamp(
            source.get("exportedAt"), "$.source.exportedAt"
        )
        self._safe_string(source.get("redactionRevision"), "$.source.redactionRevision")

    def _verify_revisions(self, value: Any) -> None:
        revisions = self._object(
            value,
            "$.revisions",
            {"java", "accessGateway", "fiber"},
        )
        if revisions is None:
            return
        java_revision = self._revision(revisions.get("java"), "$.revisions.java")
        gateway_revision = self._revision(
            revisions.get("accessGateway"), "$.revisions.accessGateway"
        )
        fiber_revision = self._revision(revisions.get("fiber"), "$.revisions.fiber")
        for name, revision in (
            ("java", java_revision),
            ("accessGateway", gateway_revision),
            ("fiber", fiber_revision),
        ):
            if revision is not None:
                self._manifest_revisions[name] = revision
        if java_revision is not None and java_revision != JAVA_BASELINE:
            self._issue("revision_mismatch", "$.revisions.java")
        if (
            gateway_revision is not None
            and gateway_revision != self._expected_gateway_revision
        ):
            self._issue("revision_mismatch", "$.revisions.accessGateway")
        if fiber_revision is not None and fiber_revision != self._expected_fiber_revision:
            self._issue("revision_mismatch", "$.revisions.fiber")

    def _verify_workload(self, value: Any) -> dict[str, int] | None:
        workload = self._object(value, "$.workload", set(WORKLOAD_FIELDS))
        if workload is None:
            return None
        result: dict[str, int] = {}
        for name in WORKLOAD_FIELDS:
            count = self._non_negative_integer(workload.get(name), f"$.workload.{name}")
            if count is not None:
                result[name] = count
                if count == 0:
                    self._block(f"empty_workload:{name}")
        return result

    def _verify_corpus(self, value: Any, workload: dict[str, int] | None) -> None:
        corpus = self._object(value, "$.corpus", {"manifest", "sha256"})
        if corpus is None:
            return
        digest = self._digest(corpus.get("sha256"), "$.corpus.sha256")
        self._corpus_digest = digest
        manifest_reference = corpus.get("manifest")
        path = self._verify_artifact(
            manifest_reference,
            digest,
            "$.corpus.manifest",
            MAX_MANIFEST_BYTES,
        )
        if path is None:
            return
        try:
            content = path.read_bytes()
        except OSError:
            self._issue("unreadable", "$.corpus.manifest")
            return
        manifest = self._parse_json(content, "$.corpus.manifest")
        if manifest is not None:
            if content != canonical_json_bytes(manifest):
                self._issue("noncanonical_json", "$.corpus.manifest")
            manifest_parent = pathlib.PurePosixPath(".")
            if isinstance(manifest_reference, str):
                manifest_parent = pathlib.PurePosixPath(manifest_reference).parent
            self._verify_content_manifest(manifest, workload, manifest_parent)

    def _verify_content_manifest(
        self,
        manifest: dict[str, Any],
        workload: dict[str, int] | None,
        manifest_parent: pathlib.PurePosixPath,
    ) -> None:
        required = {
            "schema",
            "sourceTenantSha256",
            "configs",
            "missingRouteCount",
            "missingGrayMatch",
        }
        self._object(manifest, "$.corpus.content", required)
        if manifest.get("schema") != CORPUS_SCHEMA:
            self._issue("unsupported_schema", "$.corpus.content.schema")
        self._digest(
            manifest.get("sourceTenantSha256"),
            "$.corpus.content.sourceTenantSha256",
        )

        missing_routes = self._non_negative_integer(
            manifest.get("missingRouteCount"),
            "$.corpus.content.missingRouteCount",
        )
        missing_gray = manifest.get("missingGrayMatch")
        if not isinstance(missing_gray, bool):
            self._issue("expected_boolean", "$.corpus.content.missingGrayMatch")
            missing_gray = None
        if missing_routes is not None and missing_routes > 0:
            self._block("corpus_missing_routes")
        if missing_gray:
            self._block("corpus_missing_gray")

        configs = manifest.get("configs")
        if not isinstance(configs, list) or not configs:
            self._issue("expected_non_empty_array", "$.corpus.content.configs")
            return
        kind_counts = {"projects": 0, "route": 0, "gray": 0}
        seen_files: set[str] = set()
        seen_config_keys: set[tuple[str, str]] = set()
        for index, raw_config in enumerate(configs):
            field = f"$.corpus.content.configs[{index}]"
            config = self._object(
                raw_config,
                field,
                {"kind", "dataId", "group", "type", "file", "bytes", "sha256"},
            )
            if config is None:
                continue
            kind = config.get("kind")
            if kind not in kind_counts:
                self._issue("invalid_config_kind", f"{field}.kind")
            else:
                kind_counts[kind] += 1
            for metadata_name in ("dataId", "group", "type"):
                metadata = config.get(metadata_name)
                if not isinstance(metadata, str) or not metadata:
                    self._issue(
                        "invalid_non_empty_string",
                        f"{field}.{metadata_name}",
                    )
            data_id = config.get("dataId")
            group = config.get("group")
            config_type = config.get("type")
            if isinstance(data_id, str) and isinstance(group, str):
                config_key = (data_id, group)
                if config_key in seen_config_keys:
                    self._issue("duplicate_config_key", f"{field}.dataId")
                seen_config_keys.add(config_key)
            contract_matches = False
            if kind == "projects":
                contract_matches = (
                    data_id == PROJECTS_DATA_ID
                    and group == ROUTE_GROUP
                    and config_type == "text"
                )
            elif kind == "route":
                contract_matches = (
                    isinstance(data_id, str)
                    and data_id.startswith(ROUTE_DATA_ID_PREFIX)
                    and len(data_id) > len(ROUTE_DATA_ID_PREFIX)
                    and group == ROUTE_GROUP
                    and config_type == "json"
                )
            elif kind == "gray":
                contract_matches = (
                    data_id == GRAY_DATA_ID
                    and group == GRAY_GROUP
                    and config_type == "json"
                )
            if kind in kind_counts and not contract_matches:
                self._issue("config_contract_mismatch", f"{field}.kind")
            file_ref = config.get("file")
            rooted_file_ref: Any = file_ref
            if isinstance(file_ref, str):
                if file_ref in seen_files:
                    self._issue("duplicate_artifact", f"{field}.file")
                seen_files.add(file_ref)
                rooted_file_ref = str(manifest_parent / pathlib.PurePosixPath(file_ref))
            byte_count = self._non_negative_integer(config.get("bytes"), f"{field}.bytes")
            file_digest = self._digest(config.get("sha256"), f"{field}.sha256")
            path = self._verify_artifact(
                rooted_file_ref,
                file_digest,
                f"{field}.file",
                MAX_ARTIFACT_BYTES,
                allow_empty=True,
            )
            if path is not None and byte_count is not None:
                try:
                    if path.stat().st_size != byte_count:
                        self._issue("file_size_mismatch", f"{field}.bytes")
                except OSError:
                    self._issue("unreadable", f"{field}.file")

        if kind_counts["projects"] != 1:
            self._issue("invalid_project_list_count", "$.corpus.content.configs")
        expected_gray_count = 0 if missing_gray else 1
        if missing_gray is not None and kind_counts["gray"] != expected_gray_count:
            self._issue("inconsistent_gray_state", "$.corpus.content.configs")
        if workload is not None and "projectConfigs" in workload and missing_routes is not None:
            if kind_counts["route"] + missing_routes != workload["projectConfigs"]:
                self._issue("project_count_mismatch", "$.workload.projectConfigs")

    def _verify_approvals(self, value: Any) -> set[str]:
        if not isinstance(value, list):
            self._issue("expected_array", "$.approvedDifferences")
            return set()
        approvals: set[str] = set()
        for index, raw_approval in enumerate(value):
            field = f"$.approvedDifferences[{index}]"
            approval = self._object(
                raw_approval,
                field,
                {"id", "decision", "sha256"},
            )
            if approval is None:
                continue
            approval_id = self._safe_string(approval.get("id"), f"{field}.id")
            if approval_id is not None:
                if approval_id in approvals:
                    self._issue("duplicate_approval", f"{field}.id")
                approvals.add(approval_id)
            digest = self._digest(approval.get("sha256"), f"{field}.sha256")
            self._verify_artifact(
                approval.get("decision"),
                digest,
                f"{field}.decision",
                MAX_REPORT_BYTES,
            )
        return approvals

    def _verify_gates(self, value: Any, approvals: set[str]) -> None:
        if not isinstance(value, list):
            self._issue("expected_array", "$.gates")
            return
        seen: set[str] = set()
        referenced_approvals: set[str] = set()
        for index, raw_gate in enumerate(value):
            field = f"$.gates[{index}]"
            if not isinstance(raw_gate, dict):
                self._issue("expected_object", field)
                continue
            gate_id = self._safe_string(raw_gate.get("id"), f"{field}.id")
            status_value = raw_gate.get("status")
            if status_value not in {"passed", "failed", "not_run"}:
                self._issue("invalid_gate_status", f"{field}.status")
                status_value = None
            expected_fields = {"id", "status"}
            if status_value in {"passed", "failed"}:
                expected_fields.update({"report", "sha256"})
            self._object(raw_gate, field, expected_fields)
            if gate_id is None:
                continue
            if gate_id not in REQUIRED_GATES:
                self._issue("unknown_gate", f"{field}.id")
                continue
            if gate_id in seen:
                self._issue("duplicate_gate", f"{field}.id")
                continue
            seen.add(gate_id)
            if status_value == "not_run":
                self._not_run.add(gate_id)
                self._block(f"gate_not_run:{gate_id}")
                continue
            if status_value not in {"passed", "failed"}:
                continue

            digest = self._digest(raw_gate.get("sha256"), f"{field}.sha256")
            report_path = self._verify_artifact(
                raw_gate.get("report"),
                digest,
                f"{field}.report",
                MAX_REPORT_BYTES,
            )
            if report_path is not None:
                try:
                    report_content = report_path.read_bytes()
                except OSError:
                    self._issue("unreadable", f"{field}.report")
                else:
                    report = self._parse_json(report_content, f"{field}.report")
                    if report is not None:
                        referenced_approvals.update(
                            self._verify_gate_report(
                                report,
                                gate_id,
                                status_value,
                                f"{field}.report",
                                approvals,
                            )
                        )
            if status_value == "passed":
                self._passed.add(gate_id)
            else:
                self._failed.add(gate_id)
                self._block(f"gate_failed:{gate_id}")

        for missing in REQUIRED_GATES:
            if missing not in seen:
                self._issue("missing_gate", f"$.gates.{missing}")
        for _ in sorted(approvals - referenced_approvals):
            self._issue("unused_approval", "$.approvedDifferences")

    def _verify_gate_report(
        self,
        report: dict[str, Any],
        gate_id: str,
        gate_status: str,
        field: str,
        approvals: set[str],
    ) -> set[str]:
        required = {
            "schema",
            "recordId",
            "gateId",
            "status",
            "corpusSha256",
            "revisions",
            "startedAt",
            "completedAt",
            "checks",
            "approvedDifferenceIds",
            "artifacts",
        }
        self._object(report, field, required)
        if report.get("schema") != GATE_REPORT_SCHEMA:
            self._issue("unsupported_schema", f"{field}.schema")
        if report.get("recordId") != self._record_id:
            self._issue("record_id_mismatch", f"{field}.recordId")
        if report.get("gateId") != gate_id:
            self._issue("gate_id_mismatch", f"{field}.gateId")
        if report.get("status") != gate_status:
            self._issue("gate_status_mismatch", f"{field}.status")
        report_corpus_digest = self._digest(
            report.get("corpusSha256"), f"{field}.corpusSha256"
        )
        if (
            report_corpus_digest is not None
            and self._corpus_digest is not None
            and report_corpus_digest != self._corpus_digest
        ):
            self._issue("corpus_digest_mismatch", f"{field}.corpusSha256")
        report_revisions = self._object(
            report.get("revisions"),
            f"{field}.revisions",
            {"java", "accessGateway", "fiber"},
        )
        if report_revisions is not None:
            for name in ("java", "accessGateway", "fiber"):
                report_revision = self._revision(
                    report_revisions.get(name), f"{field}.revisions.{name}"
                )
                if (
                    report_revision is not None
                    and name in self._manifest_revisions
                    and report_revision != self._manifest_revisions[name]
                ):
                    self._issue(
                        "revision_mismatch",
                        f"{field}.revisions.{name}",
                    )
        started = self._timestamp(report.get("startedAt"), f"{field}.startedAt")
        completed = self._timestamp(report.get("completedAt"), f"{field}.completedAt")
        if (
            started is not None
            and self._exported_at is not None
            and started < self._exported_at
        ):
            self._issue("report_before_export", f"{field}.startedAt")
        if started is not None and completed is not None and completed < started:
            self._issue("invalid_time_order", f"{field}.completedAt")

        checks = self._object(
            report.get("checks"),
            f"{field}.checks",
            {"total", "passed", "failed", "approvedDifferences"},
        )
        counts: dict[str, int] = {}
        if checks is not None:
            for name in ("total", "passed", "failed", "approvedDifferences"):
                count = self._non_negative_integer(
                    checks.get(name),
                    f"{field}.checks.{name}",
                )
                if count is not None:
                    counts[name] = count
            if len(counts) == 4:
                if counts["total"] == 0:
                    self._issue("empty_gate_report", f"{field}.checks.total")
                if counts["total"] != (
                    counts["passed"]
                    + counts["failed"]
                    + counts["approvedDifferences"]
                ):
                    self._issue("check_count_mismatch", f"{field}.checks")
                if gate_status == "passed" and counts["failed"] != 0:
                    self._issue("passed_gate_has_failures", f"{field}.checks.failed")
                if gate_status == "failed" and counts["failed"] == 0:
                    self._issue("failed_gate_has_no_failures", f"{field}.checks.failed")

        approved_ids_value = report.get("approvedDifferenceIds")
        approved_ids: set[str] = set()
        if not isinstance(approved_ids_value, list):
            self._issue("expected_array", f"{field}.approvedDifferenceIds")
        else:
            for index, raw_id in enumerate(approved_ids_value):
                approved_id = self._safe_string(
                    raw_id,
                    f"{field}.approvedDifferenceIds[{index}]",
                )
                if approved_id is None:
                    continue
                if approved_id in approved_ids:
                    self._issue(
                        "duplicate_approval_reference",
                        f"{field}.approvedDifferenceIds[{index}]",
                    )
                approved_ids.add(approved_id)
                if approved_id not in approvals:
                    self._issue(
                        "unknown_approval_reference",
                        f"{field}.approvedDifferenceIds[{index}]",
                    )
        if "approvedDifferences" in counts and len(approved_ids) != counts["approvedDifferences"]:
            self._issue(
                "approval_count_mismatch",
                f"{field}.approvedDifferenceIds",
            )

        artifacts = report.get("artifacts")
        if not isinstance(artifacts, list) or not artifacts:
            self._issue("expected_non_empty_array", f"{field}.artifacts")
        else:
            seen_artifacts: set[str] = set()
            for index, raw_artifact in enumerate(artifacts):
                artifact_field = f"{field}.artifacts[{index}]"
                artifact = self._object(
                    raw_artifact,
                    artifact_field,
                    {"path", "sha256"},
                )
                if artifact is None:
                    continue
                artifact_ref = artifact.get("path")
                if isinstance(artifact_ref, str):
                    if artifact_ref in seen_artifacts:
                        self._issue("duplicate_artifact", f"{artifact_field}.path")
                    seen_artifacts.add(artifact_ref)
                artifact_digest = self._digest(
                    artifact.get("sha256"),
                    f"{artifact_field}.sha256",
                )
                self._verify_artifact(
                    artifact_ref,
                    artifact_digest,
                    f"{artifact_field}.path",
                    MAX_ARTIFACT_BYTES,
                )
        return approved_ids

    def _verify_artifact(
        self,
        raw_reference: Any,
        expected_digest: str | None,
        field: str,
        maximum_bytes: int,
        *,
        allow_empty: bool = False,
    ) -> pathlib.Path | None:
        if self._root is None:
            return None
        if not isinstance(raw_reference, str) or not raw_reference or len(raw_reference) > 512:
            self._issue("invalid_artifact_reference", field)
            return None
        if "\\" in raw_reference:
            self._issue("invalid_artifact_reference", field)
            return None
        relative = pathlib.PurePosixPath(raw_reference)
        if (
            relative.is_absolute()
            or str(relative) != raw_reference
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            self._issue("invalid_artifact_reference", field)
            return None

        cached = self._file_cache.get(raw_reference)
        if cached is None:
            candidate = self._root
            try:
                for part in relative.parts:
                    candidate = candidate / part
                    candidate_stat = candidate.lstat()
                    if stat.S_ISLNK(candidate_stat.st_mode):
                        self._issue("symlink_not_allowed", field)
                        return None
                resolved = candidate.resolve(strict=True)
                if not resolved.is_relative_to(self._root):
                    self._issue("artifact_outside_root", field)
                    return None
                resolved_stat = resolved.stat()
                if not stat.S_ISREG(resolved_stat.st_mode):
                    self._issue("not_a_file", field)
                    return None
                if (
                    (resolved_stat.st_size == 0 and not allow_empty)
                    or resolved_stat.st_size > maximum_bytes
                ):
                    self._issue("invalid_file_size", field)
                    return None
                digest = hashlib.sha256()
                with resolved.open("rb") as stream:
                    while chunk := stream.read(1024 * 1024):
                        digest.update(chunk)
                cached = (resolved, digest.hexdigest(), resolved_stat.st_size)
                self._file_cache[raw_reference] = cached
            except OSError:
                self._issue("unreadable", field)
                return None
        path, actual_digest, size = cached
        if (size == 0 and not allow_empty) or size > maximum_bytes:
            self._issue("invalid_file_size", field)
            return None
        if expected_digest is not None and actual_digest != expected_digest:
            self._issue("sha256_mismatch", field)
            return None
        return path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify private Access Gateway cutover evidence without printing its content."
    )
    parser.add_argument(
        "--manifest",
        default=os.environ.get("ACCESS_SERVER_CUTOVER_EVIDENCE_MANIFEST", ""),
    )
    parser.add_argument(
        "--evidence-root",
        default=os.environ.get("ACCESS_SERVER_CUTOVER_EVIDENCE_ROOT", ""),
    )
    parser.add_argument(
        "--expected-access-gateway-revision",
        default=os.environ.get("ACCESS_SERVER_CUTOVER_GATEWAY_REVISION", ""),
    )
    parser.add_argument(
        "--expected-fiber-revision",
        default=os.environ.get("ACCESS_SERVER_CUTOVER_FIBER_REVISION", ""),
    )
    return parser.parse_args()


def invalid_arguments(fields: list[str]) -> int:
    issues = [ValidationIssue(code="missing_or_invalid_argument", field=field) for field in fields]
    result = canonical_result("INVALID", None, [], [], [], [], issues)
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 2


def main() -> int:
    args = parse_args()
    invalid: list[str] = []
    if not args.manifest:
        invalid.append("manifest")
    if not args.expected_access_gateway_revision or REVISION.fullmatch(
        args.expected_access_gateway_revision
    ) is None:
        invalid.append("expectedAccessGatewayRevision")
    if not args.expected_fiber_revision or REVISION.fullmatch(
        args.expected_fiber_revision
    ) is None:
        invalid.append("expectedFiberRevision")
    if invalid:
        return invalid_arguments(invalid)

    manifest_path = pathlib.Path(args.manifest)
    evidence_root = (
        pathlib.Path(args.evidence_root)
        if args.evidence_root
        else manifest_path.parent
    )
    verifier = CutoverEvidenceVerifier(
        evidence_root,
        args.expected_access_gateway_revision,
        args.expected_fiber_revision,
    )
    try:
        result, exit_status = verifier.verify(manifest_path)
    except Exception:  # Defensive boundary: never leak private exception values.
        result = canonical_result(
            "INVALID",
            None,
            [],
            [],
            [],
            [],
            [ValidationIssue(code="internal_validation_error", field="$")],
        )
        exit_status = 2
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return exit_status


if __name__ == "__main__":
    sys.exit(main())
