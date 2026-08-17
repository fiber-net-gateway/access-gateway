#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ACCESS_SERVER_ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ACCESS_SERVER_ROOT / "scripts" / "verify_cutover_evidence.py"
sys.path.insert(0, str(ACCESS_SERVER_ROOT / "scripts"))

from sync_test_nacos import ConfigEntry, write_dump  # noqa: E402


EVIDENCE_SCHEMA = "access-gateway-cutover-evidence/v1"
REPORT_SCHEMA = "access-gateway-gate-report/v1"
JAVA_REVISION = "22c2bf543b96b52c0ccecd4ceb07d4911c502f45"
GATEWAY_REVISION = "a" * 40
FIBER_REVISION = "b" * 40
RECORD_ID = "production-cutover-2026-08-18"
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


def sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def write_bytes(root: pathlib.Path, relative: str, content: bytes) -> str:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return sha256(content)


def write_json(root: pathlib.Path, relative: str, value: object) -> str:
    content = (
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        + "\n"
    ).encode()
    return write_bytes(root, relative, content)


def read_json(path: pathlib.Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    assert isinstance(value, dict)
    return value


def make_corpus(
    root: pathlib.Path,
    *,
    missing_route: bool = False,
    missing_gray: bool = False,
    empty_route: bool = False,
) -> tuple[str, int]:
    corpus = root / "corpus"
    (corpus / "routes").mkdir(parents=True)
    projects = ["orders", "missing"] if missing_route else ["orders"]
    entries = [
        ConfigEntry(
            data_id="ploto.unified-access.projects",
            group="ACCESS-SERVER",
            content=";".join(projects).encode(),
            config_type="text",
            kind="projects",
        ),
        ConfigEntry(
            data_id="ploto.unified-access.route.orders",
            group="ACCESS-SERVER",
            content=b"" if empty_route else b'{"version":1,"hosts":[]}',
            config_type="json",
            kind="route",
            project="orders",
        ),
    ]
    if not missing_gray:
        entries.append(
            ConfigEntry(
                data_id="ploto.unified-access.gray-match",
                group="DEFAULT_GROUP",
                content=b"{}",
                config_type="json",
                kind="gray",
            )
        )
    digest = write_dump(
        corpus,
        "production-tenant",
        entries,
        ["missing"] if missing_route else [],
        missing_gray,
    )
    return digest, len(projects)


def make_valid_record(
    root: pathlib.Path,
    *,
    missing_route: bool = False,
    missing_gray: bool = False,
    empty_route: bool = False,
) -> pathlib.Path:
    corpus_digest, project_count = make_corpus(
        root,
        missing_route=missing_route,
        missing_gray=missing_gray,
        empty_route=empty_route,
    )
    gates: list[dict[str, object]] = []
    for gate_id in REQUIRED_GATES:
        detail_ref = f"details/{gate_id}.txt"
        detail_digest = write_bytes(root, detail_ref, b"bounded test evidence\n")
        report_ref = f"reports/{gate_id}.json"
        report = {
            "schema": REPORT_SCHEMA,
            "recordId": RECORD_ID,
            "gateId": gate_id,
            "status": "passed",
            "corpusSha256": corpus_digest,
            "revisions": {
                "java": JAVA_REVISION,
                "accessGateway": GATEWAY_REVISION,
                "fiber": FIBER_REVISION,
            },
            "startedAt": "2026-08-18T01:00:00Z",
            "completedAt": "2026-08-18T01:01:00Z",
            "checks": {
                "total": 1,
                "passed": 1,
                "failed": 0,
                "approvedDifferences": 0,
            },
            "approvedDifferenceIds": [],
            "artifacts": [{"path": detail_ref, "sha256": detail_digest}],
        }
        report_digest = write_json(root, report_ref, report)
        gates.append(
            {
                "id": gate_id,
                "status": "passed",
                "report": report_ref,
                "sha256": report_digest,
            }
        )

    evidence = {
        "schema": EVIDENCE_SCHEMA,
        "recordId": RECORD_ID,
        "source": {
            "environment": "production",
            "exportedAt": "2026-08-18T00:00:00Z",
            "redactionRevision": "redactor-v1",
        },
        "revisions": {
            "java": JAVA_REVISION,
            "accessGateway": GATEWAY_REVISION,
            "fiber": FIBER_REVISION,
        },
        "corpus": {
            "manifest": "corpus/content-manifest.json",
            "sha256": corpus_digest,
        },
        "workload": {
            "projectConfigs": project_count,
            "routeItems": 1,
            "requests": 1,
            "lifecycleScenarios": 1,
            "rolloutInstances": 1,
        },
        "gates": gates,
        "approvedDifferences": [],
    }
    manifest = root / "cutover-evidence.json"
    write_json(root, manifest.name, evidence)
    return manifest


def run_verifier(root: pathlib.Path, manifest: pathlib.Path) -> tuple[int, dict[str, object], str]:
    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--manifest",
            str(manifest),
            "--evidence-root",
            str(root),
            "--expected-access-gateway-revision",
            GATEWAY_REVISION,
            "--expected-fiber-revision",
            FIBER_REVISION,
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    output = json.loads(completed.stdout)
    assert isinstance(output, dict)
    return completed.returncode, output, completed.stdout + completed.stderr


class CutoverEvidenceGateTest(unittest.TestCase):
    def test_complete_record_is_met(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)

            status, result, output = run_verifier(root, manifest)

            self.assertEqual(status, 0)
            self.assertEqual(result["status"], "MET")
            self.assertEqual(result["requiredGateCount"], len(REQUIRED_GATES))
            self.assertEqual(result["passedGates"], list(REQUIRED_GATES))
            self.assertEqual(result["errors"], [])
            self.assertNotIn(str(root), output)

    def test_empty_route_candidate_remains_a_valid_corpus_entry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root, empty_route=True)

            status, result, _ = run_verifier(root, manifest)

            self.assertEqual(status, 0)
            self.assertEqual(result["status"], "MET")

    def test_not_run_and_non_production_are_not_met(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            evidence = read_json(manifest)
            evidence["source"]["environment"] = "test"  # type: ignore[index]
            evidence["gates"][0] = {  # type: ignore[index]
                "id": REQUIRED_GATES[0],
                "status": "not_run",
            }
            write_json(root, manifest.name, evidence)

            status, result, _ = run_verifier(root, manifest)

            self.assertEqual(status, 1)
            self.assertEqual(result["status"], "NOT_MET")
            self.assertIn("source_not_production", result["blockers"])
            self.assertIn(
                f"gate_not_run:{REQUIRED_GATES[0]}",
                result["blockers"],
            )

    def test_failed_gate_with_consistent_report_is_not_met(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            evidence = read_json(manifest)
            gate = evidence["gates"][0]  # type: ignore[index]
            report_path = root / gate["report"]  # type: ignore[index]
            report = read_json(report_path)
            report["status"] = "failed"
            report["checks"] = {
                "total": 1,
                "passed": 0,
                "failed": 1,
                "approvedDifferences": 0,
            }
            gate["status"] = "failed"  # type: ignore[index]
            gate["sha256"] = write_json(  # type: ignore[index]
                root,
                str(gate["report"]),  # type: ignore[index]
                report,
            )
            write_json(root, manifest.name, evidence)

            status, result, _ = run_verifier(root, manifest)

            self.assertEqual(status, 1)
            self.assertEqual(result["status"], "NOT_MET")
            self.assertEqual(result["failedGates"], [REQUIRED_GATES[0]])

    def test_digest_mismatch_is_invalid_without_private_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            secret = "private-host-and-route-value"
            detail = root / "details" / f"{REQUIRED_GATES[0]}.txt"
            detail.write_text(secret, encoding="utf-8")
            evidence = read_json(manifest)
            evidence[secret] = "must-not-be-echoed"
            write_json(root, manifest.name, evidence)

            status, result, output = run_verifier(root, manifest)

            self.assertEqual(status, 2)
            self.assertEqual(result["status"], "INVALID")
            self.assertTrue(
                any(error["code"] == "sha256_mismatch" for error in result["errors"])
            )
            self.assertNotIn(secret, output)
            self.assertNotIn(str(root), output)

    def test_traversal_and_symlink_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            evidence = read_json(manifest)
            evidence["gates"][0]["report"] = "../outside.json"  # type: ignore[index]
            write_json(root, manifest.name, evidence)

            status, result, output = run_verifier(root, manifest)
            self.assertEqual(status, 2)
            self.assertTrue(
                any(
                    error["code"] == "invalid_artifact_reference"
                    for error in result["errors"]
                )
            )
            self.assertNotIn(str(root), output)

        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            evidence = read_json(manifest)
            gate = evidence["gates"][0]  # type: ignore[index]
            target = root / gate["report"]  # type: ignore[index]
            link = root / "reports" / "linked.json"
            link.symlink_to(target)
            gate["report"] = "reports/linked.json"  # type: ignore[index]
            write_json(root, manifest.name, evidence)

            status, result, _ = run_verifier(root, manifest)
            self.assertEqual(status, 2)
            self.assertTrue(
                any(error["code"] == "symlink_not_allowed" for error in result["errors"])
            )

    def test_revision_mismatch_and_missing_gate_are_invalid(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            evidence = read_json(manifest)
            evidence["revisions"]["fiber"] = "c" * 40  # type: ignore[index]
            evidence["gates"].pop()  # type: ignore[union-attr]
            write_json(root, manifest.name, evidence)

            status, result, _ = run_verifier(root, manifest)

            self.assertEqual(status, 2)
            codes = {error["code"] for error in result["errors"]}
            self.assertIn("revision_mismatch", codes)
            self.assertIn("missing_gate", codes)

    def test_duplicate_json_keys_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            content = manifest.read_text(encoding="utf-8")
            content = content.replace(
                f'"schema":"{EVIDENCE_SCHEMA}"',
                f'"schema":"{EVIDENCE_SCHEMA}","schema":"{EVIDENCE_SCHEMA}"',
                1,
            )
            manifest.write_text(content, encoding="utf-8")

            status, result, _ = run_verifier(root, manifest)

            self.assertEqual(status, 2)
            self.assertEqual(result["status"], "INVALID")
            self.assertTrue(
                any(error["code"] == "invalid_json" for error in result["errors"])
            )

    def test_gate_report_is_bound_to_corpus_revision_and_export_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            evidence = read_json(manifest)
            gate = evidence["gates"][0]  # type: ignore[index]
            report = read_json(root / gate["report"])  # type: ignore[index]
            report["corpusSha256"] = "c" * 64
            report["revisions"]["accessGateway"] = "d" * 40  # type: ignore[index]
            report["startedAt"] = "2026-08-17T23:59:59Z"
            gate["sha256"] = write_json(  # type: ignore[index]
                root,
                str(gate["report"]),  # type: ignore[index]
                report,
            )
            write_json(root, manifest.name, evidence)

            status, result, _ = run_verifier(root, manifest)

            self.assertEqual(status, 2)
            codes = {error["code"] for error in result["errors"]}
            self.assertIn("corpus_digest_mismatch", codes)
            self.assertIn("revision_mismatch", codes)
            self.assertIn("report_before_export", codes)

    def test_incomplete_corpus_is_not_met(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(
                root,
                missing_route=True,
                missing_gray=True,
            )

            status, result, _ = run_verifier(root, manifest)

            self.assertEqual(status, 1)
            self.assertEqual(result["status"], "NOT_MET")
            self.assertIn("corpus_missing_routes", result["blockers"])
            self.assertIn("corpus_missing_gray", result["blockers"])

    def test_approved_difference_requires_a_hashed_decision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            manifest = make_valid_record(root)
            evidence = read_json(manifest)
            decision_ref = "decisions/compat-001.json"
            decision_digest = write_bytes(root, decision_ref, b'{"approved":true}\n')
            evidence["approvedDifferences"] = [
                {
                    "id": "compat-001",
                    "decision": decision_ref,
                    "sha256": decision_digest,
                }
            ]
            gate = evidence["gates"][0]  # type: ignore[index]
            report = read_json(root / gate["report"])  # type: ignore[index]
            report["checks"] = {
                "total": 1,
                "passed": 0,
                "failed": 0,
                "approvedDifferences": 1,
            }
            report["approvedDifferenceIds"] = ["compat-001"]
            gate["sha256"] = write_json(  # type: ignore[index]
                root,
                str(gate["report"]),  # type: ignore[index]
                report,
            )
            write_json(root, manifest.name, evidence)

            status, result, _ = run_verifier(root, manifest)

            self.assertEqual(status, 0)
            self.assertEqual(result["status"], "MET")

    def test_content_manifest_digest_is_stable_for_the_same_input(self) -> None:
        with tempfile.TemporaryDirectory() as first_temporary:
            with tempfile.TemporaryDirectory() as second_temporary:
                first = pathlib.Path(first_temporary)
                second = pathlib.Path(second_temporary)
                first_digest, _ = make_corpus(first)
                second_digest, _ = make_corpus(second)

                self.assertEqual(first_digest, second_digest)
                self.assertEqual(
                    (first / "corpus" / "content-manifest.sha256").read_text(
                        encoding="ascii"
                    ),
                    f"{first_digest}  content-manifest.json\n",
                )


if __name__ == "__main__":
    unittest.main()
