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

#include "sat/simplification.h"

#include "base/timer.h"
#include "base/strongly_connected_components.h"
#include "base/stl_util.h"
#include "algorithms/dynamic_partition.h"

namespace operations_research {
namespace sat {

SatPostsolver::SatPostsolver(int num_variables) {
  reverse_mapping_.resize(num_variables);
  for (VariableIndex var(0); var < num_variables; ++var) {
    reverse_mapping_[var] = var;
  }
  assignment_.Resize(num_variables);
}

void SatPostsolver::Add(Literal x, const std::vector<Literal>& clause) {
  CHECK(!clause.empty());
  DCHECK(std::find(clause.begin(), clause.end(), x) != clause.end());
  associated_literal_.push_back(ApplyReverseMapping(x));
  clauses_start_.push_back(clauses_literals_.size());
  for (const Literal& l : clause) {
    clauses_literals_.push_back(ApplyReverseMapping(l));
  }
}

void SatPostsolver::FixVariable(Literal x) {
  Literal l = ApplyReverseMapping(x);
  CHECK(!assignment_.IsLiteralAssigned(l));
  assignment_.AssignFromTrueLiteral(l);
}

void SatPostsolver::ApplyMapping(
    const ITIVector<VariableIndex, VariableIndex>& mapping) {
  ITIVector<VariableIndex, VariableIndex> new_mapping(reverse_mapping_.size(),
                                                      VariableIndex(-1));
  for (VariableIndex v(0); v < mapping.size(); ++v) {
    const VariableIndex image = mapping[v];
    if (image != VariableIndex(-1)) {
      CHECK_EQ(new_mapping[image], VariableIndex(-1));
      CHECK_LT(v, reverse_mapping_.size());
      CHECK_NE(reverse_mapping_[v], VariableIndex(-1));
      new_mapping[image] = reverse_mapping_[v];
    }
  }
  std::swap(new_mapping, reverse_mapping_);
}

Literal SatPostsolver::ApplyReverseMapping(Literal l) {
  CHECK_LT(l.Variable(), reverse_mapping_.size());
  CHECK_NE(reverse_mapping_[l.Variable()], VariableIndex(-1));
  return Literal(reverse_mapping_[l.Variable()], l.IsPositive());
}

void SatPostsolver::Postsolve(VariablesAssignment* assignment) const {
  // First, we set all unassigned variable to true.
  // This will be a valid assignment of the presolved problem.
  for (VariableIndex var(0); var < assignment->NumberOfVariables(); ++var) {
    if (!assignment->VariableIsAssigned(var)) {
      assignment->AssignFromTrueLiteral(Literal(var, true));
    }
  }

  int previous_start = clauses_literals_.size();
  for (int i = static_cast<int>(clauses_start_.size()) - 1; i >= 0; --i) {
    bool set_associated_var = true;
    const int new_start = clauses_start_[i];
    for (int j = new_start; j < previous_start; ++j) {
      if (assignment->LiteralIsTrue(clauses_literals_[j])) {
        set_associated_var = false;
        break;
      }
    }
    previous_start = new_start;
    if (set_associated_var) {
      // Note(user): The VariablesAssignment interface is a bit weird in this
      // context, because we can only assign an unassigned literal.
      assignment->UnassignLiteral(associated_literal_[i]);
      assignment->AssignFromTrueLiteral(associated_literal_[i]);
    }
  }
}

std::vector<bool> SatPostsolver::ExtractAndPostsolveSolution(
    const SatSolver& solver) {
  std::vector<bool> solution(solver.NumVariables());
  for (VariableIndex var(0); var < solver.NumVariables(); ++var) {
    CHECK(solver.Assignment().VariableIsAssigned(var));
    solution[var.value()] =
        solver.Assignment().LiteralIsTrue(Literal(var, true));
  }
  return PostsolveSolution(solution);
}

std::vector<bool> SatPostsolver::PostsolveSolution(const std::vector<bool>& solution) {
  for (VariableIndex var(0); var < solution.size(); ++var) {
    CHECK_LT(var, reverse_mapping_.size());
    CHECK_NE(reverse_mapping_[var], VariableIndex(-1));
    CHECK(!assignment_.VariableIsAssigned(reverse_mapping_[var]));
    assignment_.AssignFromTrueLiteral(
        Literal(reverse_mapping_[var], solution[var.value()]));
  }
  Postsolve(&assignment_);
  std::vector<bool> postsolved_solution;
  for (int i = 0; i < reverse_mapping_.size(); ++i) {
    postsolved_solution.push_back(
        assignment_.LiteralIsTrue(Literal(VariableIndex(i), true)));
  }
  return postsolved_solution;
}

void SatPresolver::AddBinaryClause(Literal a, Literal b) {
  Literal c[2];
  c[0] = a;
  c[1] = b;
  AddClause(ClauseRef(c, c + 2));
}

void SatPresolver::AddClause(ClauseRef clause) {
  CHECK_GT(clause.size(), 0) << "Added an empty clause to the presolver";
  const ClauseIndex ci(clauses_.size());
  clauses_.push_back(std::vector<Literal>(clause.begin(), clause.end()));
  in_clause_to_process_.push_back(true);
  clause_to_process_.push_back(ci);

  std::vector<Literal>& clause_ref = clauses_.back();
  if (!equiv_mapping_.empty()) {
    for (int i = 0; i < clause_ref.size(); ++i) {
      clause_ref[i] = Literal(equiv_mapping_[clause_ref[i].Index()]);
    }
  }
  std::sort(clause_ref.begin(), clause_ref.end());
  clause_ref.erase(std::unique(clause_ref.begin(), clause_ref.end()),
                   clause_ref.end());

  // Check for trivial clauses:
  for (int i = 1; i < clause_ref.size(); ++i) {
    if (clause_ref[i] == clause_ref[i - 1].Negated()) {
      // The clause is trivial!
      ++num_trivial_clauses_;
      clause_to_process_.pop_back();
      clauses_.pop_back();
      in_clause_to_process_.pop_back();
      return;
    }
  }

  const Literal max_literal = clause_ref.back();
  const int required_size =
      std::max(max_literal.Index().value(), max_literal.NegatedIndex().value()) + 1;
  if (required_size > literal_to_clauses_.size()) {
    literal_to_clauses_.resize(required_size);
    literal_to_clause_sizes_.resize(required_size);
  }
  for (Literal e : clause_ref) {
    literal_to_clauses_[e.Index()].push_back(ci);
    literal_to_clause_sizes_[e.Index()]++;
  }
}

void SatPresolver::AddClauseInternal(std::vector<Literal>* clause) {
  CHECK_GT(clause->size(), 0) << "TODO(fdid): Unsat during presolve?";
  const ClauseIndex ci(clauses_.size());
  clauses_.push_back(std::vector<Literal>());
  clauses_.back().swap(*clause);
  in_clause_to_process_.push_back(true);
  clause_to_process_.push_back(ci);
  for (Literal e : clauses_.back()) {
    literal_to_clauses_[e.Index()].push_back(ci);
    literal_to_clause_sizes_[e.Index()]++;
  }
}

ITIVector<VariableIndex, VariableIndex> SatPresolver::VariableMapping() const {
  ITIVector<VariableIndex, VariableIndex> result;
  VariableIndex new_var(0);
  for (VariableIndex var(0); var < NumVariables(); ++var) {
    if (literal_to_clause_sizes_[Literal(var, true).Index()] > 0 ||
        literal_to_clause_sizes_[Literal(var, false).Index()] > 0) {
      result.push_back(new_var);
      ++new_var;
    } else {
      result.push_back(VariableIndex(-1));
    }
  }
  return result;
}

void SatPresolver::LoadProblemIntoSatSolver(SatSolver* solver) {
  // Cleanup some memory that is not needed anymore. Note that we do need
  // literal_to_clause_sizes_ for VariableMapping() to work.
  var_pq_.Clear();
  var_pq_elements_.clear();
  in_clause_to_process_.clear();
  clause_to_process_.clear();
  literal_to_clauses_.clear();

  const ITIVector<VariableIndex, VariableIndex> mapping = VariableMapping();
  int new_size = 0;
  for (VariableIndex index : mapping) {
    if (index != VariableIndex(-1)) ++new_size;
  }

  std::vector<Literal> temp;
  solver->SetNumVariables(new_size);
  for (std::vector<Literal>& clause_ref : clauses_) {
    temp.clear();
    for (Literal l : clause_ref) {
      CHECK_NE(mapping[l.Variable()], VariableIndex(-1));
      temp.push_back(Literal(mapping[l.Variable()], l.IsPositive()));
    }
    if (!temp.empty()) solver->AddProblemClause(temp);
    STLClearObject(&clause_ref);
  }
}

bool SatPresolver::ProcessAllClauses() {
  while (!clause_to_process_.empty()) {
    const ClauseIndex ci = clause_to_process_.front();
    in_clause_to_process_[ci] = false;
    clause_to_process_.pop_front();
    if (!ProcessClauseToSimplifyOthers(ci)) return false;
  }
  return true;
}

bool SatPresolver::Presolve() {
  WallTimer timer;
  timer.Start();
  LOG(INFO) << "num trivial clauses: " << num_trivial_clauses_;
  DisplayStats(0);

  // TODO(user): When a clause is strengthened, add it to a queue so it can
  // be processed again?
  if (!ProcessAllClauses()) return false;
  DisplayStats(timer.Get());

  InitializePriorityQueue();
  while (var_pq_.Size() > 0) {
    VariableIndex var = var_pq_.Top()->variable;
    var_pq_.Pop();
    if (CrossProduct(Literal(var, true))) {
      if (!ProcessAllClauses()) return false;
    }
  }

  DisplayStats(timer.Get());
  return true;
}

// TODO(user): Binary clauses are really common, and we can probably do this
// more efficiently for them. For instance, we could just take the intersection
// of two sorted lists to get the simplified clauses.
//
// TODO(user): SimplifyClause can returns true only if the variables in 'a' are
// a subset of the one in 'b'. Use an int64 signature for speeding up the test.
bool SatPresolver::ProcessClauseToSimplifyOthers(ClauseIndex clause_index) {
  const std::vector<Literal>& clause = clauses_[clause_index];
  if (clause.empty()) return true;
  DCHECK(std::is_sorted(clause.begin(), clause.end()));

  LiteralIndex opposite_literal;
  const Literal lit = FindLiteralWithShortestOccurenceList(clause);

  // Try to simplify the clauses containing 'lit'. We take advantage of this
  // loop to also remove the empty sets from the list.
  {
    int new_index = 0;
    std::vector<ClauseIndex>& occurence_list_ref = literal_to_clauses_[lit.Index()];
    for (ClauseIndex ci : occurence_list_ref) {
      if (clauses_[ci].empty()) continue;
      if (ci != clause_index &&
          SimplifyClause(clause, &clauses_[ci], &opposite_literal)) {
        if (opposite_literal == LiteralIndex(-1)) {
          Remove(ci);
          continue;
        } else {
          CHECK_NE(opposite_literal, lit.Index());
          if (clauses_[ci].empty()) return false;  // UNSAT.
          // Remove ci from the occurence list. Note that the occurence list
          // can't be shortest_list or its negation.
          auto iter =
              std::find(literal_to_clauses_[opposite_literal].begin(),
                        literal_to_clauses_[opposite_literal].end(), ci);
          DCHECK(iter != literal_to_clauses_[opposite_literal].end());
          literal_to_clauses_[opposite_literal].erase(iter);

          --literal_to_clause_sizes_[opposite_literal];
          UpdatePriorityQueue(Literal(opposite_literal).Variable());

          if (!in_clause_to_process_[ci]) {
            in_clause_to_process_[ci] = true;
            clause_to_process_.push_back(ci);
          }
        }
      }
      occurence_list_ref[new_index] = ci;
      ++new_index;
    }
    occurence_list_ref.resize(new_index);
    CHECK_EQ(literal_to_clause_sizes_[lit.Index()], new_index);
    literal_to_clause_sizes_[lit.Index()] = new_index;
  }

  // Now treat clause containing lit.Negated().
  // TODO(user): choose a potentially smaller list.
  {
    int new_index = 0;
    bool something_removed = false;
    std::vector<ClauseIndex>& occurence_list_ref =
        literal_to_clauses_[lit.NegatedIndex()];
    for (ClauseIndex ci : occurence_list_ref) {
      if (clauses_[ci].empty()) continue;

      // TODO(user): not super optimal since we could abort earlier if
      // opposite_literal is not the negation of shortest_list.
      if (SimplifyClause(clause, &clauses_[ci], &opposite_literal)) {
        CHECK_EQ(opposite_literal, lit.NegatedIndex());
        if (clauses_[ci].empty()) return false;  // UNSAT.
        if (!in_clause_to_process_[ci]) {
          in_clause_to_process_[ci] = true;
          clause_to_process_.push_back(ci);
        }
        something_removed = true;
        continue;
      }
      occurence_list_ref[new_index] = ci;
      ++new_index;
    }
    occurence_list_ref.resize(new_index);
    literal_to_clause_sizes_[lit.NegatedIndex()] = new_index;
    if (something_removed) {
      UpdatePriorityQueue(Literal(lit.NegatedIndex()).Variable());
    }
  }
  return true;
}

void SatPresolver::RemoveAndRegisterForPostsolveAllClauseContaining(Literal x) {
  for (ClauseIndex i : literal_to_clauses_[x.Index()]) {
    if (!clauses_[i].empty()) RemoveAndRegisterForPostsolve(i, x);
  }
  STLClearObject(&literal_to_clauses_[x.Index()]);
  literal_to_clause_sizes_[x.Index()] = 0;
}

bool SatPresolver::CrossProduct(Literal x) {
  const int s1 = literal_to_clause_sizes_[x.Index()];
  const int s2 = literal_to_clause_sizes_[x.NegatedIndex()];

  // Note that if s1 or s2 is equal to 0, this function will implicitely just
  // fix the variable x.
  if (s1 == 0 && s2 == 0) return false;

  // Heuristic. Abort if the work required to decide if x should be removed
  // seems to big.
  if (s1 > 1 && s2 > 1 && s1 * s2 > parameters_.presolve_bve_threshold()) {
    return false;
  }

  // Compute the threshold under which we don't remove x.Variable().
  int threshold = 0;
  const int clause_weight = parameters_.presolve_bve_clause_weight();
  for (ClauseIndex i : literal_to_clauses_[x.Index()]) {
    if (!clauses_[i].empty()) {
      threshold += clause_weight + clauses_[i].size();
    }
  }
  for (ClauseIndex i : literal_to_clauses_[x.NegatedIndex()]) {
    if (!clauses_[i].empty()) {
      threshold += clause_weight + clauses_[i].size();
    }
  }

  // For the BCE, we prefer s2 to be small.
  if (s1 < s2) x = x.Negated();

  // Test whether we should remove the x.Variable().
  int size = 0;
  for (ClauseIndex i : literal_to_clauses_[x.Index()]) {
    if (clauses_[i].empty()) continue;
    bool no_resolvant = true;
    for (ClauseIndex j : literal_to_clauses_[x.NegatedIndex()]) {
      if (clauses_[j].empty()) continue;
      const int rs = ComputeResolvantSize(x, clauses_[i], clauses_[j]);
      if (rs >= 0) {
        no_resolvant = false;
        size += clause_weight + rs;

        // Abort early if the "size" become too big.
        if (size > threshold) return false;
      }
    }
    if (no_resolvant) {
      // This is an incomplete heuristic for blocked clause detection. Here,
      // the clause i is "blocked", so we can remove it. Note that the code
      // below already do that if we decide to eliminate x.
      //
      // For more details, see the paper "Blocked clause elimination", Matti
      // Jarvisalo, Armin Biere, Marijn Heule. TACAS, volume 6015 of Lecture
      // Notes in Computer Science, pages 129–144. Springer, 2010.
      //
      // TODO(user): Choose if we use x or x.Negated() depending on the list
      // sizes? The function achieve the same if x = x.Negated(), however the
      // loops are not done in the same order which may change this incomplete
      // "blocked" clause detection.
      RemoveAndRegisterForPostsolve(i, x);
    }
  }

  // Add all the resolvant clauses.
  // Note that the variable priority queue will only be updated during the
  // deletion.
  std::vector<Literal> temp;
  for (ClauseIndex i : literal_to_clauses_[x.Index()]) {
    if (clauses_[i].empty()) continue;
    for (ClauseIndex j : literal_to_clauses_[x.NegatedIndex()]) {
      if (clauses_[j].empty()) continue;
      if (ComputeResolvant(x, clauses_[i], clauses_[j], &temp)) {
        AddClauseInternal(&temp);
      }
    }
  }

  // Deletes the old clauses.
  //
  // TODO(user): We could only update the priority queue once for each variable
  // instead of doing it many times.
  RemoveAndRegisterForPostsolveAllClauseContaining(x);
  RemoveAndRegisterForPostsolveAllClauseContaining(x.Negated());

  // TODO(user): At this point x.Variable() is added back to the priority queue.
  // Avoid doing that.
  return true;
}

void SatPresolver::Remove(ClauseIndex ci) {
  for (Literal e : clauses_[ci]) {
    literal_to_clause_sizes_[e.Index()]--;
    UpdatePriorityQueue(e.Variable());
  }
  STLClearObject(&clauses_[ci]);
}

void SatPresolver::RemoveAndRegisterForPostsolve(ClauseIndex ci, Literal x) {
  for (Literal e : clauses_[ci]) {
    literal_to_clause_sizes_[e.Index()]--;
    UpdatePriorityQueue(e.Variable());
  }
  postsolver_->Add(x, clauses_[ci]);
  STLClearObject(&clauses_[ci]);
}

Literal SatPresolver::FindLiteralWithShortestOccurenceList(
    const std::vector<Literal>& clause) {
  CHECK(!clause.empty());
  Literal result = clause.front();
  for (const Literal l : clause) {
    if (literal_to_clause_sizes_[l.Index()] <
        literal_to_clause_sizes_[result.Index()]) {
      result = l;
    }
  }
  return result;
}

void SatPresolver::UpdatePriorityQueue(VariableIndex var) {
  if (var_pq_elements_.empty()) return;  // not initialized.
  PQElement* element = &var_pq_elements_[var];
  element->weight = literal_to_clause_sizes_[Literal(var, true).Index()] +
                    literal_to_clause_sizes_[Literal(var, false).Index()];
  if (var_pq_.Contains(element)) {
    var_pq_.NoteChangedPriority(element);
  } else {
    var_pq_.Add(element);
  }
}

void SatPresolver::InitializePriorityQueue() {
  const int num_vars = NumVariables();
  var_pq_elements_.resize(num_vars);
  for (VariableIndex var(0); var < num_vars; ++var) {
    PQElement* element = &var_pq_elements_[var];
    element->variable = var;
    element->weight = literal_to_clause_sizes_[Literal(var, true).Index()] +
                      literal_to_clause_sizes_[Literal(var, false).Index()];
    var_pq_.Add(element);
  }
}

void SatPresolver::DisplayStats(double elapsed_seconds) {
  int num_literals = 0;
  int num_clauses = 0;
  int num_singleton_clauses = 0;
  for (const std::vector<Literal>& c : clauses_) {
    if (!c.empty()) {
      if (c.size() == 1) ++num_singleton_clauses;
      ++num_clauses;
      num_literals += c.size();
    }
  }
  int num_one_side = 0;
  int num_simple_definition = 0;
  int num_vars = 0;
  for (VariableIndex var(0); var < NumVariables(); ++var) {
    const int s1 = literal_to_clause_sizes_[Literal(var, true).Index()];
    const int s2 = literal_to_clause_sizes_[Literal(var, false).Index()];
    if (s1 == 0 && s2 == 0) continue;

    ++num_vars;
    if (s1 == 0 || s2 == 0) {
      num_one_side++;
    } else if (s1 == 1 || s2 == 1) {
      num_simple_definition++;
    }
  }
  LOG(INFO) << " [" << elapsed_seconds << "s]"
            << " clauses:" << num_clauses << " literals:" << num_literals
            << " vars:" << num_vars << " one_side_vars:" << num_one_side
            << " simple_definition:" << num_simple_definition
            << " singleton_clauses:" << num_singleton_clauses;
}

bool SimplifyClause(const std::vector<Literal>& a, std::vector<Literal>* b,
                    LiteralIndex* opposite_literal) {
  if (b->size() < a.size()) return false;
  DCHECK(std::is_sorted(a.begin(), a.end()));
  DCHECK(std::is_sorted(b->begin(), b->end()));

  *opposite_literal = LiteralIndex(-1);

  int num_diff = 0;
  std::vector<Literal>::const_iterator ia = a.begin();
  std::vector<Literal>::iterator ib = b->begin();
  std::vector<Literal>::iterator to_remove = b->begin();

  // Because we abort early when size_diff becomes negative, the second test
  // in the while loop is not needed.
  int size_diff = b->size() - a.size();
  while (ia != a.end() /* && ib != b->end() */) {
    if (*ia == *ib) {  // Same literal.
      ++ia;
      ++ib;
    } else if (*ia == ib->Negated()) {  // Opposite literal.
      ++num_diff;
      if (num_diff > 1) return false;  // Too much difference.
      to_remove = ib;
      ++ia;
      ++ib;
    } else if (*ia < *ib) {
      return false;  // A literal of a is not in b.
    } else {         // *ia > *ib
      ++ib;

      // A literal of b is not in a, we can abort early by comparing the sizes
      // left.
      if (--size_diff < 0) return false;
    }
  }
  if (num_diff == 1) {
    *opposite_literal = to_remove->Index();
    b->erase(to_remove);
  }
  return true;
}

bool ComputeResolvant(Literal x, const std::vector<Literal>& a,
                      const std::vector<Literal>& b, std::vector<Literal>* out) {
  DCHECK(std::is_sorted(a.begin(), a.end()));
  DCHECK(std::is_sorted(b.begin(), b.end()));

  out->clear();
  std::vector<Literal>::const_iterator ia = a.begin();
  std::vector<Literal>::const_iterator ib = b.begin();
  while ((ia != a.end()) && (ib != b.end())) {
    if (*ia == *ib) {
      out->push_back(*ia);
      ++ia;
      ++ib;
    } else if (*ia == ib->Negated()) {
      if (*ia != x) return false;  // Trivially true.
      DCHECK_EQ(*ib, x.Negated());
      ++ia;
      ++ib;
    } else if (*ia < *ib) {
      out->push_back(*ia);
      ++ia;
    } else {  // *ia > *ib
      out->push_back(*ib);
      ++ib;
    }
  }

  // Copy remaining literals.
  out->insert(out->end(), ia, a.end());
  out->insert(out->end(), ib, b.end());
  return true;
}

// Note that this function takes a big chunk of the presolve running time.
int ComputeResolvantSize(Literal x, const std::vector<Literal>& a,
                         const std::vector<Literal>& b) {
  DCHECK(std::is_sorted(a.begin(), a.end()));
  DCHECK(std::is_sorted(b.begin(), b.end()));

  int size = static_cast<int>(a.size() + b.size()) - 2;
  std::vector<Literal>::const_iterator ia = a.begin();
  std::vector<Literal>::const_iterator ib = b.begin();
  while ((ia != a.end()) && (ib != b.end())) {
    if (*ia == *ib) {
      --size;
      ++ia;
      ++ib;
    } else if (*ia == ib->Negated()) {
      if (*ia != x) return -1;  // Trivially true.
      DCHECK_EQ(*ib, x.Negated());
      ++ia;
      ++ib;
    } else if (*ia < *ib) {
      ++ia;
    } else {  // *ia > *ib
      ++ib;
    }
  }
  DCHECK_GE(size, 0);
  return size;
}

// A simple graph where the nodes are the literals and the nodes adjacent to a
// literal l are the propagated literal when l is assigned in the underlying
// sat solver.
//
// This can be used to do a strong component analysis while probing all the
// literals of a solver. Note that this can be expensive, hence the support
// for a deterministic time limit.
class PropagationGraph {
 public:
  PropagationGraph(double deterministic_time_limit, SatSolver* solver)
      : solver_(solver),
        deterministic_time_limit(solver->deterministic_time() +
                                 deterministic_time_limit) {}

  // Returns the set of node adjacent to the given one.
  // Interface needed by FindStronglyConnectedComponents(), note that it needs
  // to be const.
  const std::vector<int32>& operator[](int32 index) const {
    scratchpad_.clear();
    solver_->Backtrack(0);

    // Note that when the time limit is reached, we just keep returning empty
    // adjacency list. This way, the SCC algorithm will terminate quickly and
    // the equivalent literals detection will be incomplete but correct. Note
    // also that thanks to the SCC algorithm, we will explore the connected
    // components first.
    if (solver_->deterministic_time() > deterministic_time_limit) {
      return scratchpad_;
    }

    const Literal l = Literal(LiteralIndex(index));
    if (!solver_->Assignment().IsLiteralAssigned(l)) {
      const int trail_index = solver_->LiteralTrail().Index();
      solver_->EnqueueDecisionAndBackjumpOnConflict(l);
      if (solver_->CurrentDecisionLevel() > 0) {
        // Note that the +1 is to avoid adding a => a.
        for (int i = trail_index + 1; i < solver_->LiteralTrail().Index();
             ++i) {
          scratchpad_.push_back(solver_->LiteralTrail()[i].Index().value());
        }
      }
    }
    return scratchpad_;
  }

 private:
  mutable std::vector<int32> scratchpad_;
  SatSolver* const solver_;
  const double deterministic_time_limit;

  DISALLOW_COPY_AND_ASSIGN(PropagationGraph);
};

void ProbeAndFindEquivalentLiteral(
    SatSolver* solver, SatPostsolver* postsolver,
    ITIVector<LiteralIndex, LiteralIndex>* mapping) {
  solver->Backtrack(0);
  mapping->clear();
  const int num_already_fixed_vars = solver->LiteralTrail().Index();

  PropagationGraph graph(
      solver->parameters().presolve_probing_deterministic_time_limit(), solver);
  const int32 size = solver->NumVariables() * 2;
  std::vector<std::vector<int32>> scc;
  FindStronglyConnectedComponents(size, graph, &scc);

  // We have no guarantee that the cycle of x and not(x) touch the same
  // variables. This is because we may have more info for the literal probed
  // later or the propagation may go only in one direction. For instance if we
  // have two clauses (not(x1) v x2) and (not(x1) v not(x2) v x3) then x1
  // implies x2 and x3 but not(x3) doesn't imply anything by unit propagation.
  //
  // TODO(user): Add some constraint so that it does?
  //
  // Because of this, we "merge" the cycles.
  MergingPartition partition(size);
  for (const std::vector<int32>& component : scc) {
    if (component.size() > 1) {
      if (mapping->empty()) mapping->resize(size, LiteralIndex(-1));
      const Literal representative((LiteralIndex(component[0])));
      for (int i = 1; i < component.size(); ++i) {
        const Literal l((LiteralIndex(component[i])));
        // TODO(user): check compatibility? if x ~ not(x) => unsat.
        // but probably, the solver would have found this too? not sure...
        partition.MergePartsOf(representative.Index().value(),
                               l.Index().value());
        partition.MergePartsOf(representative.NegatedIndex().value(),
                               l.NegatedIndex().value());
      }

      // We rely on the fact that the representative of a literal x and the one
      // of its negation are the same variable.
      CHECK_EQ(Literal(LiteralIndex(partition.GetRootAndCompressPath(
                   representative.Index().value()))),
               Literal(LiteralIndex(partition.GetRootAndCompressPath(
                           representative.NegatedIndex().value()))).Negated());
    }
  }

  solver->Backtrack(0);
  int num_equiv = 0;
  std::vector<Literal> temp;
  if (!mapping->empty()) {
    // If a variable in a cycle is fixed. We want to fix all of them.
    const VariablesAssignment& assignment = solver->Assignment();
    for (LiteralIndex i(0); i < size; ++i) {
      const LiteralIndex rep(partition.GetRootAndCompressPath(i.value()));
      if (assignment.IsLiteralAssigned(Literal(i)) &&
          !assignment.IsLiteralAssigned(Literal(rep))) {
        solver->AddUnitClause(assignment.LiteralIsTrue(Literal(i))
                                  ? Literal(rep)
                                  : Literal(rep).Negated());
      }
    }

    for (LiteralIndex i(0); i < size; ++i) {
      const LiteralIndex rep(partition.GetRootAndCompressPath(i.value()));
      (*mapping)[i] = rep;
      if (assignment.IsLiteralAssigned(Literal(rep))) {
        if (!assignment.IsLiteralAssigned(Literal(i))) {
          solver->AddUnitClause(assignment.LiteralIsTrue(Literal(rep))
                                    ? Literal(Literal(i))
                                    : Literal(i).Negated());
        }
      } else if (rep != i) {
        CHECK(!solver->Assignment().IsLiteralAssigned(Literal(i)));
        CHECK(!solver->Assignment().IsLiteralAssigned(Literal(rep)));
        ++num_equiv;
        temp.clear();
        temp.push_back(Literal(i));
        temp.push_back(Literal(rep).Negated());
        postsolver->Add(Literal(i), temp);
      }
    }
  }

  LOG(INFO) << "Probing. fixed " << num_already_fixed_vars << " + "
            << solver->LiteralTrail().Index() - num_already_fixed_vars
            << " equiv " << num_equiv / 2 << " total "
            << solver->NumVariables();
}

}  // namespace sat
}  // namespace operations_research
