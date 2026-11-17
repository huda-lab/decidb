"""The E/G/O construction must route rows and retain the deck's containers."""

import re
import os
import subprocess

from ._anr_cases import F1_MAX


def construction(cli, sql, chunk=2048):
    # These are explicitly requested internal dumps, so inspect them directly instead of the user-output guard.
    setting = f"SET decide_debug = 'tree,containers,chunk={chunk}'; "
    result = subprocess.run(
        [cli.exe, "-c", "SET threads=1; SET decide_direct_solve='off'; " + setting + sql],
        env={**os.environ, **(cli.env or {})},
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stderr
    containers = result.stderr.split("DECIDB_CONTAINERS_BEGIN\n", 1)[1].split("DECIDB_CONTAINERS_END", 1)[0]
    tree = result.stderr.split("DECIDB_TREE_BEGIN\n", 1)[1].split("DECIDB_TREE_END", 1)[0]
    return tree, containers


def nodes(containers):
    """Read the dump's actual node state, without inferring it from the answer."""
    pattern = re.compile(
        r"^node (\d+) ([EGO]) rows (\d+) -> (\d+) groups=(\d+) entries=(\d+) instances=(\d+) text=([^\n]*)\n",
        re.MULTILINE,
    )
    found = list(pattern.finditer(containers))
    return [
        {
            "id": int(match[1]),
            "kind": match[2],
            "incoming": int(match[3]),
            "outgoing": int(match[4]),
            "groups": int(match[5]),
            "entries": int(match[6]),
            "instances": int(match[7]),
            "text": match[8],
            "body": containers[match.end() : found[i + 1].start() if i + 1 < len(found) else len(containers)],
        }
        for i, match in enumerate(found)
    ]


def test_deck_data_reducer_retains_six_values_in_two_mode_groups(decidb_cli):
    """Slide 91 stores Air's four values and Ground's two; it has not folded them."""
    _, containers = construction(decidb_cli, F1_MAX.sql)
    state = nodes(containers)
    mode = [n for n in state if n["kind"] == "G" and "key=(Air)" in n["body"] and "key=(Ground)" in n["body"]]
    assert len(mode) == 1, containers
    assert (mode[0]["groups"], mode[0]["entries"]) == (2, 6), containers
    operand = [n for n in state if n["kind"] == "E" and n["text"] == mode[0]["text"] + " operand"]
    assert len(operand) == 1, containers
    assert operand[0]["entries"] == 6, containers
    # The stored columns still contain the six input demands, not Air=75/Ground=15.
    data = re.findall(r"data=\(([^)]*)\)", operand[0]["body"])
    assert data == ["20", "15", "10", "5", "25", "15"], containers
