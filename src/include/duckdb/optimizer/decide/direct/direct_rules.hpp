#pragma once

#include "duckdb/optimizer/decide/direct/direct_rule.hpp"

namespace duckdb {

//! The factory of every direct-solve rule. A new rule adds its declaration here and one line in direct_registry.cpp.

//! The first direct-solve rule: one row-scoped Boolean under uniform-matroid (cardinality) bounds (S1).
unique_ptr<DirectSolveRule> MakeS1UniformMatroidRule();

//! The second direct-solve rule: separable box-constrained decisions: independent row-scoped decisions, each under
//! bounds of its own, with an objective that is a sum of one linear or quadratic term per row (A1).
unique_ptr<DirectSolveRule> MakeA1SeparableBoxRule();

//! The third direct-solve rule: one row-scoped REAL decision under per-row bounds and one `SUM(w*x) <= B` budget, with
//! a linear objective (R1, the continuous knapsack).
unique_ptr<DirectSolveRule> MakeR1ContinuousKnapsackRule();

//! The fourth direct-solve rule: one row-scoped REAL decision in a box [0, inf) or (-inf, inf) under one
//! `SUM(POWER(x, 2)) <= B`, with a linear objective or one square (N1, the Euclidean ball).
unique_ptr<DirectSolveRule> MakeN1EuclideanBallRule();

//! The fifth direct-solve rule: one query-wide REAL decision in a constant interval under one square of the decision
//! minus a source value, whose answer is the mean of the values clamped into the interval (A2, single-facility
//! location).
unique_ptr<DirectSolveRule> MakeA2SingleFacilityLocationRule();

//! The sixth direct-solve rule: a one-to-one matching of the classes of the two sides of a cross product, under a
//! distance or product cost of one position of each class, answered by pairing equal (or opposite) ranks (O1, the
//! Monge assignment).
unique_ptr<DirectSolveRule> MakeO1MongeAssignmentRule();

//! The seventh direct-solve rule: one row-scoped Boolean under one budget `SUM(w*x) <= B`, maximizing how many rows it
//! selects, answered by the longest prefix of the rows sorted by weight that fits the budget (S3, the
//! maximum-cardinality knapsack).
unique_ptr<DirectSolveRule> MakeS3MaxCardinalityKnapsackRule();

//! The eighth direct-solve rule: one row-scoped Boolean under upper count quotas whose groups nest (departments inside
//! divisions inside one global cap), maximizing a linear score, answered by one ranking per level from the finest to
//! the coarsest (S2, the laminar matroid).
unique_ptr<DirectSolveRule> MakeS2LaminarMatroidRule();

} // namespace duckdb
