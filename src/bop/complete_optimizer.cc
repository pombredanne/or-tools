// Copyright 2010-2014 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bop/complete_optimizer.h"

#include "bop/bop_util.h"
#include "sat/boolean_problem.h"
#include "sat/optimization.h"

namespace operations_research {
namespace bop {

SatCoreBasedOptimizer::SatCoreBasedOptimizer(const std::string& name)
    : BopOptimizerBase(name),
      state_update_stamp_(ProblemState::kInitialStampValue),
      initialized_(false),
      assumptions_already_added_(false) {
  // This is in term of number of variables not at their minimal value.
  lower_bound_ = sat::Coefficient(0);
  upper_bound_ = sat::kCoefficientMax;
}

SatCoreBasedOptimizer::~SatCoreBasedOptimizer() {}

BopOptimizerBase::Status SatCoreBasedOptimizer::SynchronizeIfNeeded(
    const ProblemState& problem_state) {
  if (state_update_stamp_ == problem_state.update_stamp()) {
    return BopOptimizerBase::CONTINUE;
  }
  state_update_stamp_ = problem_state.update_stamp();

  // Note that if the solver is not empty, this only load the newly learned
  // information.
  const BopOptimizerBase::Status status =
      LoadStateProblemToSatSolver(problem_state, &solver_);
  if (status != BopOptimizerBase::CONTINUE) return status;

  if (!initialized_) {
    // Initialize the algorithm.
    nodes_ = sat::CreateInitialEncodingNodes(
        problem_state.original_problem().objective(), &offset_, &repository_);
    initialized_ = true;

    // This is used by the "stratified" approach.
    stratified_lower_bound_ = sat::Coefficient(0);
    for (sat::EncodingNode* n : nodes_) {
      stratified_lower_bound_ = std::max(stratified_lower_bound_, n->weight());
    }
  }

  // Extract the new upper bound.
  if (problem_state.solution().IsFeasible()) {
    upper_bound_ = problem_state.solution().GetCost() + offset_;
  }
  return BopOptimizerBase::CONTINUE;
}

sat::SatSolver::Status SatCoreBasedOptimizer::SolveWithAssumptions() {
  solver_.Backtrack(0);
  for (sat::EncodingNode* n : nodes_) {
    lower_bound_ += n->Reduce(solver_) * n->weight();
  }
  if (upper_bound_ != sat::kCoefficientMax) {
    const sat::Coefficient gap = upper_bound_ - lower_bound_;
    if (gap <= 0) {
      // The lower bound is proved to equal the upper bound, the upper bound
      // corresponding to the current solution value from the problem_state.
      // As the optimizer is looking for a better solution (see
      // LoadStateProblemToSatSolver), that means the current model is UNSAT
      // and so the synchronized solution is optimal.
      return sat::SatSolver::MODEL_UNSAT;
    }
    for (sat::EncodingNode* n : nodes_) {
      n->ApplyUpperBound((gap / n->weight()).value(), &solver_);
    }
  }
  std::vector<sat::Literal> assumptions;
  {
    int new_node_index = 0;
    for (sat::EncodingNode* n : nodes_) {
      if (n->size() > 0) {
        if (n->weight() >= stratified_lower_bound_) {
          assumptions.push_back(n->literal(0).Negated());
        }
        nodes_[new_node_index] = n;
        ++new_node_index;
      }
    }
    nodes_.resize(new_node_index);
  }
  CHECK_LE(assumptions.size(), nodes_.size());
  return solver_.ResetAndSolveWithGivenAssumptions(assumptions);
}

// Only run this if there is an objective.
bool SatCoreBasedOptimizer::ShouldBeRun(
    const ProblemState& problem_state) const {
  return problem_state.original_problem().objective().literals_size() > 0;
}

BopOptimizerBase::Status SatCoreBasedOptimizer::Optimize(
    const BopParameters& parameters, const ProblemState& problem_state,
    LearnedInfo* learned_info, TimeLimit* time_limit) {
  SCOPED_TIME_STAT(&stats_);
  CHECK(learned_info != nullptr);
  CHECK(time_limit != nullptr);
  learned_info->Clear();

  const BopOptimizerBase::Status sync_status =
      SynchronizeIfNeeded(problem_state);
  if (sync_status != BopOptimizerBase::CONTINUE) {
    return sync_status;
  }

  int64 conflict_limit = parameters.max_number_of_conflicts_in_random_lns();
  double deterministic_time_at_last_sync = solver_.deterministic_time();
  while (!time_limit->LimitReached()) {
    sat::SatParameters sat_params = solver_.parameters();
    sat_params.set_max_time_in_seconds(time_limit->GetTimeLeft());
    sat_params.set_max_deterministic_time(
        time_limit->GetDeterministicTimeLeft());
    sat_params.set_random_seed(parameters.random_seed());
    sat_params.set_max_number_of_conflicts(conflict_limit);
    solver_.SetParameters(sat_params);

    const int64 old_num_conflicts = solver_.num_failures();
    const sat::SatSolver::Status sat_status =
        assumptions_already_added_ ? solver_.Solve() : SolveWithAssumptions();
    time_limit->AdvanceDeterministicTime(solver_.deterministic_time() -
                                         deterministic_time_at_last_sync);
    deterministic_time_at_last_sync = solver_.deterministic_time();

    assumptions_already_added_ = true;
    conflict_limit -= solver_.num_failures() - old_num_conflicts;
    learned_info->lower_bound = lower_bound_.value() - offset_.value();

    // This is possible because we over-constrain the objective.
    if (sat_status == sat::SatSolver::MODEL_UNSAT) {
      return problem_state.solution().IsFeasible()
                 ? BopOptimizerBase::OPTIMAL_SOLUTION_FOUND
                 : BopOptimizerBase::INFEASIBLE;
    }

    ExtractLearnedInfoFromSatSolver(&solver_, learned_info);
    if (sat_status == sat::SatSolver::LIMIT_REACHED || conflict_limit < 0) {
      return BopOptimizerBase::CONTINUE;
    }
    if (sat_status == sat::SatSolver::MODEL_SAT) {
      const sat::Coefficient old_lower_bound = stratified_lower_bound_;
      for (sat::EncodingNode* n : nodes_) {
        if (n->weight() < old_lower_bound) {
          if (stratified_lower_bound_ == old_lower_bound ||
              n->weight() > stratified_lower_bound_) {
            stratified_lower_bound_ = n->weight();
          }
        }
      }

      // We found a better solution!
      SatAssignmentToBopSolution(solver_.Assignment(), &learned_info->solution);
      if (stratified_lower_bound_ < old_lower_bound) {
        assumptions_already_added_ = false;
        return BopOptimizerBase::SOLUTION_FOUND;
      }
      return BopOptimizerBase::OPTIMAL_SOLUTION_FOUND;
    }

    // The interesting case: we have a core.
    // TODO(user): Check that this cannot fail because of the conflict limit.
    std::vector<sat::Literal> core = solver_.GetLastIncompatibleDecisions();
    sat::MinimizeCore(&solver_, &core);

    // TODO(user): make a function.
    sat::Coefficient min_weight = sat::kCoefficientMax;
    {
      int index = 0;
      for (int i = 0; i < core.size(); ++i) {
        for (; index < nodes_.size() &&
                   nodes_[index]->literal(0).Negated() != core[i];
             ++index) {
        }
        CHECK_LT(index, nodes_.size());
        min_weight = std::min(min_weight, nodes_[index]->weight());
      }
    }
    solver_.Backtrack(0);
    assumptions_already_added_ = false;

    int new_node_index = 0;
    if (core.size() == 1) {
      CHECK(solver_.Assignment().LiteralIsFalse(core[0]));
      for (sat::EncodingNode* n : nodes_) {
        if (n->literal(0).Negated() == core[0]) {
          sat::IncreaseNodeSize(n, &solver_);
          continue;
        }
      }
    } else {
      int index = 0;
      std::vector<sat::EncodingNode*> to_merge;
      for (int i = 0; i < core.size(); ++i) {
        for (; nodes_[index]->literal(0).Negated() != core[i]; ++index) {
          CHECK_LT(index, nodes_.size());
          nodes_[new_node_index] = nodes_[index];
          ++new_node_index;
        }
        CHECK_LT(index, nodes_.size());
        to_merge.push_back(nodes_[index]);
        if (nodes_[index]->weight() > min_weight) {
          nodes_[index]->set_weight(nodes_[index]->weight() - min_weight);
          nodes_[new_node_index] = nodes_[index];
          ++new_node_index;
        }
        ++index;
      }
      for (; index < nodes_.size(); ++index) {
        nodes_[new_node_index] = nodes_[index];
        ++new_node_index;
      }
      nodes_.resize(new_node_index);
      nodes_.push_back(
          sat::LazyMergeAllNodeWithPQ(to_merge, &solver_, &repository_));
      sat::IncreaseNodeSize(nodes_.back(), &solver_);
      nodes_.back()->set_weight(min_weight);
      CHECK(solver_.AddUnitClause(nodes_.back()->literal(0)));
    }
  }
  return BopOptimizerBase::CONTINUE;
}

}  // namespace bop
}  // namespace operations_research
