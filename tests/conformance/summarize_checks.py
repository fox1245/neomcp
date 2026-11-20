"""Read what conformance runs wrote, for the CI jobs `conformance` and `conformance-client`.

Usage:
  summarize_checks.py RESULTS_DIR...
      Print the checks of runs that did not succeed, one line each. Each RESULTS_DIR is a folder the runner wrote with
      --output-dir: one subfolder per scenario, each with a checks.json (an array of checks with `id`, `status` and,
      when it did not pass, `errorMessage`). Checks that succeeded, were skipped or only inform are left out. A folder
      that does not exist is reported and skipped.

  summarize_checks.py --check-listing EXPECTATIONS REVISION=LISTING...
      Compare the client scenarios each LISTING (the output of the runner's `list --requirements REVISION`) names with
      the `required` lists of EXPECTATIONS (client-scenarios.json), and fail on any difference.

  summarize_checks.py --list-runs EXPECTATIONS
      Print the single runs of the client gate, one per line: profile mode, revision, scenario.

  summarize_checks.py --client EXPECTATIONS ROOT
      Judge the single runs of the client gate. ROOT/<mode>/<revision>/<scenario>/ holds the runner's exit status in
      `exit-code` and the one result folder the runner wrote there. A run passes when the runner exited 0, its
      checks.json exists and is not empty, no check is a FAILURE or a WARNING, every check id the expectations name is
      there with a SUCCESS, every row they name is a SUCCESS (not SKIPPED), and the fixture's last line on standard
      output says it completed. A run listed as expected to fail must fail, and every other run must pass.
"""

import json
import os
import sys

QUIET = {"SUCCESS", "SKIPPED", "INFO"}
FAILING = {"FAILURE", "WARNING"}
MODES = ("run", "all")
FIXTURE = "neomcp-conformance-client"
CLIENT_SECTION = "Client scenarios (test against a client):"
ITEM_PREFIX = "- "


def summarize(root):
    if not os.path.isdir(root):
        print(f"{root}: no results")
        return
    print(f"== {root}")
    for scenario in sorted(os.listdir(root)):
        path = os.path.join(root, scenario, "checks.json")
        if not os.path.isfile(path):
            continue
        try:
            with open(path, encoding="utf-8") as file:
                checks = json.load(file)
        except (OSError, ValueError) as error:
            print(f"{scenario}: unreadable checks.json ({error})")
            continue
        for check in checks if isinstance(checks, list) else []:
            if not isinstance(check, dict) or check.get("status") in QUIET:
                continue
            message = str(check.get("errorMessage", "")).replace("\n", " ")
            print(f"{scenario}: {check.get('id')} {check.get('status')}: {message[:400]}")


def load(path):
    with open(path, encoding="utf-8") as file:
        return json.load(file)


def listed_client_scenarios(text):
    """The names under the client section of a requirements listing; None when the listing has no such section."""
    names = None
    for line in text.splitlines():
        stripped = line.strip()
        if names is None:
            if stripped == CLIENT_SECTION:
                names = []
            continue
        if not stripped:
            break
        if stripped.startswith(ITEM_PREFIX):
            names.append(stripped[len(ITEM_PREFIX):].strip())
        else:
            break
    return names


def check_listing(expectations_path, pairs):
    expectations = load(expectations_path)
    required = expectations.get("required", {})
    status = 0
    for pair in pairs:
        revision, _, listing_path = pair.partition("=")
        with open(listing_path, encoding="utf-8") as file:
            text = file.read()
        names = listed_client_scenarios(text)
        if names is None:
            print(f"{revision}: the listing has no section '{CLIENT_SECTION}'; it reads:")
            print(text)
            status = 1
            continue
        want = set(required.get(revision, []))
        have = set(names)
        if want == have:
            print(f"{revision}: the runner lists the {len(have)} client scenarios the expectations name")
            continue
        status = 1
        for name in sorted(have - want):
            print(f"{revision}: the runner lists {name}, which the expectations do not name")
        for name in sorted(want - have):
            print(f"{revision}: the expectations name {name}, which the runner does not list")
    for revision, scenarios in expectations.get("run", {}).items():
        for name in scenarios:
            if name not in required.get(revision, []):
                print(f"{revision}: {name} is run but not required")
                status = 1
    return status


def list_runs(expectations_path):
    expectations = load(expectations_path)
    for mode in MODES:
        for revision, scenarios in sorted(expectations.get("run", {}).items()):
            for scenario in scenarios:
                print(f"{mode} {revision} {scenario}")
    return 0


def result_checks(folder):
    """The checks of the one result folder in `folder`, or why there are none."""
    found = []
    for entry in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        path = os.path.join(folder, entry, "checks.json")
        if os.path.isfile(path):
            found.append(os.path.join(folder, entry))
    if len(found) != 1:
        return None, None, f"{len(found)} result folders with a checks.json"
    try:
        checks = load(os.path.join(found[0], "checks.json"))
    except (OSError, ValueError) as error:
        return None, None, f"unreadable checks.json ({error})"
    if not isinstance(checks, list) or not checks:
        return None, None, "checks.json holds no check"
    return checks, found[0], None


def fixture_status(result_folder):
    """The fixture's own last line on standard output, read as JSON; None when there is none."""
    path = os.path.join(result_folder, "stdout.txt")
    try:
        with open(path, encoding="utf-8") as file:
            lines = [line for line in file.read().splitlines() if line.strip()]
    except OSError:
        return None
    if not lines:
        return None
    try:
        status = json.loads(lines[-1])
    except ValueError:
        return None
    return status if isinstance(status, dict) and status.get("fixture") == FIXTURE else None


def judge(expectations, folder, scenario):
    """The problems of one single run; empty when it passed."""
    problems = []
    try:
        with open(os.path.join(folder, "exit-code"), encoding="utf-8") as file:
            code = int(file.read().strip())
    except (OSError, ValueError):
        return ["no exit status of the runner"]
    if code != 0:
        problems.append(f"the runner exited {code}")
    checks, result_folder, why = result_checks(folder)
    if checks is None:
        problems.append(why)
        return problems
    by_id = {}
    for check in checks:
        if not isinstance(check, dict):
            continue
        by_id.setdefault(check.get("id"), []).append(check)
        if check.get("status") in FAILING:
            message = str(check.get("errorMessage", "")).replace("\n", " ")[:300]
            problems.append(f"{check.get('id')} {check.get('name', '')} {check.get('status')}: {message}")
    for wanted in expectations.get("checks", {}).get(scenario, []):
        rows = by_id.get(wanted, [])
        if not any(row.get("status") == "SUCCESS" for row in rows):
            problems.append(f"{wanted}: no SUCCESS")
    names = {check.get("name"): check.get("status") for check in checks if isinstance(check, dict)}
    for row in expectations.get("rows", {}).get(scenario, []):
        if names.get(row) != "SUCCESS":
            problems.append(f"{row}: {names.get(row, 'missing')}")
    status = fixture_status(result_folder)
    if status is None:
        problems.append("the fixture wrote no final status line")
    elif status.get("status") != "ok":
        problems.append(f"the fixture failed: {status.get('reason', '')}")
    return problems


def client_gate(expectations_path, root):
    expectations = load(expectations_path)
    expected = expectations.get("expected_to_fail", {})
    status = 0
    for mode in MODES:
        for revision, scenarios in sorted(expectations.get("run", {}).items()):
            for scenario in scenarios:
                key = f"{scenario}@{revision}"
                problems = judge(expectations, os.path.join(root, mode, revision, scenario), scenario)
                listed = key in expected.get(mode, [])
                if not problems and not listed:
                    print(f"pass  {mode} {key}")
                elif problems and listed:
                    print(f"fails as expected  {mode} {key}: {problems[0]}")
                elif not problems:
                    print(f"PASSES BUT IS LISTED AS EXPECTED TO FAIL  {mode} {key}")
                    status = 1
                else:
                    print(f"FAIL  {mode} {key}")
                    for problem in problems:
                        print(f"      {problem}")
                    status = 1
    return status


def main(argv):
    if len(argv) >= 2 and argv[1] == "--check-listing":
        return check_listing(argv[2], argv[3:])
    if len(argv) == 3 and argv[1] == "--list-runs":
        return list_runs(argv[2])
    if len(argv) == 4 and argv[1] == "--client":
        return client_gate(argv[2], argv[3])
    for root in argv[1:]:
        summarize(root)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
