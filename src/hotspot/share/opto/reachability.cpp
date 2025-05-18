/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "opto/c2_MacroAssembler.hpp"
#include "opto/callnode.hpp"
#include "opto/compile.hpp"
#include "opto/loopnode.hpp"
#include "opto/phaseX.hpp"
#include "opto/reachability.hpp"
#include "opto/regalloc.hpp"
#include "opto/runtime.hpp"

// RF is redundant when there's another
// other_referent <== referent <== ctrl <== use
static bool is_redundant_rf_helper(Node* ctrl, Node* referent, PhaseIdealLoop* phase, PhaseGVN& gvn, bool cfg_only) {
  const Type* t = gvn.type(referent);
  if (EliminateConstantReachabilityFence && t->singleton()) {
    return true; // no-op fence
  }
  if (t == TypePtr::NULL_PTR) {
    return true; // no-op fence
  }
  for (Node* cur = referent;
       cur != nullptr;
       cur = (cur->is_ConstraintCast() ? cur->in(1) : nullptr)) {
    for (DUIterator_Fast imax, i = cur->fast_outs(imax); i < imax; i++) {
      Node* use = cur->fast_out(i);
      if (cfg_only && !use->is_CFG()) {
        continue; // skip non-CFG uses
      }
      if (use != ctrl) {
        if (phase != nullptr) {
          Node* use_ctrl = (cfg_only ? use : phase->ctrl_or_self(use));
          if (phase->is_dominator(ctrl, use_ctrl)) {
            return true;
          }
        } else {
          assert(cfg_only, "");
          if (gvn.is_dominator(ctrl, use)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

Node* ReachabilityFenceNode::Ideal(PhaseGVN* phase, bool can_reshape) {
  if (remove_dead_region(phase, can_reshape)) {
    return this;
  }
  if (in(0) != nullptr && in(0)->is_top()) {
    return nullptr;
  }
  if (is_redundant_rf_helper(this, in(1), nullptr, *phase, true /*cfg_only*/)) {
    return TupleNode::make(TypeTuple::MEMBAR, in(0), nullptr, nullptr, nullptr, nullptr);
  }
  return nullptr;
}

#ifndef PRODUCT
static void rf_desc(outputStream* st, const ReachabilityFenceNode* rf, PhaseRegAlloc* ra) {
  char buf[50];
  ra->dump_register(rf->in(1), buf, sizeof(buf));
  st->print("reachability fence [%s]", buf);
}

void ReachabilityFenceNode::format(PhaseRegAlloc* ra, outputStream* st) const {
  rf_desc(st, this, ra);
}

void ReachabilityFenceNode::emit(C2_MacroAssembler* masm, PhaseRegAlloc* ra) const {
  ResourceMark rm;
  stringStream ss;
  rf_desc(&ss, this, ra);
  const char* desc = masm->code_string(ss.freeze());
  masm->block_comment(desc);
}
#endif

// Detect safepoint nodes which are important for reachability tracking purposes.
static bool is_significant_sfpt(Node* n) {
  if (n->is_SafePoint()) {
    SafePointNode* sfpt = n->as_SafePoint();
    if (!sfpt->guaranteed_safepoint()) {
      return false; // not a real safepoint
    } else if (sfpt->is_CallStaticJava() && sfpt->as_CallStaticJava()->is_uncommon_trap()) {
      return false; // uncommon traps are exit points
    }
    return true;
  }
  return false;
}

static void replace_node(Node* old_node, Node* new_node, PhaseIdealLoop* phase) {
  IdealLoopTree* lpt = phase->get_loop(old_node);
  if (!lpt->is_root()) {
    lpt->_body.yank(old_node);
  }
  if (old_node->is_ReachabilityFence()) {
    assert(lpt->_rfs != nullptr, "missing");
    assert(lpt->_rfs->contains(old_node), "missing");
    lpt->_rfs->yank(old_node);
  }
  phase->lazy_replace(old_node, new_node);
}

static void insert_reachability_fence(Node* ctrl_start, Node* referent, PhaseIdealLoop* phase) {
  IdealLoopTree* lpt = phase->get_loop(ctrl_start);
  Node* ctrl_end = ctrl_start->unique_ctrl_out();

  Node* new_rf = new ReachabilityFenceNode(phase->C, ctrl_start, referent);

  phase->register_control(new_rf, lpt, ctrl_start);
  if (lpt->_rfs == nullptr) {
    lpt->_rfs = new Node_List();
  }
  lpt->_rfs->push(new_rf);

  Node* new_rf_proj = new ProjNode(new_rf, TypeFunc::Control);
  phase->register_control(new_rf_proj, lpt, new_rf);

  phase->igvn().rehash_node_delayed(ctrl_end);
  ctrl_end->replace_edge(ctrl_start, new_rf_proj);

  if (phase->idom(ctrl_end) == ctrl_start) {
    phase->set_idom(ctrl_end, new_rf_proj, phase->dom_depth(new_rf_proj));
  } else {
    assert(ctrl_end->is_Region(), "");
  }
}

static bool remove_reachability_fence(Node* rf, PhaseIdealLoop* phase) {
  Node* referent = rf->in(1);
  if (phase->igvn().type(referent) != TypePtr::NULL_PTR) {
    phase->igvn().replace_input_of(rf, 1, phase->makecon(TypePtr::NULL_PTR));
    if (referent->outcnt() == 0) {
      phase->remove_dead_node(referent);
    }
  }

  Node* rf_ctrl_in   = rf->in(0);
  Node* rf_ctrl_proj = rf->unique_ctrl_out();

  replace_node(rf, rf_ctrl_in, phase);
  replace_node(rf_ctrl_proj, rf_ctrl_in, phase);

  return true;
}

//======================================================================
//---------------------------- Phase 1 ---------------------------------

#ifdef ASSERT
static void dump_rfs_on(outputStream* st, PhaseIdealLoop* phase, Unique_Node_List& ignored_rfs, bool cfg_only) {
  for (int i = 0; i < phase->C->reachability_fences_count(); i++) {
    Node* rf = phase->C->reachability_fence(i);
    Node* referent = rf->in(1);
    bool detected = ignored_rfs.member(rf);
    bool redundant = is_redundant_rf_helper(rf, referent, phase, phase->igvn(), cfg_only);

    st->print(" %3d: %s%s ", i, (redundant ? "R" : " "), (detected ? "D" : " "));
    rf->dump("", false, st);
    st->cr();

    st->print("         ");
    referent->dump("", false, st);
    st->cr();
    if (redundant != detected) {
      for (Node* cur = referent;
           cur != nullptr;
           cur = (cur->is_ConstraintCast() ? cur->in(1) : nullptr)) {
        bool first = true;
        for (DUIterator_Fast imax, i = cur->fast_outs(imax); i < imax; i++) {
          Node* use = cur->fast_out(i);
          if (cfg_only && !use->is_CFG()) {
            continue; // skip non-CFG uses
          }
          if (use != rf) {
            if (phase != nullptr) {
              Node* use_ctrl = (cfg_only ? use : phase->ctrl_or_self(use));
              if (phase->is_dominator(rf, use_ctrl)) {
                if (first) {
                  st->print("=====REF "); cur->dump("", false, st); st->cr();
                  first = false;
                }
                st->print("     DDD "); use_ctrl->dump("", false, st); st->cr();
                if (use != use_ctrl) {
                  st->print("         "); use->dump("", false, st); st->cr();
                }
              }
            } else {
              assert(cfg_only, "");
              if (phase->igvn().is_dominator(rf, use)) {
                if (first) {
                  st->print("=====REF "); cur->dump("", false, st); st->cr();
                  first = false;
                }
                st->print("     DDD "); use->dump("", false, st); st->cr();
              }
            }
          }
        }
      }
    }
  }
}

bool PhaseIdealLoop::has_redundant_rfs(Unique_Node_List& ignored_rfs, bool cfg_only) {
  for (int i = 0; i < C->reachability_fences_count(); i++) {
    Node* rf = C->reachability_fence(i);
    Node* referent = rf->in(1);
    assert(rf->outcnt() > 0, "dead node");
    if (ignored_rfs.member(rf)) {
      continue; // skip
    } else if (is_redundant_rf(rf, cfg_only)) {
      dump_rfs_on(tty, this, ignored_rfs, cfg_only);
      return true;
    }
  }
  return false;
}
#endif // ASSERT

static Node* counted_loop_exit(IdealLoopTree* lpt) {
  if (lpt->is_loop()) {
    if (lpt->head()->is_BaseCountedLoop()) {
      return lpt->head()->as_BaseCountedLoop()->loopexit()->proj_out_or_null(0 /* false */);
    }
    if (lpt->head()->is_OuterStripMinedLoop()) {
      return lpt->head()->as_OuterStripMinedLoop()->outer_loop_exit();
    }
  }
  return nullptr;
}

bool PhaseIdealLoop::is_redundant_rf(Node* rf, bool cfg_only) {
  assert(rf->is_ReachabilityFence(), "");
  Node* referent = rf->in(1);
  return is_redundant_rf_helper(rf, referent, this, igvn(), cfg_only);
}

bool PhaseIdealLoop::find_redundant_rfs(Unique_Node_List& redundant_rfs) {
  bool found = false;
  for (int i = 0; i < C->reachability_fences_count(); i++) {
    Node* rf = C->reachability_fence(i);
    Node* referent = rf->in(1);
    assert(rf->outcnt() > 0, "dead node");
    if (!redundant_rfs.member(rf) && is_redundant_rf(rf, true /*cfg_only*/)) {
      redundant_rfs.push(rf);
    }
  }
  return found;
}

bool PhaseIdealLoop::optimize_reachability_fences() {
  Compile::TracePhase tp(_t_reachability);

  if (!OptimizeReachabilityFence) {
    return false;
  }

  Unique_Node_List redundant_rfs;
  find_redundant_rfs(redundant_rfs);

  Node_List worklist;
  for (int i = 0; i < C->reachability_fences_count(); i++) {
    Node* rf = C->reachability_fence(i);
    if (!redundant_rfs.member(rf)) {
      // Move RFs out of counted loops when possible.
      IdealLoopTree* lpt = get_loop(rf);
      Node* referent = rf->in(1);

      if (lpt->is_invariant(referent) && counted_loop_exit(lpt) != nullptr) {
        // Switch to the outermost loop.
        for (IdealLoopTree* outer_loop = lpt->_parent;
             outer_loop->is_invariant(referent) && counted_loop_exit(outer_loop) != nullptr;
             outer_loop = outer_loop->_parent) {
          assert(is_member(outer_loop, rf), "");
          lpt = outer_loop;
        }
        worklist.push(referent);
        worklist.push(counted_loop_exit(lpt));
        redundant_rfs.push(rf);
      }
    }
  }

  // Populate RFs outside counted loops.
  while (worklist.size() > 0) {
    Node* ctrl_out = worklist.pop();
    Node* referent = worklist.pop();
    insert_reachability_fence(ctrl_out, referent, this);
  }

  // Redundancy is determined by dominance relation.
  // Sometimes it becomes evident that an RF is redundant once it is moved out of the loop.
  // Also, newly introduced RF can make some existing RFs redundant.
  find_redundant_rfs(redundant_rfs);

  // Eliminate redundant RFs.
  bool progress = false;
  if (redundant_rfs.size() > 0) {
    while (redundant_rfs.size() > 0) {
      Node* rf = redundant_rfs.pop();
      progress |= remove_reachability_fence(rf, this);
    }
  }

  assert(redundant_rfs.size() == 0, "");
  assert(!has_redundant_rfs(redundant_rfs, true /*cfg_only*/), "");

  return progress;
}

//======================================================================
//---------------------------- Phase 2 ---------------------------------

// Linearly traverse CFG upwards starting at n until first merge point.
// All encountered safepoints are recorded in safepoints list.
static void linear_traversal(Node* n, Node_Stack& worklist, VectorSet& visited, Node_List& safepoints) {
  for (Node* ctrl = n; ctrl != nullptr; ctrl = ctrl->in(0)) {
    assert(ctrl->is_CFG(), "");
    if (visited.test_set(ctrl->_idx)) {
      return;
    } else {
      if (ctrl->is_Region()) {
        worklist.push(ctrl, 1);
        return; // stop at merge points
      } else if (is_significant_sfpt(ctrl)) {
        safepoints.push(ctrl);
      }
    }
  }
}

// Enumerate all safepoints which are reachable from the RF to its referent through CFG.
// Start at rf node and traverse CFG upwards until referent's control node is reached.
static void enumerate_interfering_sfpts(Node* rf, PhaseIdealLoop* phase, Node_List& safepoints) {
  Node* referent = rf->in(1);
  Node* referent_ctrl = phase->get_ctrl(referent);
  assert(phase->is_dominator(referent_ctrl, rf), "sanity");

  VectorSet visited;
  visited.set(referent_ctrl->_idx); // end point

  Node_Stack stack(0);
  linear_traversal(rf, stack, visited, safepoints); // start point
  while (stack.is_nonempty()) {
    Node* cur = stack.node();
    uint  idx = stack.index();

    assert(cur != nullptr, "");
    assert(cur->is_Region(), "%s", NodeClassNames[cur->Opcode()]);
    assert(phase->is_dominator(referent_ctrl, cur), "");
    assert(idx > 0 && idx <= cur->req(), "%d %d", idx, cur->req());

    if (idx < cur->req()) {
      stack.set_index(idx + 1);
      linear_traversal(cur->in(idx), stack, visited, safepoints);
    } else {
      stack.pop();
    }
  }
}

// Phase 2: migrate reachability info to safepoints.
// All RFs are replaced with edges from corresponding referents to interfering safepoints.
// Interfering safepoints are safepoint nodes which are reachable from the RF to its referent through CFG.
bool PhaseIdealLoop::eliminate_reachability_fences() {
  Compile::TracePhase tp(_t_reachability);

  if (!OptimizeReachabilityFence) {
    return false;
  }

  Unique_Node_List redundant_rfs;
  Node_List worklist;
  for (int i = 0; i < C->reachability_fences_count(); i++) {
    ReachabilityFenceNode* rf = C->reachability_fence(i)->as_ReachabilityFence();
    assert(!is_redundant_rf(rf, true /*cfg_only*/), "missed");
    if (!is_redundant_rf(rf, false /*cfg_only*/)) {
      Node_List safepoints;
      enumerate_interfering_sfpts(rf, this, safepoints);

      Node* referent = rf->in(1);
      while (safepoints.size() > 0) {
        Node* sfpt = safepoints.pop();
        assert(is_dominator(get_ctrl(referent), sfpt), "");
        if (sfpt->find_edge(referent) == -1) {
          worklist.push(sfpt);
          worklist.push(referent);
        }
      }
    }
    redundant_rfs.push(rf);
  }

  while (worklist.size() > 0) {
    Node* referent = worklist.pop();
    Node* sfpt     = worklist.pop();
    sfpt->add_req(referent);
    igvn()._worklist.push(sfpt);
  }

  bool progress = false;
  if (redundant_rfs.size() > 0) {
    while (redundant_rfs.size() > 0) {
      Node* rf = redundant_rfs.pop();
      progress |= remove_reachability_fence(rf, this);
    }
  }

  assert(C->reachability_fences_count() == 0, "");
  return progress;
}

//======================================================================
//---------------------------- Phase 3 ---------------------------------

static int nof_extra_inputs(SafePointNode* sfpt) {
  if (sfpt->is_Call()) {
    address entry = sfpt->as_Call()->entry_point();
    if (entry == OptoRuntime::new_array_Java() ||
        entry == OptoRuntime::new_array_nozero_Java()) {
      return 1; // valid_length_test_input
    }
  }
  return 0; // no extra edges
}

// Find a point in CFG right after sfpt to insert a reachability fence.
static Node* sfpt_ctrl_out(Node* sfpt) {
  if (sfpt->is_Call()) {
    CallProjections callprojs;
    sfpt->as_Call()->extract_projections(&callprojs, false /*separate_io_proj*/, false /*do_asserts*/);
    if (callprojs.fallthrough_catchproj != nullptr) {
      return callprojs.fallthrough_catchproj;
    } else if (callprojs.catchall_catchproj != nullptr) {
      return callprojs.catchall_catchproj; // rethrow stub // TODO: safe to ignore?
    } else if (callprojs.fallthrough_proj != nullptr) {
      return callprojs.fallthrough_proj; // no exceptions thrown
    } else {
      ShouldNotReachHere();
    }
  } else if (sfpt->unique_ctrl_out()->is_OuterStripMinedLoopEnd()) {
    return sfpt->unique_ctrl_out()->as_OuterStripMinedLoopEnd()->proj_out_or_null(0 /*false*/); // outer_loop_exit()
  } else {
    return sfpt;
  }
}

// Phase 3: expand reachability fences from safepoint info.
// Turn extra safepoint edges into reachability fences immediately following the safepoint.
void Compile::expand_reachability_fences(Unique_Node_List& safepoints) {
  for (uint i = 0; i < safepoints.size(); i++) {
    SafePointNode* sfpt = safepoints.at(i)->as_SafePoint();

    int off = nof_extra_inputs(sfpt);
    if (sfpt->jvms() != nullptr && sfpt->req() > (sfpt->jvms()->oopoff() + off)) {
      assert(is_significant_sfpt(sfpt), "");
      Node* ctrl_out = sfpt_ctrl_out(sfpt);
      Node* ctrl_end = ctrl_out->unique_ctrl_out();

      while (sfpt->req() > sfpt->jvms()->oopoff() + off) {
        int idx = sfpt->req() - 1;
        Node* referent = sfpt->in(idx);
        sfpt->del_req(idx);

        Node* new_rf = new ReachabilityFenceNode(C, ctrl_out, referent);
        Node* new_rf_proj = new ProjNode(new_rf, TypeFunc::Control);

        ctrl_end->replace_edge(ctrl_out, new_rf_proj);
        ctrl_end = new_rf;
      }
    }
  }
}
