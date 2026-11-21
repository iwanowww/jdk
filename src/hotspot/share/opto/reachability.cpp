/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "opto/addnode.hpp"
#include "opto/c2_MacroAssembler.hpp"
#include "opto/callnode.hpp"
#include "opto/compile.hpp"
#include "opto/loopnode.hpp"
#include "opto/movenode.hpp"
#include "opto/phaseX.hpp"
#include "opto/reachability.hpp"
#include "opto/regalloc.hpp"
#include "opto/runtime.hpp"
#include "opto/valuetypenode.hpp"
#include "utilities/pair.hpp"

/*
 * java.lang.ref.Reference::reachabilityFence support.
 *
 * Reachability Fence (RF) ensures that the given object (referent) remains strongly reachable
 * regardless of any optimizing transformations the virtual machine may perform that might otherwise
 * allow the object to become unreachable.
 *
 * RFs are intended to be used in performance-critical code, so the primary goal for C2 support is
 * to reduce their runtime overhead as much as possible.
 *
 * Reference::reachabilityFence() calls are intrinsified into ReachabilityFence CFG nodes. RF node keeps
 * its referent alive, so the referent's location is recorded at every safepoint (in its oop map) which
 * interferes with referent's live range.
 *
 * It is tempting to directly attach referents to interfering safepoints right from the beginning, but it
 * doesn't play well with some optimizations C2 does (e.g., during loop-invariant code motion a safepoint
 * can become interfering once a load is hoisted).
 *
 * Instead, reachability representation transitions through multiple phases:
 *   (0) initial set of RFs is materialized during parsing (as a result of
 *       Reference.reachabilityFence intrinsification);
 *   (1) optimization pass during loop opts eliminates redundant RF nodes and
 *       moves the ones with loop-invariant referents outside (after) loops;
 *   (2) after loop opts are over, RF nodes are eliminated and their referents are transferred to
 *       safepoint nodes (appended as edges after debug info);
 *   (3) during final graph reshaping, referent edges are removed from safepoints and materialized as RF nodes
 *       attached to their safepoint node (closely following it in CFG graph).
 *
 * Some implementation considerations.
 *
 * (a) It looks attractive to get rid of RF nodes early and transfer to safepoint-attached representation,
 * but it is not correct until loop opts are done.
 *
 * Live ranges of values are routinely extended during loop opts. And it can break the invariant that
 * all interfering safepoints contain the referent in their oop map. (If an interfering safepoint doesn't
 * keep the referent alive, then it becomes possible for the referent to be prematurely GCed.)
 *
 * compiler/c2/TestReachabilityFence.java demonstrates a situation where a load is hoisted out of a loop thus
 * extending the live range of the value it produces beyond the safepoint on loop-back edge.
 *
 * After loop opts are over, it becomes possible to reliably enumerate all interfering safepoints and
 * to ensure that the referent is present in their oop maps. Current assumption is that after loop opts the IR graph
 * is stable enough, so relative order of memory operations and safepoints is preserved and only safepoints between
 * a referent and it's uses are taken into account. A more conservative analysis can be employed -- any safepoint dominated
 * by a referent is treated as interfering with it -- if it turns out that the assumption doesn't hold.
 *
 * (b) RF nodes may interfere with Register Allocator (RA). If a safepoint is pruned during macro expansion,
 * it can make some RF nodes redundant, but we don't have information about their relations anymore to detect that.
 * Redundant RF node unnecessarily extends referent's live range and increases register pressure.
 *
 * Hence, we eliminate RF nodes and transfer their referents to corresponding safepoints (phase #2).
 * When safepoints are pruned, corresponding reachability edges also go away.
 *
 * (c) Unfortunately, it's not straightforward to stay with safepoint-attached representation till the very end,
 * because information about derived oops is attached to safepoints in a similar way. So, for now RFs are
 * rematerialized at safepoints before RA (phase #3).
 */

bool ReachabilityFenceNode::is_redundant(PhaseGVN& gvn) {
  const Type* referent_t = gvn.type(referent());
  if (referent_t == TypePtr::NULL_PTR) {
    return true; // no-op fence: null referent
  }
  for (Node* ancestor = in(0); ancestor->is_ReachabilityFence(); ancestor = ancestor->in(0)) {
    if (ancestor->as_ReachabilityFence()->referent() == referent()) {
      return true; // back-to-back reachability fences
    }
  }
  if (!OptimizeReachabilityFences) {
    return false; // keep reachability fence nodes intact
  }
  if (!PreserveReachabilityFencesOnConstants && referent_t->singleton()) {
    return true; // no-op fence: constants are strongly reachable
  }
  return false;
}

Node* ReachabilityFenceNode::Ideal(PhaseGVN* phase, bool can_reshape) {
  return (remove_dead_region(phase, can_reshape) ? this : nullptr);
}

Node* ReachabilityFenceNode::Identity(PhaseGVN* phase) {
  if (is_redundant(*phase)) {
    return in(0);
  }
  return this;
}

// Turn the RF node into a no-op by setting its referent to null.
// Subsequent IGVN pass removes cleared nodes.
bool ReachabilityFenceNode::clear_referent(PhaseIterGVN& phase) {
  if (phase.type(referent()) == TypePtr::NULL_PTR) {
    return false;
  } else {
    phase.replace_input_of(this, Referent, phase.makecon(TypePtr::NULL_PTR));
    return true;
  }
}

// TODO
bool ReachabilityFenceNode::clear_memory(PhaseIterGVN& phase) {
  if (in(Memory)->is_top()) {
    return false;
  } else {
    phase.replace_input_of(this, Memory, phase.C->top());
    return true;
  }
}

#ifndef PRODUCT
static void rf_desc(outputStream* st, const ReachabilityFenceNode* rf, PhaseRegAlloc* ra) {
  char buf[50];
  ra->dump_register(rf->referent(), buf, sizeof(buf));
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
// Some SafePoint nodes can't affect referent's reachability in any noticeable way and
// can be safely ignored during the analysis.
static bool is_interfering_sfpt_candidate(SafePointNode* sfpt) {
  if (sfpt->jvms() == nullptr) {
    return false; // not a real safepoint
  } else if (sfpt->is_CallStaticJava() && sfpt->as_CallStaticJava()->is_uncommon_trap()) {
    return false; // uncommon traps are exit points
  }
  return true; // a full-blown safepoint can interfere with a reachability fence and its referent
}

void PhaseIdealLoop::insert_rf(Node* ctrl, Node* referent) {
  IdealLoopTree* lpt = get_loop(ctrl);
  Node* ctrl_end = ctrl->unique_ctrl_out();

  auto new_rf = new ReachabilityFenceNode(C, ctrl, C->top(), referent);

  register_control(new_rf, lpt, ctrl);
  set_idom(new_rf, ctrl, dom_depth(ctrl) + 1);
  lpt->register_reachability_fence(new_rf);

  igvn().replace_edge_of(ctrl_end, ctrl, new_rf);

  // Inserting a fence can make later fences redundant.
  igvn().add_users_to_worklist(new_rf);

  if (idom(ctrl_end) == ctrl) {
    set_idom(ctrl_end, new_rf, dom_depth(new_rf) + 1);
  } else {
    assert(ctrl_end->is_Region(), "");
  }
}

void PhaseIdealLoop::replace_rf(ReachabilityFenceNode* old_node, Node* new_node) {
  IdealLoopTree* lpt = get_loop(old_node);
  lpt->_body.yank(old_node);
  assert(lpt->_reachability_fences != nullptr, "missing");
  assert(lpt->_reachability_fences->contains(old_node), "missing");
  lpt->_reachability_fences->yank(old_node);
  replace_node_and_forward_ctrl(old_node, new_node);
}

void PhaseIdealLoop::remove_rf(ReachabilityFenceNode* rf) {
  Node* rf_ctrl_in = rf->in(0);
  Node* referent = rf->referent();
  if (rf->clear_referent(igvn()) && referent->outcnt() == 0) {
    remove_dead_data_node(referent);
  }
  replace_rf(rf, rf_ctrl_in);
}

//======================================================================
//---------------------------- Phase 1 ---------------------------------
// Optimization pass over reachability fences during loop opts.
// Moves RFs with loop-invariant referents out of the loop.
bool PhaseIdealLoop::optimize_reachability_fences() {
  Compile::TracePhase tp(_t_reachability_optimize);

  assert(OptimizeReachabilityFences, "required");

  // ResourceMark rm; // NB! not safe because insert_rf may trigger _idom reallocation
  Unique_Node_List<ReachabilityFenceNode> redundant_rfs;
  typedef Pair<Node*,Node*> LoopExit;
  GrowableArray<LoopExit> worklist;

  for (int i = 0; i < C->reachability_fences_count(); i++) {
    ReachabilityFenceNode* rf = C->reachability_fence(i);
    if (rf->is_redundant(igvn())) {
      redundant_rfs.push(rf);
      continue; // skip
    }
    if (rf->in(ReachabilityFenceNode::Memory) != C->top()) {
      continue; // skip; wait until memory is not needed
    }
    // Move RFs out of counted loops when possible.
    IdealLoopTree* lpt = get_loop(rf);
    Node* referent = rf->referent();
    if (lpt->is_invariant(referent)) {
      IfFalseNode* unique_loop_exit = lpt->unique_loop_exit_proj_or_null();
      if (unique_loop_exit != nullptr) {
        // Skip over an outer strip-mined loop.
        if (!lpt->is_root()) {
          IdealLoopTree* outer_lpt = lpt->_parent;
          if (outer_lpt->head()->is_OuterStripMinedLoop()) {
            if (outer_lpt->is_invariant(referent)) {
              IfNode* outer_loop_end = outer_lpt->head()->as_OuterStripMinedLoop()->outer_loop_end();
              if (outer_loop_end != nullptr && outer_loop_end->false_proj_or_null() != nullptr) {
                unique_loop_exit = outer_loop_end->false_proj_or_null();
              }
            } else {
              // An attempt to insert an RF node inside outer strip-mined loop breaks
              // its IR invariants and manifests as assertion failures.
              assert(false, "not loop invariant in outer strip-mined loop");
              continue; // skip
            }
          }
        }

        LoopExit p(referent, unique_loop_exit);
        worklist.push(p);
        redundant_rfs.push(rf);

#ifndef PRODUCT
        if (TraceLoopOpts) {
          IdealLoopTree* loop = get_loop(unique_loop_exit->in(0));
          tty->print_cr("ReachabilityFence: N%d: %s N%d/N%d -> loop exit N%d (%s N%d/N%d)",
                        rf->_idx, lpt->head()->Name(), lpt->head()->_idx, lpt->tail()->_idx,
                        unique_loop_exit->_idx,
                        loop->head()->Name(), loop->head()->_idx, loop->tail()->_idx);
        }
#endif // !PRODUCT
      }
    }
  }

  // Populate RFs outside loops. Memory information is lost.
  while (worklist.is_nonempty()) {
    LoopExit p = worklist.pop();
    Node* referent = p.first;
    Node* ctrl_out = p.second;
    insert_rf(ctrl_out, referent);
  }

  // Eliminate redundant RFs.
  bool progress = (redundant_rfs.size() > 0);
  while (redundant_rfs.size() > 0) {
    remove_rf(redundant_rfs.pop());
  }

  return progress;
}

//======================================================================
//---------------------------- Phase 2 ---------------------------------

// Linearly traverse CFG upwards starting at ctrl_start until first merge point.
// All encountered safepoints are recorded in safepoints list, except
// the ones filtered out by is_interfering_sfpt_candidate().
static void enumerate_interfering_sfpts_linear_traversal(Node* ctrl_start, Node_Stack<RegionNode>& stack, VectorSet& visited,
                                                         Node_List<SafePointNode>& interfering_sfpts) {
  for (Node* ctrl = ctrl_start; ctrl != nullptr; ctrl = ctrl->in(0)) {
    assert(ctrl->is_CFG(), "");
    if (visited.test_set(ctrl->_idx)) {
      return;
    } else {
      if (ctrl->is_Region()) {
        stack.push(ctrl->as_Region(), 1);
        return; // stop at merge points
      } else if (ctrl->is_SafePoint() && is_interfering_sfpt_candidate(ctrl->as_SafePoint())) {
        assert(!ctrl->is_CallStaticJava() || !ctrl->as_CallStaticJava()->is_uncommon_trap(),
               "uncommon traps should not be enumerated");
        interfering_sfpts.push(ctrl->as_SafePoint());
      }
    }
  }
}

#ifdef ASSERT
static const char* dumpNode(Node* n) {
  n->dump(3);
  n->dump(-1);
  return n->Name();
}
#endif // ASSERT

static Node* normal_return_ctrl(CallNode* call) {
  CallProjections* callprojs = call->extract_projections(false /*separate_io_proj*/,
                                                         false /*do_asserts*/,
                                                         true  /*allow_handlers*/);
  if (callprojs->fallthrough_catchproj != nullptr) {
    return callprojs->fallthrough_catchproj;
  }
  assert(callprojs->catchall_catchproj == nullptr, "normal control is missing");
  return callprojs->fallthrough_proj;
}

static Node* ctrl_for_referent(Node* referent, PhaseIdealLoop* phase) {
  // get_early_ctrl doesn't handle Phis
  if (referent->is_Phi()) {
    return referent->as_Phi()->in(0)->as_Region();
  }
  Node* early_ctrl = phase->get_early_ctrl(referent);
  // Node* late_ctrl = phase->get_late_ctrl(referent, early_ctrl);
  Node* ctrl = phase->get_ctrl(referent);
  assert(phase->is_dominator(early_ctrl, ctrl), "wrong control");
  if (early_ctrl->is_Call()) {
    // Reproduce:
    //  $JAVA_SLOW -Xcomp -XX:-TieredCompilation -XX:+StressReachabilityFences -XX:+TraceReachabilityFences -XX:CompileCommand=quiet -XX:CompileCommand=compileonly,jdk.internal.module.ServicesCatalog::putServicesCatalog
    // if (referent->is_Proj() && referent->as_Proj()->in(0)->is_Call()) {
    //   // Result is not accessible on exception-throwing projection.
    // assert(phase->get_ctrl(referent) == referent->as_Proj()->in(0)->as_Call(), "");

    // How to distinguish between normal and exceptional branches?
    if (ctrl != early_ctrl) {
      // compiler/c2/vectorapi/ShortGatherIndexMapTOCTOU.java w/ -XX:+StressReachabilityFences
      return ctrl;
    }
    assert(referent->is_Proj() && referent->as_Proj()->in(0) == early_ctrl,
           "not a Call proj: %s", dumpNode(referent));
    Node* fallthrough = normal_return_ctrl(early_ctrl->as_Call());

    assert(fallthrough != nullptr, "normal control missing");
    assert(fallthrough->bottom_type() == Type::CONTROL,
           "what else? %s %d", fallthrough->Name(), fallthrough->_idx);
    return fallthrough;
  }

  return early_ctrl;
}

// Enumerate all safepoints which are reachable between ctrl_start and ctrl_end in CFG.
// Start at ctrl_start node and traverse CFG upwards until ctrl_end is reached.
static void enumerate_interfering_sfpts(Node* ctrl_start,
                                        Node* ctrl_end,
                                        PhaseIdealLoop* phase,
                                        Node_Stack<RegionNode>& stack, VectorSet& visited,
                                        Node_List<SafePointNode>& interfering_sfpts) {
  assert(ctrl_start->is_CFG(), "not a CFG node");
  assert(ctrl_end->is_CFG(), "not a CFG node");
  assert(stack.is_empty(), "required");
  assert(visited.is_empty(), "required");

  visited.set(ctrl_end->_idx); // end point
  enumerate_interfering_sfpts_linear_traversal(ctrl_start, stack, visited, interfering_sfpts); // starting point in CFG
  while (stack.is_nonempty()) {
    RegionNode* cur = stack.node();
    uint idx = stack.index();

    assert(cur != nullptr, "");
    assert(phase->is_dominator(ctrl_end, cur), "");
    assert(idx > 0 && idx <= cur->req(), "%d %d", idx, cur->req());

    if (idx < cur->req()) {
      stack.set_index(idx + 1);
      enumerate_interfering_sfpts_linear_traversal(cur->in(idx), stack, visited, interfering_sfpts);
    } else {
      stack.pop();
    }
  }
  // Reset temporary structures to their initial state.
  assert(stack.is_empty(), "required");
  visited.clear();
}

// Start offset for reachability info on a safepoint node.
static uint rf_base_offset(SafePointNode* sfpt) {
  return sfpt->jvms()->debug_end();
}

static bool dominates_another_rf(ReachabilityFenceNode* rf, PhaseIdealLoop* phase) {
  assert(!rf->is_redundant(phase->igvn()), "");

  for (int i = 0; i < phase->C->reachability_fences_count(); i++) {
    ReachabilityFenceNode* other_rf = phase->C->reachability_fence(i);
    assert(other_rf->outcnt() > 0, "dead node");
    if (rf != other_rf && rf->referent()->eqv_uncast(other_rf->referent()) &&
        phase->is_dominator(rf, other_rf)) {
      return true; // dominates another reachability fence with the same referent
    }
  }
  return false;
}

class ReachabilityEdge : public ResourceObj {
public:
  enum Type { invalid, edgeType, fenceType };

  Type   _type;
  Node* _ctrl;
  Node* _referent;

  ReachabilityEdge() : _type(invalid), _ctrl(nullptr), _referent(nullptr) {}
  ReachabilityEdge(Type type, Node* ctrl, Node* referent)
  : _type(type), _ctrl(ctrl), _referent(referent) {}

  static ReachabilityEdge edge(Node* referent, SafePointNode* sfpt) {
    assert(!referent->is_CFG(), "not a data node");
    return ReachabilityEdge(edgeType, sfpt, referent);
  }

  static ReachabilityEdge fence(Node* referent, Node* ctrl) {
    assert(!referent->is_CFG(), "not a data node");
    assert(!referent->is_AddP(), "");
    assert(ctrl->is_CFG(), "not a control node");
    return ReachabilityEdge(fenceType, ctrl, referent);
  }

  static ReachabilityEdge fence(ReachabilityFenceNode* rf) {
    return ReachabilityEdge(fenceType, rf, rf->referent());
  }
};

void PhaseIdealLoop::populate_reachability_edge_worklists(Node_List<ReachabilityFenceNode>& for_processing,
                                                          Node_List<ReachabilityFenceNode>& for_removal) {
  for (int i = 0; i < C->reachability_fences_count(); i++) {
    ReachabilityFenceNode* rf = C->reachability_fence(i);
    assert(!rf->is_redundant(igvn()), "missed");
    Node* referent = rf->referent();
    assert(!referent->is_ValueType(), "VT referent not supported");

    const Type* referent_t = igvn().type(referent);
    assert(referent_t != TypePtr::NULL_PTR, "redundant rf");
    bool is_constant_rf = referent_t->singleton();

    // Tells whether it is safe to remove RF node after expansion or not.
    // FIXME: describe problematic scenarios. Referent has to stay alive in presence of interfering safepoints.
    if (dominates_another_rf(rf, this)) {
      // Redundant fence: dominates another RF with the same referent.
      // RF is redundant for some referent oop when the referent has another RF which
      // keeps it alive across the RF. In terms of dominance relation it can be formulated
      // as "a referent has another RF which is dominated by the redundant RF".
      //
      // NB! It is safe to assume that dominating RF is redundant only during expansion.
      // Otherwise, there's a chance for the superseding RF node to go away before expansion.
      // Non-RF users are ignored for a similar reason: they can go away before or after expansion
      // takes place, so no guarantees reachability information is preserved.
#ifndef PRODUCT
      if (TraceReachabilityFences) {
        tty->print_cr("EXPAND: ReachabilityFence: N%d => %s N%d: redundant",
                      rf->_idx, referent->Name(), referent->_idx);
      }
#endif // !PRODUCT
      for_removal.push(rf);
    } else if (is_constant_rf) {
#ifndef PRODUCT
      if (TraceReachabilityFences) {
        tty->print_cr("EXPAND: ReachabilityFence: N%d => %s N%d: constant",
                      rf->_idx, referent->Name(), referent->_idx);
      }
#endif // !PRODUCT
      if (PreserveReachabilityFencesOnConstants) {
        // leave RFs on constants intact
      } else {
        for_removal.push(rf);
      }
    } else {
      for_processing.push(rf);
      for_removal.push(rf);
    }
  }
}

// Add reachability edge to a safepoint node as a non-debug edge following debug information.
bool PhaseIdealLoop::add_reachability_edge_to_safepoint(Node* referent, SafePointNode* sfpt) {
  assert(referent->bottom_type()->isa_oopptr() ||
         referent->bottom_type()->isa_narrowoop(),
         "not an oop: %s %d = %s", referent->Name(), referent->_idx, Type::str(referent->bottom_type()));
  assert(!referent->is_ValueType(), "reachability edge to VT is not supported");
  assert(is_dominator(ctrl_for_referent(referent, this), sfpt), "unschedulable");
  // Debug inputs must be available to reconstruct interpreter state on deoptimization,
  // so their oops are kept alive in the caller's oop map. Existing reachability edges
  // also keep the referent alive across the safepoint. Outgoing call arguments, before
  // debug_start(), are excluded from the caller's oop map (see OopFlow::build_oop_map).
  // Their reachability is the callee's responsibility, and the callee may stop keeping
  // an argument alive before returning. An argument edge alone therefore cannot replace
  // a reachability edge, which keeps the referent alive across the entire call.
  if (sfpt->find_edge_in_range(referent, sfpt->jvms()->debug_start(), sfpt->req()) == -1) {
    sfpt->add_req(referent);
    igvn()._worklist.push(sfpt);
    return true;
  }
  return false; // no new edges added
}

static bool is_proj_to_alloc(Node* n) {
  if (n->is_Proj() && n->as_Proj()->_con == TypeFunc::Parms) {
    if (n->as_Proj()->in(0)->is_Allocate()) {
      return true;
    }
    if (n->as_Proj()->in(0)->is_CallStaticJava()) {
      address entry = n->as_Proj()->in(0)->as_CallStaticJava()->entry_point();
      if (entry == OptoRuntime::new_array_Java()        ||
          entry == OptoRuntime::new_array_nozero_Java() ||
          entry == OptoRuntime::multianewarray2_Java()  ||
          entry == OptoRuntime::multianewarray3_Java()  ||
          entry == OptoRuntime::multianewarray4_Java()  ||
          entry == OptoRuntime::multianewarray5_Java()  ||
          entry == OptoRuntime::multianewarrayN_Java()) {
        return true;
      }
    }
  }
  return false;
}

#ifdef ASSERT
// A raw pointer is safe to stop at only if every live input is a newly
// allocated oop.  A raw pointer's type alone does not establish this.
static bool has_allocation_origin(Node* raw_oop) {
  Unique_Node_List<Node> worklist;
  worklist.push(raw_oop);
  bool found_allocation = false;

  for (uint i = 0; i < worklist.size(); i++) {
    Node* value = worklist.at(i);
    if (is_proj_to_alloc(value)) {
      found_allocation = true;
    } else if (value->is_ConstraintCast() && value->in(1)->bottom_type()->isa_rawptr()) {
      worklist.push(value->in(1));
    } else if (value->is_Phi()) {
      RegionNode* region = value->as_Phi()->region();
      for (uint j = 1; j < value->req(); j++) {
        Node* predecessor = region->in(j);
        Node* input = value->in(j);
        if (predecessor == nullptr || input == nullptr) {
          return false;
        }
        if (!predecessor->is_top()) {
          worklist.push(input);
        }
      }
    } else if (value->is_CMove()) {
      worklist.push(value->in(CMoveNode::IfFalse));
      worklist.push(value->in(CMoveNode::IfTrue));
    } else {
      return false;
    }
  }
  return found_allocation;
}
#endif // ASSERT

static bool move_referent_up(Node* referent,
                             Node* referent_ctrl,
                             VectorSet& visited_phis,
                             GrowableArray<ReachabilityEdge>& worklist) {
  // Try to move referent up.
  if (referent->is_ConstraintCast() && referent->in(1)->bottom_type()->isa_oopptr()) { // ignore raw oops
    worklist.push(ReachabilityEdge::fence(referent->in(1), referent_ctrl));
    return true;
  } else if (referent->is_DecodeN() || referent->is_EncodeP()) {
    worklist.push(ReachabilityEdge::fence(referent->in(1), referent_ctrl));
    return true;
  } else if (referent->is_Phi()) {
    assert(referent->as_Phi()->in(0)->as_Region() == referent_ctrl, "sanity");
    if (!visited_phis.test_set(referent->as_Phi()->_idx)) {
      // Push reachability fence through phi.
      RegionNode* r = referent->as_Phi()->in(0)->as_Region();
      for (uint k = 1; k < r->req(); k++) {
        ReachabilityEdge fence = ReachabilityEdge::fence(referent->as_Phi()->in(k), r->in(k));
        worklist.push(fence);
      }
    }
    return true;
  } else if (referent->is_CMove()) {
    CMoveNode* cmov = referent->as_CMove();

    ReachabilityEdge fence_on_false = ReachabilityEdge::fence(cmov->in(CMoveNode::IfFalse), referent_ctrl);
    worklist.push(fence_on_false);

    ReachabilityEdge fence_on_true = ReachabilityEdge::fence(cmov->in(CMoveNode::IfTrue), referent_ctrl);
    worklist.push(fence_on_true);
    return true;
  } else if ((referent->is_Load() || referent->is_LoadStore()) &&
              referent->in(MemNode::Address)->is_AddP() &&
              !referent->in(MemNode::Address)->in(AddPNode::Base)->is_top()) {
    // AddP's base is the oop to keep alive. Its address can be a Phi of
    // derived addresses, so it need not be a linear AddP chain from the base.
    AddPNode* ld_adr = referent->in(MemNode::Address)->as_AddP();
    Node* base = ld_adr->in(AddPNode::Base);
    assert(ld_adr->address_input_has_same_base(), "mismatched AddP base");
    assert(base->bottom_type()->isa_oopptr() != nullptr,
           "AddP base is not an oop: %s %d", base->Name(), base->_idx);
    ReachabilityEdge fence_on_base = ReachabilityEdge::fence(base, referent_ctrl);
    worklist.push(fence_on_base);
    return true;
  } else if (referent->is_ValueType()) {
    ValueTypeNode* referent_vt = referent->as_ValueType();
    for (uint i = 0; i < referent_vt->field_count(); i++) {
      Node* field_value = referent_vt->field_value(i);
      bool is_oop_field = field_value->bottom_type()->isa_oopptr() || field_value->bottom_type()->isa_narrowoop();
      if (is_oop_field) {
        ReachabilityEdge field_value_fence = ReachabilityEdge::fence(field_value, referent_ctrl);
        worklist.push(field_value_fence);
      }
    }
    return true;
  } else {
    assert(referent->is_Parm()              ||
           referent->is_CreateEx()          ||
           referent->Opcode() == Op_CastI2N ||
           (referent->is_ConstraintCast() && (referent->in(1)->bottom_type() == TypeRawPtr::NOTNULL)) ||
           (referent->is_Proj() && referent->as_Proj()->in(0)->is_Call())                             ||
           ((referent->is_Load() || referent->is_LoadStore()) &&
             (!referent->in(MemNode::Address)->is_AddP() ||
              referent->in(MemNode::Address)->in(AddPNode::Base)->is_top())),
           "what else could it be? %s", dumpNode(referent));
    return false;
  }
}

static bool enumerate_reachability_edges(PhaseIdealLoop* phase,
                                         Node* referent,
                                         Node* referent_ctrl,
                                         Node* fence_ctrl,
                                         bool is_initial_pass,
                                         const Node_List<SafePointNode>& sfpt_candidates,
                                         GrowableArray<ReachabilityEdge>& reachability_edges) {
  uint no_of_sfpts = sfpt_candidates.size();
  uint no_of_processed_sfpts = 0;

  for (uint k = 0; k < sfpt_candidates.size(); k++) {
    SafePointNode* sfpt = sfpt_candidates.at(k);
    if (sfpt != nullptr) { // not yet processed
      assert(sfpt->req() == rf_base_offset(sfpt), "no extra edges allowed");

      // The assumption is that on each step up the referent chain
      // domination over interfering candidates monotonically changes towards true.
      assert(phase->is_dominator(referent_ctrl, sfpt) ||
             !phase->is_dominator(fence_ctrl, sfpt), "non-monotonic");

      // TODO
      if (referent_ctrl != sfpt && phase->is_dominator(referent_ctrl, sfpt)) {
        // Has the safepoint been already processed?
        bool is_processed = !is_initial_pass && phase->is_strict_dominator(fence_ctrl, sfpt);
        if (!is_processed) {
          NOT_PRODUCT( if (TraceReachabilityFences) { tty->print("+E%d", referent->_idx); } )
          ReachabilityEdge edge = ReachabilityEdge::edge(referent, sfpt);
          reachability_edges.push(edge);
        }
        no_of_processed_sfpts++;
      }
    }
  }

  NOT_PRODUCT( if (TraceReachabilityFences) {
    tty->print_cr(" (%2d/%2d)", no_of_processed_sfpts, no_of_sfpts);
  } )

  assert(no_of_processed_sfpts <= no_of_sfpts, "sanity");
  return (no_of_processed_sfpts < no_of_sfpts); // more work to do?
}

void PhaseIdealLoop::process_reachability_fence_edge(const ReachabilityEdge& fence,
                                                     bool is_initial_pass,
                                                     const Node_List<SafePointNode>& sfpt_candidates,
                                                     VectorSet& visited_phis,
                                                     GrowableArray<ReachabilityEdge>& worklist,
                                                     GrowableArray<ReachabilityEdge>& reachability_edges) {
  assert(fence._type == ReachabilityEdge::fenceType, "not a fence");
  assert(igvn().type(fence._referent)->singleton() == false, "constant referent");

  Node* referent = fence._referent;

  Node* referent_ctrl = ctrl_for_referent(referent, this);
  if (referent_ctrl == fence._ctrl || referent->is_ValueType()) { // no reachability edges with VTs, please
    // Try to move referent up.
    Node* ctrl = (referent->is_ValueType() ? fence._ctrl : referent_ctrl);
    if (move_referent_up(referent, ctrl, visited_phis, worklist)) {
      // Populates worklist with extra work.
    } else {
#ifndef PRODUCT
      if (TraceReachabilityFences) {
        tty->print_cr("  * F %s%d <= C %s%d: ",
                      referent->Name(), referent->_idx,
                      referent_ctrl->Name(), referent_ctrl->_idx);
      }
#endif // !PRODUCT
      reachability_edges.push(fence); // leave as is
    }
  } else {
#ifndef PRODUCT
    if (TraceReachabilityFences) {
      tty->print("  * R %s%d / C %s%d",
                 referent->Name(), referent->_idx,
                 referent_ctrl->Name(), referent_ctrl->_idx);
    }
#endif // !PRODUCT
    if (enumerate_reachability_edges(this, referent, referent_ctrl, fence._ctrl,
                                     is_initial_pass, sfpt_candidates,
                                  reachability_edges)) {
      worklist.push(ReachabilityEdge::fence(referent, referent_ctrl));
    }
  }
}

void PhaseIdealLoop::process_reachability_fence(ReachabilityFenceNode* reachability_fence,
                                                GrowableArray<ReachabilityEdge>& reachability_edges) { // out-param
  ResourceMark rm;
  Node_List<SafePointNode> sfpt_worklist;
  Node_Stack<RegionNode> stack(0);
  GrowableArray<ReachabilityEdge> reachability_edges_worklist;
  VectorSet visited;

  enumerate_interfering_sfpts(reachability_fence, C->start(), this, stack, visited, sfpt_worklist);

#ifndef PRODUCT
  if (TraceReachabilityFences) {
    tty->print("EXPAND: ReachabilityFence %d => %s %d:",
               reachability_fence->_idx, reachability_fence->referent()->Name(), reachability_fence->_idx);

    for (uint k = 0; k < sfpt_worklist.size(); k++) {
      SafePointNode* sfpt = sfpt_worklist.at(k);
      tty->print(" %sN%d", sfpt->Name(), sfpt->_idx);
    }
    tty->cr();
  }
#endif // !PRODUCT

  reachability_edges_worklist.push(ReachabilityEdge::fence(reachability_fence));

  assert(visited.is_empty(), "not empty");
  while (reachability_edges_worklist.is_nonempty()) {
    ReachabilityEdge fence = reachability_edges_worklist.pop();
    assert(fence._type == ReachabilityEdge::fenceType, "not a fence");

    const Type* referent_t = igvn().type(fence._referent);
    if (referent_t->singleton()) {
      // a fence on a constant is a no-op
      if (PreserveReachabilityFencesOnConstants && referent_t != TypePtr::NULL_PTR) {
        reachability_edges.push(fence);
      }
    } else {
      bool is_initial_pass = (fence._ctrl == reachability_fence);
      process_reachability_fence_edge(fence, is_initial_pass,
                                      sfpt_worklist, visited, reachability_edges_worklist,
                                      reachability_edges);
    }
  }
}

static Node* normalize_ctrl(Compile* C, Node* ctrl, Node* referent) {
  if (ctrl->is_Initialize() || ctrl->is_MemBar()) {
    return ctrl->as_Multi()->proj_out(0);
  } else if (ctrl->is_Call() ||
             (ctrl->is_Proj() && ctrl->as_Proj()->in(0)->is_Call())) {
    CallNode* call = (ctrl->is_Call() ? ctrl->as_Call()
                                      : ctrl->as_Proj()->in(0)->as_Call());
    CallProjections* callprojs = call->extract_projections(false /*separate_io_proj*/,
                                                           false /*do_asserts*/,
                                                           true  /*allow_handlers*/);
    if (callprojs->fallthrough_catchproj != nullptr) {
      C->assert_alive_node(callprojs->fallthrough_catchproj);
      return callprojs->fallthrough_catchproj;
    } else if (callprojs->catchall_catchproj != nullptr) {
      C->assert_alive_node(callprojs->catchall_catchproj);
      return callprojs->catchall_catchproj; // rethrow stub
    } else if (callprojs->fallthrough_proj != nullptr) {
      return callprojs->fallthrough_proj; // no exceptions thrown
    } else {
      ShouldNotReachHere();
    }
  } else {
    return ctrl;
  }
}

void PhaseIdealLoop::add_reachability_fence(Node* referent, Node* ctrl) {
  assert(ctrl->is_Start() == referent->is_Parm(),
         "wrong ctrl to insert a fence: %s %d", ctrl->Name(), ctrl->_idx);

  if (referent->is_ConstraintCast() && (is_proj_to_alloc(referent->in(1)) ||
                                        referent->in(1)->bottom_type() == TypeRawPtr::NOTNULL)) {
    // No need to climb beyond an allocation site.
    // NB! In some pathological cases, an allocation may be hidden behind multiple layers of Phis.
    assert(has_allocation_origin(referent->in(1)),
           "raw oop cast without an allocation origin: %s %d", referent->Name(), referent->_idx);
  } else if (referent->is_Parm() && ctrl->is_Start()) {
    // No special care is needed for arguments.
  } else if (referent->is_Proj() &&
              referent->as_Proj()->in(0)->is_Call() &&
              !referent->as_Proj()->in(0)->is_CallLeafPure()) {
    // Calls should stay unless dominating part of CFG dies.
    // The only exception is primitive boxing/unboxing calls: unused ones are pruned
    // during macro expansion. But there's no point in keeping them alive solely for
    // reachability fence purposes.
  } else if (referent->is_CreateEx()) {
    // An exception (either new or pre-existing) originated from a call.
    // It's kept alive inside the call and there are reachability edges added
    // if there are any interfering safepoints. So, safe to drop the fence.
  } else {
    assert(referent->bottom_type()->isa_oopptr() ||
           referent->bottom_type()->isa_narrowoop(),
           "not an oop? %s %s %d", Type::str(referent->bottom_type()), referent->Name(), referent->_idx);
    assert(referent->Opcode() == Op_CastI2N ||
           (referent->is_Con() && PreserveReachabilityFencesOnConstants) ||
           //(referent->is_Proj() && referent->as_Proj()->in(0)->is_CallLeafPure()) || // revisit; should be safe to ignore
           ((referent->is_Load() || referent->is_LoadStore()) &&
             (!referent->in(MemNode::Address)->is_AddP() ||
              referent->in(MemNode::Address)->in(AddPNode::Base)->is_top())),
           "what else could it be? %s %d", referent->Name(), referent->_idx);
    // Stop here and leave a reachability fence behind.
#ifndef PRODUCT
    if (TraceReachabilityFences) {
      tty->print_cr("  * F %s%d <= C %s%d: ",
                 referent->Name(), referent->_idx,
                 ctrl->Name(), ctrl->_idx);
    }
#endif // !PRODUCT
    Node* normalized_ctrl = normalize_ctrl(C, ctrl, referent);
    assert(is_dominator(ctrl, normalized_ctrl),
           "normalized control is outside the referent's CFG path");
    assert(normalized_ctrl->unique_ctrl_out_or_null() != nullptr,
           "reachability fence insertion requires one CFG successor: %s %d for %s %d",
           normalized_ctrl->Name(), normalized_ctrl->_idx, referent->Name(), referent->_idx);
    insert_rf(normalized_ctrl, referent);
  }
}

// Phase 2: migrate reachability info to safepoints.
// All RFs are replaced with edges from corresponding referents to interfering safepoints.
// Interfering safepoints are safepoint nodes which are reachable from the RF to its referent through CFG.
bool PhaseIdealLoop::expand_reachability_fences() {
  Compile::TracePhase tp(_t_reachability_expand);

  assert(OptimizeReachabilityFences, "required");
  assert(C->post_loop_opts_phase(), "required");
  DEBUG_ONLY( int no_of_preserved_rfs = 0; )

  // Used to communicate between processing and bulk graph modification steps.
  // Allocated in compiler arena, so reachability fence processing can access them
  // while operating under a local resource mark.
  Node_List<ReachabilityFenceNode> for_removal(C->comp_arena(), C->reachability_fences_count());
  GrowableArray<ReachabilityEdge> reachability_edges(C->comp_arena(), C->reachability_fences_count(), 0, ReachabilityEdge());

  { // NB! No modifications causing DOM tree changes are allowed.
    ResourceMark rm;
    Node_List<ReachabilityFenceNode> for_processing;

    populate_reachability_edge_worklists(for_processing, for_removal);
    DEBUG_ONLY( no_of_preserved_rfs = C->reachability_fences_count() - for_removal.size(); )

    for (uint i = 0; i < for_processing.size(); i++) {
      ReachabilityFenceNode* rf = for_processing.at(i);
      process_reachability_fence(rf, reachability_edges);
    }
  }
  // Materialize reachability edges.
  // NB! Reachability fence insertion modifies DOM tree.
  while (reachability_edges.length() > 0) {
    ReachabilityEdge p = reachability_edges.pop();
    if (p._type == ReachabilityEdge::edgeType) {
      add_reachability_edge_to_safepoint(p._referent, p._ctrl->as_SafePoint());
    } else if (p._type == ReachabilityEdge::fenceType) {
      add_reachability_fence(p._referent, p._ctrl);
    } else {
      ShouldNotReachHere();
    }
  }

  // Eliminate processed RFs. They become redundant once reachability edges are added.
  bool progress = (for_removal.size() > 0);
  while (for_removal.size() > 0) {
    remove_rf(for_removal.pop());
  }

  return progress;
}

//======================================================================
//---------------------------- Phase 3 ---------------------------------

// Find a point in CFG right after safepoint node to insert reachability fence.
static Node* sfpt_ctrl_out(SafePointNode* sfpt) {
  if (sfpt->is_Call()) {
    CallProjections* callprojs = sfpt->as_Call()->extract_projections(false /*separate_io_proj*/,
                                                                      false /*do_asserts*/,
                                                                      true  /*allow_handlers*/);
    // Calls can have multiple control projections. However, reachability edge expansion
    // happens during final graph reshaping which is performed very late in compilation pipeline.
    // The assumption here is that the control path chosen for insertion can't go away.
    // So, materializing a reachability fence on a single control path produced by a call
    // is enough to keep the referent oop alive across the call.
    if (callprojs->fallthrough_catchproj != nullptr) {
      return callprojs->fallthrough_catchproj;
    } else if (callprojs->catchall_catchproj != nullptr) {
      return callprojs->catchall_catchproj; // rethrow stub
    } else if (callprojs->fallthrough_proj != nullptr) {
      return callprojs->fallthrough_proj; // no exceptions thrown
    } else {
      ShouldNotReachHere();
    }
  } else {
    return sfpt;
  }
}

// Phase 3: materialize reachability fences from reachability edges on safepoints.
// Turn extra safepoint edges into reachability fences immediately following the safepoint.
//
// NB! As of now, a special care is needed to properly enumerate reachability edges because
// there are other use cases for non-debug safepoint edges. expand_reachability_edges() runs
// after macro expansion where runtime calls during array allocation are annotated with
// valid_length_test_input as an extra edges. Until there's a mechanism to distinguish between
// different types of non-debug edges, unrelated cases are filtered out explicitly and in ad-hoc manner.
void Compile::expand_reachability_edges(Unique_Node_List<SafePointNode>& safepoints) {
  for (uint i = 0; i < safepoints.size(); i++) {
    SafePointNode* sfpt = safepoints.at(i);

    uint rf_offset = rf_base_offset(sfpt);
    if (sfpt->jvms() != nullptr && sfpt->req() > rf_offset) {
      assert(is_interfering_sfpt_candidate(sfpt), "");
      // Find a point in CFG right after safepoint to insert reachability fence.
      Node* ctrl_out = sfpt_ctrl_out(sfpt);
      Node* ctrl_end = ctrl_out->unique_ctrl_out();

      Node* extra_edge = nullptr;
      if (sfpt->is_Call()) {
        address entry = sfpt->as_Call()->entry_point();
        if (entry == OptoRuntime::new_array_Java() ||
            entry == OptoRuntime::new_array_nozero_Java()) {
          // valid_length_test_input is appended during macro expansion at the very end
          int last_idx = sfpt->req() - 1;
          extra_edge = sfpt->in(last_idx);
          sfpt->del_req(last_idx);
        }
      }

      while (sfpt->req() > rf_offset) {
        int idx = sfpt->req() - 1;
        Node* referent = sfpt->in(idx);
        sfpt->del_req(idx);

        Node* new_rf = new ReachabilityFenceNode(C, ctrl_out, C->top(), referent);
        ctrl_end->replace_edge(ctrl_out, new_rf);
        ctrl_end = new_rf;
      }

      if (extra_edge != nullptr) {
        sfpt->add_req(extra_edge); // Add valid_length_test_input edge back
      }
    }
  }
}
