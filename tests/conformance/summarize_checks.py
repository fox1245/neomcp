"""Print the checks of conformance runs that did not succeed, one line each.

Usage: summarize_checks.py RESULTS_DIR...

Each RESULTS_DIR is a folder the runner wrote with --output-dir: one subfolder per scenario, each with a
checks.json (an array of checks with `id`, `status` and, when it did not pass, `errorMessage`). Checks that succeeded,
were skipped or only inform are left out. A folder that does not exist is reported and skipped.
"""

import json
import os
import sys

QUIET = {"SUCCESS", "SKIPPED", "INFO"}


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


def main(argv):
    for root in argv[1:]:
        summarize(root)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
