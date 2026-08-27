"""Shared driver for the `DIAGNOSE <query>` relation.

`DIAGNOSE` is the only thing that starts the diagnostics engine, and it returns its
findings directly as a flat relation:

    state | clause | suggested_change | amount | total | scope | edit_source | group | row

`run` drives it through the CLI and hands back the parsed rows. `as_eav` re-expresses
those rows in the long-form (subject, attribute, value) vocabulary the behaviour tests
were written against — those tests are about *what* the engine finds (which clause, how
far to move it, loosen vs remove), not about how the columns are laid out. The flat
shape itself is asserted directly, once, in `TestDiagnoseRelationShape`.
"""

import csv
import io


def run(cli, decide_sql, *, setup="", pragmas="", scope=None, timeout=120):
    """Run `DIAGNOSE <decide_sql>` and return the CompletedProcess.

    A single statement now: the prefix runs the query and returns the diagnosis, so
    there is no failing statement to recover from and no second read-back. `scope` sets
    the infeasible slack-scope tuning pragma (query | expanded) when given.
    """
    scope_pragma = (
        f"PRAGMA diagnose_decide_infeasible_slack_scope='{scope}';\n" if scope else ""
    )
    script = (
        ".mode csv\n"
        f"{setup}"
        f"{scope_pragma}"
        f"{pragmas}"
        f"DIAGNOSE {decide_sql};\n"
    )
    return cli.execute_script(script, timeout=timeout)


def rows(result):
    """The findings, as one dict per row of the flat relation."""
    return list(csv.DictReader(io.StringIO(result.stdout)))


def amount_text(raw):
    """The relation's `amount` is a DOUBLE, so CSV renders 5 as `5.0`. These tests read
    it as the user's own magnitude, so print it back the way the query wrote it."""
    if raw in (None, "", "NULL"):
        return ""
    value = float(raw)
    return str(int(value)) if value == int(value) else repr(value)


def as_eav(flat_rows):
    """Re-express flat findings as long-form (subject_kind, subject, attribute, value)
    rows. One finding fans back out into the attributes it carries."""
    out = []

    def text(value):
        """`.mode csv` renders a SQL NULL as the literal `NULL`; read it as absent."""
        return "" if value in (None, "", "NULL") else value

    for r in flat_rows:
        state = text(r.get("state"))

        def emit(kind, subject, attribute, value):
            out.append(
                {
                    "state": state,
                    "subject_kind": kind,
                    "subject": subject,
                    "attribute": attribute,
                    "value": value,
                }
            )

        clause = text(r.get("clause"))
        change = text(r.get("suggested_change"))
        amount = amount_text(r.get("amount"))
        source = r.get("edit_source") or ""
        group = text(r.get("group"))
        if source == "achievable_objective":
            emit("model", "NULL", "achievable_objective", amount)
        elif source == "unbounded_after_fix":
            emit("model", "NULL", "achievable_objective", "unbounded")
        elif source == "rigid_conflict":
            emit("model", "NULL", "elastic_infeasible", "true")
        elif source == "undiagnosed":
            emit("model", "NULL", "undiagnosed", change)
        elif source == "unreachable_bound":
            emit("clause", clause, "unreachable_bound", "true")
            if group:
                emit("clause", clause, "group", group)
        elif source == "remove_only":
            emit("clause", clause, "edit_kind", "drop")
        elif source.startswith("runaway_"):
            emit("variable", clause, "grows_toward", source.split("_", 1)[1])
            if amount:
                emit("variable", clause, "escaping_instances", amount)
            if group:
                emit("variable", clause, "escaping_group", group)
        elif change:
            emit("clause", clause, "edit_kind", "loosen")
            emit("clause", clause, "suggested_change", change)
            emit("clause", clause, "amount", amount)
            if group:
                emit("clause", clause, "group", group)
            if source:
                emit("clause", clause, "edit_source", source)
                emit(
                    "clause",
                    clause,
                    "offset_scope",
                    {"expanded_row": "row", "expanded_group": "group"}.get(
                        source, "clause"
                    ),
                )
    return out


def eav_rows(result):
    """`rows` then `as_eav`, the shape most behaviour tests want."""
    return as_eav(rows(result))
