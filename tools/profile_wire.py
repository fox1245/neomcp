#!/usr/bin/env python3
"""Derive the `wire` member of a revision profile from an official MCP schema.json.

The `wire` member holds what the published schema states about a revision: its
requests and notifications with their senders, the requests a server may ask of
a client inside a result, the properties of every object type (with the values of
its `type` property where the schema lists them, and the type of each property that
is an object), and the unions of types. An object written inline as a property is a
type of its own, named after the type that writes it and the property. A profile's
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
# The property that says which of the types of a union an object is: its values are recorded per type.
DISCRIMINATOR = "type"
# An object written inline as a property of a type has no name in the schema. It is recorded as a type of its own,
# named after the type that writes it and the property, joined by this separator ("Resource.annotations"). A name in
# the schema never holds it, so the derived name cannot be one the schema defines.
INLINE_SEPARATOR = "."


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


def object_shape(definitions, node, seen=()):
    """The properties and required members of an object type, or None when the
    node is not one. An object type is written with `properties`, as a
    reference to an object type, or as an allOf of object types."""
    if isinstance(node.get("properties"), dict):
        return set(node["properties"]), set(node.get("required", []))
    target = ref_name(node)
    if target:
        if target in seen:
            raise SystemExit(f"{target} refers to itself")
        return object_shape(definitions, definitions[target], (*seen, target))
    parts = node.get("allOf")
    if parts is None:
        return None
    properties, required = set(), set()
    for part in parts:
        shape = object_shape(definitions, part, seen)
        if shape is None:
            return None
        properties |= shape[0]
        required |= shape[1]
    return properties, required


def property_schema(definitions, node, name, seen=()):
    """The schema of the property `name` of an object type, or None. The type is
    written with `properties`, as a reference or as an allOf of object types."""
    properties = node.get("properties")
    if isinstance(properties, dict):
        return properties.get(name)
    target = ref_name(node)
    if target:
        if target in seen:
            raise SystemExit(f"{target} refers to itself")
        return property_schema(definitions, definitions[target], name, (*seen, target))
    for part in node.get("allOf", []):
        found = property_schema(definitions, part, name, seen)
        if found is not None:
            return found
    return None


def member_schemas(definitions, node, owner, seen=()):
    """The properties of an object type with their schemas, as {name: (schema, writer)}: `writer` is the name of the
    type whose definition writes the property, which is where an object written inline gets its derived name.
    The type is written with `properties`, as a reference or as an allOf of object types."""
    properties = node.get("properties")
    if isinstance(properties, dict):
        return {name: (schema, owner) for name, schema in properties.items()}
    target = ref_name(node)
    if target:
        if target in seen:
            raise SystemExit(f"{target} refers to itself")
        return member_schemas(definitions, definitions[target], target, (*seen, target))
    found = {}
    for part in node.get("allOf", []):
        found.update(member_schemas(definitions, part, owner, seen))
    return found


def fixed_strings(node):
    """The strings a property schema allows when it lists them (a const or an
    enum of strings), sorted; None for any other schema."""
    if node is None:
        return None
    if isinstance(node.get("const"), str):
        return [node["const"]]
    values = node.get("enum")
    if isinstance(values, list) and values and all(isinstance(value, str) for value in values):
        return sorted(set(values))
    return None


def union_of_types(node):
    """The member type names of a union whose alternatives are all references,
    or None for any other node."""
    alternatives = node.get("anyOf") or node.get("oneOf")
    if alternatives is None:
        return None
    names = [ref_name(alternative) for alternative in alternatives]
    return None if None in names else names


def required_whatever_the_alternative(definitions, name, seen=()):
    """The members every alternative of a union of object types requires."""
    if name in seen:
        raise SystemExit(f"{name} contains itself")
    members = union_of_types(definitions[name])
    common = None
    for member in members:
        shape = object_shape(definitions, definitions[member])
        if shape is not None:
            required = shape[1]
        elif union_of_types(definitions[member]) is not None:
            required = required_whatever_the_alternative(definitions, member, (*seen, name))
        else:
            raise SystemExit(f"{name} has the alternative {member}, which is not an object type")
        common = required if common is None else common & required
    return common or set()


def params_of(definitions, name):
    definition = definitions[name]
    params = definition.get("properties", {}).get("params")
    entry = {"params": None, "params_required": []}
    if params is None:
        return entry
    target = ref_name(params)
    entry["params"] = target
    shape = object_shape(definitions, params)
    if shape is not None:
        entry["params_required"] = sorted(shape[1])
    elif target and union_of_types(definitions[target]) is not None:
        entry["params_required"] = sorted(required_whatever_the_alternative(definitions, target))
    else:
        raise SystemExit(f"the parameters of {name} are neither an object type nor a union of object types")
    return entry


def add_object_type(definitions, types, name, node, writer):
    """Record the object type `name` of `node`, and the object types its properties are. A property whose schema is
    a reference to an object type is of that type; a property written inline as an object with `properties` is of a
    type of its own, named after `writer` (the type whose definition writes it) and the property, and recorded in
    the same way. `property_types` lists those properties and their types; it is left out when there are none."""
    shape = object_shape(definitions, node)
    entry = {"properties": sorted(shape[0]), "required": sorted(shape[1])}
    values = fixed_strings(property_schema(definitions, node, DISCRIMINATOR))
    if values is not None:
        entry["type_values"] = values
    types[name] = entry
    property_types = {}
    for member, (schema, written_by) in member_schemas(definitions, node, writer).items():
        target = ref_name(schema)
        if target:
            if object_shape(definitions, definitions[target]) is not None:
                property_types[member] = target
        elif isinstance(schema.get("properties"), dict):
            derived = f"{written_by}{INLINE_SEPARATOR}{member}"
            if derived in definitions:
                raise SystemExit(f"{derived} is derived for an inline object and is also defined by the schema")
            if derived not in types:
                add_object_type(definitions, types, derived, schema, derived)
            property_types[member] = derived
    if property_types:
        entry["property_types"] = dict(sorted(property_types.items()))


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

    types = {}
    unions = {}
    for name, definition in definitions.items():
        if object_shape(definitions, definition) is not None:
            add_object_type(definitions, types, name, definition, name)
            continue
        members = union_of_types(definition)
        if members is not None:
            unions[name] = sorted(members)

    return {
        "schema": {"dialect": document.get("$schema"), "sha256": hashlib.sha256(raw).hexdigest()},
        "requests": dict(sorted(tables["requests"].items())),
        "notifications": dict(sorted(tables["notifications"].items())),
        "input_requests": dict(sorted(input_requests.items())),
        "unlisted_message_types": unlisted,
        "types": dict(sorted(types.items())),
        "unions": dict(sorted(unions.items())),
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
