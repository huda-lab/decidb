//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/solver/decide_solve.cpp
//
// The solve of a DECIDE query as ordinary execution behaviour: the options, the solve, and the stop on a time limit or
// a Ctrl-C (the checkpoint report, the continuation prompt, the incumbent). What the result then routes to is the
// operator's switch (PhysicalDecide::FinalizeSolveResult).
//
//===----------------------------------------------------------------------===//

#include "duckdb/decidb/solver/decide_solve.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/decidb/solver/solver_config.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/decide_number_warning.hpp"

#include <cmath>
// The slow-solve report needs peak process memory. It is a platform primitive; MSVC has no <sys/resource.h>. Same
// split as gurobi_loader.cpp.
#ifndef _WIN32
#include <sys/resource.h> // getrusage — peak process memory
#endif
#include <iostream> // std::cin / std::getline — read the continuation decision at a time-limit stop

namespace duckdb {

//===--------------------------------------------------------------------===//
// Slow-solve checkpoint report (S2)
//===--------------------------------------------------------------------===//
// On a time-limit termination we tell the SQL user what the solver already found
// and how far it can still improve, in plain language (no "incumbent"/"gap"/"bound"
// solver words per the user-output rule). S2 prints the informational block only;
// the interactive "keep going?" prompt and warm re-solve are S3.

//! Peak resident set size of the whole process, in bytes. Whole-process, not
//! solve-only — it answers "will continuing risk running out of RAM." ru_maxrss is
//! bytes on macOS but KiB on Linux, so normalize.
//! Returns 0 when the figure is unavailable; callers drop the memory line rather
//! than reporting a zero that reads as a measurement.
static double PeakProcessMemoryBytes() {
#ifdef _WIN32
	// Windows has no getrusage. The equivalent (GetProcessMemoryInfo) would add a
	// psapi link entry to both CMake and setup.py, so the report simply omits its
	// memory line here instead.
	return 0.0;
#else
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) != 0) {
		return 0.0;
	}
#ifdef __APPLE__
	return (double)usage.ru_maxrss;
#else
	return (double)usage.ru_maxrss * 1024.0;
#endif
#endif
}

//! Compact byte count, e.g. "1.2 GB" / "340 MB". The figure is a process's peak resident set, never under a megabyte.
static string FormatMemory(double bytes) {
	const char *unit = "MB";
	double value = bytes / (1024.0 * 1024.0);
	if (value >= 1024.0) {
		value /= 1024.0;
		unit = "GB";
	}
	return StringUtil::Format(value >= 100.0 ? "%.0f %s" : "%.1f %s", value, unit);
}

//! Seconds, integer when whole (300s), one decimal for sub-limit probe values (1.5s).
//! Sub-second values keep their significant digits (0.05s) — one decimal would round a
//! small time limit up to a misleading 0.1s (or down to 0.0s).
static string FormatDuration(double seconds) {
	if (std::fabs(seconds) < 1.0) {
		return StringUtil::Format("%.2gs", seconds);
	}
	if (std::fabs(seconds - std::round(seconds)) < 0.05) {
		return StringUtil::Format("%.0fs", std::round(seconds));
	}
	return StringUtil::Format("%.1fs", seconds);
}

string FormatDecideGapPercent(double gap) {
	// Fixed decimals round a gap of 2e-6 to "0.00%", which beside "not proven best" reads as a contradiction. Two
	// significant digits keep it (0.0002%); from 1% up, the whole number plus one decimal, so %g never switches a gap
	// of 100% or more to an exponent.
	double percent = gap * 100.0;
	int digits = 2 + (percent >= 1.0 ? static_cast<int>(std::floor(std::log10(percent))) : 0);
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.*g", digits, percent);
	return buffer;
}

//! Print the path-1 (solution found) / path-2 (no solution) status block to stderr.
//! Info only — the "keep improving it?" prompt lands with the S3 continuation loop. The model minimizes, so a query
//! written MAXIMIZE (`negate_objective`) shows its best objective so far as the negated model value, as it wrote it.
static void PrintDecideTimeoutReport(const SolverResult &result, double elapsed_seconds, double time_limit,
                                     bool negate_objective) {
	string limit_str = FormatDuration(time_limit);
	// 0 means "not measurable here" (Windows, or a getrusage failure), not "no memory
	// used" — say nothing rather than print "peak memory 0 B".
	double peak_bytes = PeakProcessMemoryBytes();
	string tail = peak_bytes > 0.0
	                  ? StringUtil::Format("  elapsed %s · peak memory %s\n", FormatDuration(elapsed_seconds).c_str(),
	                                       FormatMemory(peak_bytes).c_str())
	                  : StringUtil::Format("  elapsed %s\n", FormatDuration(elapsed_seconds).c_str());
	// A user Ctrl-C and a wall-clock timeout share the same best-so-far readback, but
	// must not read the same to the user: "you stopped it" vs "the clock ran out".
	if (result.has_solution) {
		const double shown = negate_objective ? 0.0 - result.objective_value : result.objective_value;
		// gap is a fraction on both backends; render as a percentage. It is NaN when
		// no proven bound exists (LP/QP timeouts, failed reads) — claim nothing then.
		string closeness = std::isfinite(result.gap)
		                       ? StringUtil::Format("  best objective so far: %g  (within %s%% of the best possible)\n",
		                                            shown, FormatDecideGapPercent(result.gap))
		                       : StringUtil::Format("  best objective so far: %g\n", shown);
		if (result.user_interrupted) {
			fprintf(stderr, "DECIDE stopped at your request with a usable solution (not proven best).\n%s%s",
			        closeness.c_str(), tail.c_str());
		} else {
			fprintf(stderr, "DECIDE hit the %s time limit with a usable solution (not proven best).\n%s%s",
			        limit_str.c_str(), closeness.c_str(), tail.c_str());
		}
	} else {
		// A stop at the user's request with no solution never gets here: it raises first (FinalizeSolveResult).
		fprintf(stderr, "DECIDE hit the %s time limit without finding a solution yet.\n%s", limit_str.c_str(),
		        tail.c_str());
	}
}

//! The continuation prompt of a time-limit stop on a terminal: the one place that prints to and reads from the user.
//! Each boundary prints the checkpoint report and asks whether to keep solving; "keep" resumes the warm solver for
//! another chunk. Returns on a stop decision, a proven optimum, or end of input; raises when a resume ends with no
//! solution and no way to go on.
static void PromptToContinue(SolverResult &solve_result, SolverSession &session, double chunk, double &cum_elapsed,
                             double &cum_budget, bool negate_objective) {
	for (;;) {
		PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget, negate_objective);

		// stdin is a terminal (can_prompt guaranteed this), so it is safe to block
		// reading the user's decision. The CLI's line editor is inactive
		// mid-execution, so a plain getline does not fight it.
		if (solve_result.has_solution) {
			fprintf(stderr, "Keep improving it?  [Enter] continue +%s  ·  s + Enter to stop and take this solution: ",
			        FormatDuration(chunk).c_str());
		} else {
			fprintf(stderr,
			        "Keep searching?  [Enter] continue +%s  ·  s + Enter to give up: ", FormatDuration(chunk).c_str());
		}
		fflush(stderr);
		bool go;
		string line;
		if (!std::getline(std::cin, line)) {
			go = false; // EOF (Ctrl-D / closed input) → stop
		} else {
			StringUtil::Trim(line);
			go = StringUtil::Lower(line) != "s";
		}
		if (!go) {
			break;
		}

		// Resume the warm solver for another chunk (warm start is automatic — the
		// solver never left scope).
		Profiler chunk_timer;
		chunk_timer.Start();
		solve_result = session.Continue(chunk);
		chunk_timer.End();
		cum_elapsed += chunk_timer.Elapsed();
		cum_budget += chunk;

		if (solve_result.status == SolverStatus::OPTIMAL) {
			break; // proven optimum — fall through to the shared success stores
		}
		if (solve_result.status != SolverStatus::TIME_LIMIT) {
			// A resume can only reach a definitive INFEASIBLE/UNBOUNDED when no
			// incumbent ever existed (an incumbent proves feasibility); surface it
			// as the plain solver error.
			ThrowDecideSolveError(solve_result);
		}
		// else: another time-limit stop — loop and re-report with fresh numbers.
	}
}

DecideSolveOutcome SolveDecide(ClientContext &context, SolverInput &input, const VarIndexer &indexer,
                               SolverBackend backend, bool negate_objective, bool diagnosis_armed, bool can_prompt) {
	DecideSolveOutcome outcome;
	SolverResult &solve_result = outcome.result;
	// H3: the statement's DIAGNOSE prefix, and nothing else, arms diagnosis. Under it,
	// pre-extract the unbounded ray so it is ready if the solve turns out unbounded, and
	// retain the built model so the INFEASIBLE terminal can hand it to the elastic
	// engine. An unprefixed query pays for neither, and both failure terminals are
	// prefix-only anyway (RouteSolveResult).
	// One warning latch for the whole query: every DECIDE node, direct plans included, and under DIAGNOSE the repair
	// model share it.
	auto &number_warning = DecideNumberWarningState::Of(context);
	SolveModelOptions solve_options;
	solve_options.extract_unbounded_ray = diagnosis_armed;
	solve_options.say_number_warning = [&number_warning]() {
		number_warning.Say();
	};
	// H5: slow-solve continuation is ordinary execution behaviour, not diagnosis, so it
	// runs with or without the prefix. `can_prompt` is the operator's tty test: the prompt falls back to a plain error
	// when no one can answer (tests, benchmarks, -c pipes, C-API).
	// The warm solver is retained only where a continuation can actually be asked for.
	bool want_session = can_prompt;
	// The first solve chunk uses the same per-chunk budget every Continue() chunk will,
	// so elapsed climbs in even multiples (300s, 600s, ...) across the loop.
	solve_options.time_limit_seconds = ResolveDecideTimeLimit();
	solve_options.mip_gap = GetDecideMipGap(context);
	// Ctrl-C cuts the solve short and returns the best-so-far instead of aborting the
	// query — also ordinary execution behaviour, so the poll is installed unconditionally.
	// Both backends honor it mid-solve.
	solve_options.interrupt_poll = [&context]() {
		return context.interrupted.load();
	};
	outcome.diagnostic_options.time_limit_seconds = ResolveDecideDiagnosticTimeLimit(solve_options.time_limit_seconds);
	outcome.diagnostic_options.interrupt_poll = solve_options.interrupt_poll;

	// Reconcile the `<>` label channel with the final global-var count. Only the
	// aggregate-`<>` site labels its global z (for the infeasible removal dial),
	// and those z's are allocated first (the deferred-NE loop runs before all
	// MIN/MAX linking), so the labels form a contiguous prefix; pad the trailing
	// unlabeled MIN/MAX globals with "" to keep the vector parallel with
	// global_variable_types. (Invariant: never shrinks — only labeled globals
	// precede the resize point.) An ordinary query keeps no labels.
	if (diagnosis_armed) {
		D_ASSERT(input.global_variable_labels.size() <= input.num_global_vars);
		input.global_variable_labels.resize(input.num_global_vars);
	}

	// Retained only when a continue-capable timeout mode is active: the live warm
	// solver, so the TIME_LIMIT terminal can Continue() it for more wall-clock.
	unique_ptr<SolverSession> retained_session;
	// Always-on wall-clock around the solve (independent of the profile channel): the slow
	// checkpoint report (S2) needs the elapsed solve time to show the user.
	Profiler solve_wall_timer;
	solve_wall_timer.Start();
	// Retained only when diagnosis is armed; the move is trivial and the model is
	// freed when Finalize returns. SolveModel otherwise discards the built model.
	solve_result = SolveModel(input, indexer, backend, solve_options, diagnosis_armed ? &outcome.model : nullptr,
	                          want_session ? &retained_session : nullptr);
	solve_wall_timer.End();

	// H5: a time-limit stop (or a Ctrl-C, which arrives as one) is ordinary execution
	// behaviour and is handled here, before the router sees the result — with or without
	// the prefix. It either delivers an incumbent, resumes to a proven optimum, or
	// raises; it never diagnoses.
	if (solve_result.status == SolverStatus::TIME_LIMIT) {
		// Before anything is printed: did another operator in this same query already
		// fail? A SELECT can hold several DECIDE clauses — two subqueries joined side
		// by side, say — and when one of them throws, DuckDB stops the rest by setting
		// the very query interrupt Ctrl-C sets (`Executor::PushError`). Every checkpoint
		// test below reads that interrupt, so this solve would be reported as a
		// cancellation the user never asked for, printed *above* the error that actually
		// stopped the query, with elapsed/memory figures describing a solve that was
		// never the problem. Say nothing and re-raise the real failure instead.
		auto &executor = Executor::Get(context);
		if (executor.HasError()) {
			executor.ThrowException();
		}
		// S2/S3/S4: at each chunk boundary print the checkpoint report (what was found
		// + how far it can still improve + elapsed/memory), then offer the choice: keep
		// solving, or take the incumbent with its gap to best. "Keep solving" re-runs
		// the SAME warm solver (retained_session) for another fresh chunk — the MIP
		// search resumes, elapsed accumulates. On stop, a usable incumbent is returned
		// as a SUCCESSFUL result (with a plain stderr caveat that it is not proven
		// best); no incumbent falls to the plain timeout error.
		double chunk = ResolveDecideTimeLimit();
		double cum_elapsed = solve_wall_timer.Elapsed();
		double cum_budget = chunk;

		// Did the user stop this, or the clock? `user_interrupted` is the backend's own
		// attribution (both backends cut the solve short mid-search and say so); a Ctrl-C
		// that lands after the solver's last check arrives indistinguishable from a
		// timeout. The query interrupt is set either way, so read it too. The answer is
		// written back into the result once, so the report and the error below read the
		// same decision.
		bool stopped_by_user = solve_result.user_interrupted || context.interrupted.load();
		solve_result.user_interrupted = stopped_by_user;
		if (stopped_by_user) {
			// Ctrl-C is not a question. The user has already said stop, so there is
			// nothing to offer: hand back the best-so-far, with the caveat that it is not
			// proven best. This is the one stop that does not need a terminal — the
			// decision was made at the keyboard, not in an answer to a prompt. With
			// nothing found there is nothing to hand back: the query was cancelled, so
			// it fails as DuckDB's own interrupt does, with one message.
			if (!solve_result.has_solution) {
				throw Exception(ExceptionType::INTERRUPT, "DECIDE stopped at your request before finding a solution.");
			}
			PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget, negate_objective);
			fprintf(stderr,
			        "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n");
			context.interrupted = false;
			outcome.deliver_incumbent = true;
		} else if (!can_prompt) {
			// The clock ran out and nobody is at the keyboard (tests, benchmarks, `-c`
			// pipes, the C API), so there is no one to answer the offer. Print the
			// checkpoint once and raise: never prompt where no one can answer.
			PrintDecideTimeoutReport(solve_result, cum_elapsed, cum_budget, negate_objective);
			ThrowDecideSolveError(solve_result);
		} else {
			// The interrupt poll installed on the first solve persists on the retained
			// session into every Continue() chunk, but the prompt below already stops the
			// user at each boundary, and a watcher thread contending with the interactive
			// getline destabilizes the prompt (found empirically) — so reset it to
			// boundary-only here. The entry interrupt already fired *before* this loop, so a
			// first-solve Ctrl-C still routed us in.
			if (retained_session) {
				retained_session->SetInterruptPoll({});
			}

			// Report at each boundary, then continue or stop.
			PromptToContinue(solve_result, *retained_session, chunk, cum_elapsed, cum_budget, negate_objective);

			// Loop exited on a stop decision or an OPTIMAL break.
			if (solve_result.status == SolverStatus::OPTIMAL || solve_result.has_solution) {
				if (solve_result.status != SolverStatus::OPTIMAL) {
					// Stopped early with a usable-but-unproven incumbent: succeed and
					// return it, but say plainly it is not proven best. There is no live
					// SQL NOTICE channel, so this rides on stderr with the checkpoint
					// report, which already printed the gap to best.
					fprintf(
					    stderr,
					    "DECIDE is returning the best solution found so far — it is NOT proven the best possible.\n");
					outcome.deliver_incumbent = true;
				}
				// If a Ctrl-C broke the loop, the interrupt is now handled (we stopped
				// solving and have a result to return); clear it so the rows flow to the
				// client instead of the executor aborting the query. Covers both the
				// incumbent stop and the rare "Ctrl-C landed as the optimum was proven".
				context.interrupted = false;
			} else {
				// Stopped with nothing found yet: the plain timeout error, same as the
				// non-interactive exit above.
				ThrowDecideSolveError(solve_result);
			}
		}
	}

	return outcome;
}

} // namespace duckdb
