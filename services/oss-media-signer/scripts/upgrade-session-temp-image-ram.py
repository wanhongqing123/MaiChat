#!/usr/bin/env python3
"""Extend the reviewed signer policy only to the temporary image prefix.

Use a temporary ECS administrator identity. Applying creates a new default
policy version after a full local backup and a second read that detects drift.
No AccessKey is created, loaded from files, or printed by this upgrade.
"""

import argparse
import copy
import hashlib
import importlib.util
import json
import os
import time
from pathlib import Path


def provisioning():
    source = Path(__file__).with_name("provision-cloud-drive-ram.py")
    spec = importlib.util.spec_from_file_location("cloud_drive_temp_image_ram_plan", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def target_document(plan):
    document = copy.deepcopy(plan.POLICY_DOCUMENT)
    # Retain the media-bucket deny, bucket actions, and every persistent prefix.
    # Only matching reviewed statements gain the new isolated prefix resources.
    for statement in document["Statement"]:
        prefixes = statement.get("Condition", {}).get("StringLike", {}).get("oss:Prefix")
        if isinstance(prefixes, list) and "session-attachments/*" in prefixes:
            prefixes.extend(["session-temp-images/", "session-temp-images/*"])
        resources = statement.get("Resource")
        if isinstance(resources, list) and (
                f"acs:oss:*:*:{plan.BUCKET}/session-attachments/*" in resources):
            resources.append(f"acs:oss:*:*:{plan.BUCKET}/session-temp-images/*")
    return document


def upgrade(call=None, apply=False, pause=time.sleep, plan=None, backup=None):
    plan = plan or provisioning()
    call = call or plan.call_cli
    identity = call("sts", "GetCallerIdentity")
    fingerprint = "|".join(str(identity.get(field, "")) for field in
                           ("AccountId", "Arn", "IdentityType"))
    if hashlib.sha256(fingerprint.encode()).hexdigest() != plan.ADMIN_IDENTITY_SHA256:
        raise plan.ProvisionError("Unexpected administrator identity")
    user = plan._user(call)
    policy = plan._policy(call)
    if (not user or user.get("UserName") != plan.USER or not policy or
            policy.get("PolicyName") != plan.POLICY or
            (plan.POLICY, "Custom") not in plan._attached_policies(call)):
        raise plan.ProvisionError("Existing signer identity or attached policy is missing")
    current = plan._policy_document(call, policy)
    target = target_document(plan)
    if current == target:
        return "already-updated"
    if current != plan.POLICY_DOCUMENT:
        raise plan.ProvisionError("Existing signer policy differs from the reviewed version")
    if not apply:
        return "ready-to-upgrade"
    if not callable(backup):
        raise plan.ProvisionError("A durable RAM policy backup is required")
    previous = policy["DefaultVersion"]
    backup({"policy_name": plan.POLICY, "default_version": previous,
            "document": copy.deepcopy(current)})
    second = plan._policy(call)
    if (not second or second.get("DefaultVersion") != previous or
            plan._policy_document(call, second) != current):
        raise plan.ProvisionError("RAM policy changed during preflight")
    document = json.dumps(target, separators=(",", ":"))
    try:
        call("ram", "CreatePolicyVersion", "--PolicyName", plan.POLICY,
             "--PolicyDocument", document, "--SetAsDefault", "true")
    except Exception:
        # A lost response may follow a committed version. Read back before
        # reporting failure, without creating another default version.
        if plan._policy_document(call, plan._policy(call)) != target:
            raise
    for _ in range(5):
        updated = plan._policy(call)
        if (updated and updated.get("DefaultVersion") != previous and
                plan._policy_document(call, updated) == target):
            return "updated"
        pause(1)
    raise plan.ProvisionError("Full RAM policy readback was not confirmed as default")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--backup", type=Path)
    arguments = parser.parse_args()
    if arguments.apply != bool(arguments.backup):
        parser.error("--apply requires --backup=<new-file>; dry run does not write a backup")
    if arguments.apply and (not hasattr(os, "geteuid") or os.geteuid() != 0):
        parser.error("Apply must run as the authorized ECS administrator")

    def backup(snapshot):
        descriptor = os.open(arguments.backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(snapshot, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())

    print(upgrade(apply=arguments.apply, backup=backup if arguments.apply else None))


if __name__ == "__main__":
    main()
