"""Query discovery, span accounting and CSV shape for the pipeline profiler."""

import importlib.util
import json
import sys
from pathlib import Path

import pytest


BENCHMARK_DIR = Path(__file__).resolve().parents[3] / "benchmark" / "decide"
sys.path.insert(0, str(BENCHMARK_DIR))
SPEC = importlib.util.spec_from_file_location("pipeline_profile", BENCHMARK_DIR / "profile_pipeline.py")
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)


def test_discovers_every_benchmark_query_in_order():
    found = PROFILE.discover()
    assert list(found) == [f"Q{n}" for n in range(1, 12)] + [f"P{n}" for n in range(1, 5)]
    assert all(path.exists() for path in found.values())


def test_every_query_resolves_its_placeholders():
    for name, path in PROFILE.discover().items():
        sql, coefficients = PROFILE.bench.resolve_query_sql(path.read_text(), PROFILE.DATABASE)
        assert "${" not in sql, f"{name} left an unresolved placeholder"
    assert PROFILE.bench.COEFFICIENTS["medium"]["P3_BUDGET"] == 5000000
    assert PROFILE.bench.COEFFICIENTS["large"]["P3_BUDGET"] == 10000000


def test_profile_sums_repeated_spans_and_reads_counters():
    events = [
        {"event": "begin", "id": 1, "parent": 0, "name": "root"},
        {"event": "end", "id": 1, "parent": 0, "name": "root", "duration_ms": 8},
        {"event": "end", "id": 2, "parent": 1, "name": "child", "duration_ms": 3},
        {"event": "end", "id": 3, "parent": 1, "name": "child", "duration_ms": 2},
        {"event": "counter", "parent": 1, "name": "model.variables", "value": 500000},
        {"event": "counter", "parent": 1, "name": "execution.window.sort_buffer_bytes", "value": 1024},
        {"event": "counter", "parent": 1, "name": "execution.window.sort_buffer_bytes", "value": 2048},
    ]
    text = "\n".join("DECIDB_PROFILE: " + json.dumps(event) for event in events)
    result = PROFILE.parse_profile(text)
    assert result["phases"]["child"] == 5
    assert result["phases"]["root"] == 8
    assert result["counters"]["model.variables"] == 500000
    assert result["counters"]["execution.window.sort_buffer_bytes"] == 2048
    assert result["counter_totals"]["execution.window.sort_buffer_bytes"] == 3072


def test_profile_folds_in_aggregate_totals():
    # sink_append and output_readback don't log every chunk; they report one pre-summed
    # "aggregate" event per statement instead of a "begin"/"end" pair. It must land in the
    # same phases dict as everything else, or that stage's time silently reads as zero.
    events = [
        {"event": "aggregate", "name": "execution.sink_append", "duration_ms": 23.198133, "calls": 65},
        {"event": "aggregate", "name": "execution.output_readback", "duration_ms": 8.04091, "calls": 246},
        {"event": "end", "id": 1, "parent": 0, "name": "execution.assemble_input", "duration_ms": 1.5},
    ]
    text = "\n".join("DECIDB_PROFILE: " + json.dumps(event) for event in events)
    result = PROFILE.parse_profile(text)
    assert result["phases"]["execution.sink_append"] == pytest.approx(23.198133)
    assert result["phases"]["execution.output_readback"] == pytest.approx(8.04091)
    assert result["phases"]["execution.assemble_input"] == 1.5


def test_profile_ignores_events_outside_the_measured_query():
    events = [{"event": "end", "id": 1, "parent": 0, "name": "setup", "duration_ms": 99}]
    noise = "\n".join("DECIDB_PROFILE: " + json.dumps(event) for event in events)
    body = 'DECIDB_PROFILE: {"event":"end","id":2,"parent":0,"name":"real","duration_ms":1}'
    text = noise + "\nDECIDB_PROFILE_QUERY_BEGIN\n" + body + "\nDECIDB_PROFILE_QUERY_END\n"
    assert PROFILE.parse_profile(text)["phases"] == {"real": 1}


def test_stages_never_double_count_and_csv_covers_them():
    spans = [span for names in PROFILE.STAGES.values() for span in names]
    assert len(spans) == len(set(spans)), "a span is claimed by two stages"
    for stage in PROFILE.STAGES:
        assert f"{stage}_ms" in PROFILE.COLUMNS
    for counter in PROFILE.COUNTERS:
        assert counter in PROFILE.COLUMNS
    assert len(PROFILE.COLUMNS) == len(set(PROFILE.COLUMNS)), "a column name is written twice into the CSV header"
    assert "solve" in PROFILE.STAGES, "solve_ms must come from STAGES, not a second hardcoded column"
