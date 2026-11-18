#!/usr/bin/env python3
"""Derive the `wire` member of a revision profile from an official MCP schema.json.

The `wire` member holds what the published schema states about a revision: its
requests and notifications with their senders, the requests a server may ask of
a client inside a result, and the properties of every object type. A profile's
other members are written by hand from the specification's prose.

    profile_wire.py SCHEMA                  print the wire member
    profile_wire.py SCHEMA --write PROFILE  replace the wire member of PROFILE
    profile_wire.py SCHEMA --check PROFILE  exit 1 unless PROFILE's wire member matches
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

# The unions the schema uses to say who sends a message.
SENDER_UNIONS = (
    ("ClientRequest", "requests", "client"),
    ("ServerRequest", "requests", "server"),
    ("ClientNotification", "notifications", "client"),
    ("ServerNotification", "notifications", "server"),
)
# The union of requests a server places in a result for the client to answer.
INPUT_REQUEST_UNION = "InputRequest"


def ref_name(node):
    ref = node.get("$ref") if isinstance(node, dict) else None
    return ref.rsplit("/", 1)[-1] if ref else None


def constant_method(definition):
    return definition.get("properties", {}).get("method", {}).get("const")


def union_members(definitions, union):
    """The message types of a union. The schema writes a union of several types
    as anyOf, of one type as a reference or as a copy of that type, and omits a
    union of none."""
    node = definitions.get(union)
    if node is None:
        return []
    alternatives = node.get("anyOf") or node.get("oneOf")
    if alternatives is not None:
        names = [ref_name(alternative) for alternative in alternatives]
        if None in names:
            raise SystemExit(f"{union} has a member that is not a reference")
        return names
    if ref_name(node):
        return [ref_name(node)]
    method = constant_method(node)
    if method is None:
        raise SystemExit(f"{union} is neither a union nor a message type")
    same = [
        name
        for name, definition in definitions.items()
        if name != union and constant_method(definition) == method
    ]
    if len(same) != 1:
        raise SystemExit(f"{union} copies a message type that cannot be identified: {same}")
    return same


def method_of(definitions, name):
    method = constant_method(definitions[name])
    if method is None:
        raise SystemExit(f"{name} has no constant method")
    return method


def params_of(definitions, name):
    definition = definitions[name]
    params = definition.get("properties", {}).get("params")
    entry = {"params": None, "params_required": []}
    if params is None:
        return entry
    target = ref_name(params)
    shape = definitions[target] if target else params
    entry["params"] = target
    entry["params_required"] = sorted(shape.get("required", []))
    return entry


def derive(schema_path):
    raw = Path(schema_path).read_bytes()
    document = json.loads(raw)
    definitions = document.get("$defs")
    if definitions is None:
        definitions = document.get("definitions")
    if definitions is None:
        raise SystemExit("the schema has neither $defs nor definitions")

    tables = {"requests": {}, "notifications": {}}
    listed = {union for union, _, _ in SENDER_UNIONS} | {INPUT_REQUEST_UNION}
    for union, table, sender in SENDER_UNIONS:
        for name in union_members(definitions, union):
            listed.add(name)
            method = method_of(definitions, name)
            entry = tables[table].get(method)
            if entry is None:
                entry = {"from": [], "type": name, **params_of(definitions, name)}
                tables[table][method] = entry
            elif entry["type"] != name:
                raise SystemExit(f"{method} is defined by both {entry['type']} and {name}")
            entry["from"].append(sender)

    input_requests = {}
    for name in union_members(definitions, INPUT_REQUEST_UNION):
        listed.add(name)
        input_requests[method_of(definitions, name)] = {"type": name, **params_of(definitions, name)}

    # A type that names a method and belongs to no union would be a message the
    # tables miss; the schema has none, and a new one must be looked at.
    unlisted = sorted(
        name
        for name, definition in definitions.items()
        if constant_method(definition) is not None and name not in listed
    )

    types = {
        name: {
            "properties": sorted(definition["properties"]),
            "required": sorted(definition.get("required", [])),
        }
        for name, definition in definitions.items()
        if isinstance(definition.get("properties"), dict)
    }

    return {
        "schema": {"dialect": document.get("$schema"), "sha256": hashlib.sha256(raw).hexdigest()},
        "requests": dict(sorted(tables["requests"].items())),
        "notifications": dict(sorted(tables["notifications"].items())),
        "input_requests": dict(sorted(input_requests.items())),
        "unlisted_message_types": unlisted,
        "types": dict(sorted(types.items())),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("schema", help="path of an official schema.json")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--write", metavar="PROFILE", help="replace the wire member of this profile")
    action.add_argument("--check", metavar="PROFILE", help="compare the wire member of this profile")
    arguments = parser.parse_args()

    wire = derive(arguments.schema)
    if arguments.write:
        path = Path(arguments.write)
        profile = json.loads(path.read_text(encoding="utf-8"))
        profile.pop("wire", None)
        profile["wire"] = wire
        path.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    elif arguments.check:
        profile = json.loads(Path(arguments.check).read_text(encoding="utf-8"))
        if profile.get("wire") != wire:
            print(f"{arguments.check}: the wire member does not match {arguments.schema}", file=sys.stderr)
            return 1
    else:
        json.dump(wire, sys.stdout, indent=2, ensure_ascii=False)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
