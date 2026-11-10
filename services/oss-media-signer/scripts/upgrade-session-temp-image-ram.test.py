import copy
import hashlib
import importlib.util
import json
import unittest
from pathlib import Path
from types import SimpleNamespace


path = Path(__file__).with_name("upgrade-session-temp-image-ram.py")
spec = importlib.util.spec_from_file_location("temp_image_ram_upgrade", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class UpgradeTests(unittest.TestCase):
    def fixture(self):
        original = {
            "Version": "1",
            "Statement": [
                {"Effect": "Deny", "Action": "oss:*", "Resource": ["media", "media/*"]},
                {"Effect": "Allow", "Action": "oss:ListObjects", "Resource": "bucket",
                 "Condition": {"StringLike": {"oss:Prefix": [
                     "cloud-drive/", "cloud-drive/*", "session-attachments/", "session-attachments/*"]}}},
                {"Effect": "Allow", "Action": ["oss:PutObject", "oss:GetObject", "oss:DeleteObject",
                                               "oss:ListParts", "oss:AbortMultipartUpload"],
                 "Resource": ["acs:oss:*:*:bucket/cloud-drive/*",
                              "acs:oss:*:*:bucket/session-attachments/*"]},
            ],
        }
        state = {"version": "v1", "document": copy.deepcopy(original),
                 "writes": 0, "backups": [], "events": []}
        identity = {"AccountId": "test", "Arn": "test-arn", "IdentityType": "test-type"}

        def policy(_call):
            state["events"].append("get-policy")
            return {"PolicyName": "policy", "DefaultVersion": state["version"]}

        plan = SimpleNamespace(
            ADMIN_IDENTITY_SHA256=hashlib.sha256(b"test|test-arn|test-type").hexdigest(),
            USER="signer", POLICY="policy", BUCKET="bucket", POLICY_DOCUMENT=original,
            ProvisionError=ValueError, _user=lambda _call: {"UserName": "signer"},
            _policy=policy,
            _policy_document=lambda _call, _policy: copy.deepcopy(state["document"]),
            _attached_policies=lambda _call: {("policy", "Custom")},
        )

        def call(service, action, *args):
            if (service, action) == ("sts", "GetCallerIdentity"):
                return identity
            if (service, action) == ("ram", "CreatePolicyVersion"):
                state["events"].append("put-policy")
                self.assertEqual(args[-2:], ("--SetAsDefault", "true"))
                state["document"] = json.loads(args[args.index("--PolicyDocument") + 1])
                state["version"] = "v2"
                state["writes"] += 1
                return {"PolicyVersion": {"VersionId": "v2"}}
            raise AssertionError((service, action))

        def backup(snapshot):
            state["events"].append("backup")
            state["backups"].append(copy.deepcopy(snapshot))

        return state, plan, call, backup

    def test_plan_extends_only_dedicated_prefix_permissions(self):
        state, plan, _, _ = self.fixture()
        target = module.target_document(plan)
        self.assertEqual(plan.POLICY_DOCUMENT, state["document"])
        self.assertEqual(target["Statement"][0], plan.POLICY_DOCUMENT["Statement"][0])
        self.assertEqual(target["Statement"][1]["Condition"]["StringLike"]["oss:Prefix"], [
            "cloud-drive/", "cloud-drive/*", "session-attachments/", "session-attachments/*",
            "session-temp-images/", "session-temp-images/*"])
        self.assertEqual(target["Statement"][2]["Resource"], [
            "acs:oss:*:*:bucket/cloud-drive/*", "acs:oss:*:*:bucket/session-attachments/*",
            "acs:oss:*:*:bucket/session-temp-images/*"])

    def test_dry_run_and_apply_preserve_policy_and_confirm_default(self):
        state, plan, call, backup = self.fixture()
        self.assertEqual(module.upgrade(call=call, plan=plan), "ready-to-upgrade")
        self.assertEqual(state["writes"], 0)
        state["events"].clear()
        self.assertEqual(module.upgrade(call=call, plan=plan, apply=True,
                                        backup=backup, pause=lambda _: None), "updated")
        self.assertEqual(state["events"], ["get-policy", "backup", "get-policy",
                                           "put-policy", "get-policy"])
        self.assertEqual(state["backups"], [{"policy_name": "policy", "default_version": "v1",
                                             "document": plan.POLICY_DOCUMENT}])
        self.assertEqual(module.upgrade(call=call, plan=plan, apply=True), "already-updated")
        self.assertEqual(state["writes"], 1)

    def test_unknown_policy_is_not_overwritten(self):
        state, plan, call, backup = self.fixture()
        state["document"]["Statement"][0]["Resource"].append("new-restriction")
        with self.assertRaisesRegex(ValueError, "differs"):
            module.upgrade(call=call, plan=plan, apply=True, backup=backup)
        self.assertEqual(state["writes"], 0)

    def test_drift_after_backup_stops_before_write(self):
        state, plan, call, backup = self.fixture()

        def drifting_backup(snapshot):
            backup(snapshot)
            state["version"] = "v3"

        with self.assertRaisesRegex(ValueError, "changed during preflight"):
            module.upgrade(call=call, plan=plan, apply=True, backup=drifting_backup)
        self.assertEqual(state["writes"], 0)

    def test_apply_requires_backup_before_write(self):
        state, plan, call, _ = self.fixture()
        with self.assertRaisesRegex(ValueError, "backup"):
            module.upgrade(call=call, plan=plan, apply=True)
        self.assertEqual(state["writes"], 0)


if __name__ == "__main__":
    unittest.main()
