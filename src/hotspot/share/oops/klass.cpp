/*
 * Copyright (c) 1997, 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "cds/archiveHeapLoader.hpp"
#include "cds/heapShared.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/classLoaderDataGraph.inline.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "jvm_io.h"
#include "logging/log.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oopHandle.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/synchronizer.hpp"
#include "utilities/macros.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/stack.inline.hpp"

void Klass::set_java_mirror(Handle m) {
  assert(!m.is_null(), "New mirror should never be null.");
  assert(_java_mirror.is_empty(), "should only be used to initialize mirror");
  _java_mirror = class_loader_data()->add_handle(m);
}

oop Klass::java_mirror_no_keepalive() const {
  return _java_mirror.peek();
}

bool Klass::is_cloneable() const {
  return _access_flags.is_cloneable_fast() ||
         is_subtype_of(vmClasses::Cloneable_klass());
}

void Klass::set_is_cloneable() {
  if (name() == vmSymbols::java_lang_invoke_MemberName()) {
    assert(is_final(), "no subclasses allowed");
    // MemberName cloning should not be intrinsified and always happen in JVM_Clone.
  } else if (is_instance_klass() && InstanceKlass::cast(this)->reference_type() != REF_NONE) {
    // Reference cloning should not be intrinsified and always happen in JVM_Clone.
  } else {
    _access_flags.set_is_cloneable_fast();
  }
}

void Klass::set_name(Symbol* n) {
  _name = n;
  if (_name != nullptr) _name->increment_refcount();

  if (Arguments::is_dumping_archive() && is_instance_klass()) {
    SystemDictionaryShared::init_dumptime_info(InstanceKlass::cast(this));
  }
}

bool Klass::is_subclass_of(const Klass* k) const {
  // Run up the super chain and check
  if (this == k) return true;

  Klass* t = const_cast<Klass*>(this)->super();

  while (t != nullptr) {
    if (t == k) return true;
    t = t->super();
  }
  return false;
}

void Klass::release_C_heap_structures(bool release_constant_pool) {
  if (_name != nullptr) _name->decrement_refcount();
}

bool Klass::search_secondary_supers(Klass* k) const {
  // Put some extra logic here out-of-line, before the search proper.
  // This cuts down the size of the inline method.

  // This is necessary, since I am never in my own secondary_super list.
  if (this == k) {
    return true;
  }
  if (UseSecondarySupersTable) {
    bool r = search_secondary_supers_table(k);
    if (VerifySecondarySupers) {
      guarantee(r == search_secondary_supers_linear(k), "mismatch");
    }
    return r;
  } else {

    return search_secondary_supers_linear(k);
  }
}

bool Klass::search_secondary_supers_linear(Klass* k) const {
  // Scan the array-of-objects for a match
  int cnt = secondary_supers()->length();
  for (int i = 0; i < cnt; i++) {
    if (secondary_supers()->at(i) == k) {
      return true;
    }
  }
  return false;
}

bool Klass::search_secondary_supers_table(Klass* k) const {
  assert(UseSecondarySupersTable, "");

  Array<Klass*>* ss_table = secondary_supers();
  uint table_size = secondary_supers_table_size();
  if (table_size > 0) {
    bool is_power_of_2_sizes_only = (SecondarySupersTableSizingMode & 1) == 0;
    assert(is_power_of_2(table_size) || !is_power_of_2_sizes_only, "");

    uintptr_t seed = secondary_supers_seed();
    uint primary_idx = k->index(seed, table_size);
    Klass* probe = ss_table->at(primary_idx);
    if (probe == k) {
      return true; // match
    } else if (probe == nullptr) {
      return false;
    } else {
      // Need to check the tail.
    }
  }
  return ss_table->contains(k, table_size); // scan the tail for a match
}

// Return self, except for abstract classes with exactly 1
// implementor.  Then return the 1 concrete implementation.
Klass *Klass::up_cast_abstract() {
  Klass *r = this;
  while( r->is_abstract() ) {   // Receiver is abstract?
    Klass *s = r->subklass();   // Check for exactly 1 subklass
    if (s == nullptr || s->next_sibling() != nullptr) // Oops; wrong count; give up
      return this;              // Return 'this' as a no-progress flag
    r = s;                    // Loop till find concrete class
  }
  return r;                   // Return the 1 concrete class
}

// Find LCA in class hierarchy
Klass *Klass::LCA( Klass *k2 ) {
  Klass *k1 = this;
  while( 1 ) {
    if( k1->is_subtype_of(k2) ) return k2;
    if( k2->is_subtype_of(k1) ) return k1;
    k1 = k1->super();
    k2 = k2->super();
  }
}


void Klass::check_valid_for_instantiation(bool throwError, TRAPS) {
  ResourceMark rm(THREAD);
  THROW_MSG(throwError ? vmSymbols::java_lang_InstantiationError()
            : vmSymbols::java_lang_InstantiationException(), external_name());
}


void Klass::copy_array(arrayOop s, int src_pos, arrayOop d, int dst_pos, int length, TRAPS) {
  ResourceMark rm(THREAD);
  assert(s != nullptr, "Throw NPE!");
  THROW_MSG(vmSymbols::java_lang_ArrayStoreException(),
            err_msg("arraycopy: source type %s is not an array", s->klass()->external_name()));
}


void Klass::initialize(TRAPS) {
  ShouldNotReachHere();
}

Klass* Klass::find_field(Symbol* name, Symbol* sig, fieldDescriptor* fd) const {
#ifdef ASSERT
  tty->print_cr("Error: find_field called on a klass oop."
                " Likely error: reflection method does not correctly"
                " wrap return value in a mirror object.");
#endif
  ShouldNotReachHere();
  return nullptr;
}

Method* Klass::uncached_lookup_method(const Symbol* name, const Symbol* signature,
                                      OverpassLookupMode overpass_mode,
                                      PrivateLookupMode private_mode) const {
#ifdef ASSERT
  tty->print_cr("Error: uncached_lookup_method called on a klass oop."
                " Likely error: reflection method does not correctly"
                " wrap return value in a mirror object.");
#endif
  ShouldNotReachHere();
  return nullptr;
}

void* Klass::operator new(size_t size, ClassLoaderData* loader_data, size_t word_size, TRAPS) throw() {
  return Metaspace::allocate(loader_data, word_size, MetaspaceObj::ClassType, THREAD);
}

// "Normal" instantiation is preceded by a MetaspaceObj allocation
// which zeros out memory - calloc equivalent.
// The constructor is also used from CppVtableCloner,
// which doesn't zero out the memory before calling the constructor.
Klass::Klass(KlassKind kind) : _kind(kind),
                           _shared_class_path_index(-1) {
  CDS_ONLY(_shared_class_flags = 0;)
  CDS_JAVA_HEAP_ONLY(_archived_mirror_index = -1;)
  _primary_supers[0] = this;
  set_super_check_offset(in_bytes(primary_supers_offset()));
}

jint Klass::array_layout_helper(BasicType etype) {
  assert(etype >= T_BOOLEAN && etype <= T_OBJECT, "valid etype");
  // Note that T_ARRAY is not allowed here.
  int  hsize = arrayOopDesc::base_offset_in_bytes(etype);
  int  esize = type2aelembytes(etype);
  bool isobj = (etype == T_OBJECT);
  int  tag   =  isobj ? _lh_array_tag_obj_value : _lh_array_tag_type_value;
  int lh = array_layout_helper(tag, hsize, etype, exact_log2(esize));

  assert(lh < (int)_lh_neutral_value, "must look like an array layout");
  assert(layout_helper_is_array(lh), "correct kind");
  assert(layout_helper_is_objArray(lh) == isobj, "correct kind");
  assert(layout_helper_is_typeArray(lh) == !isobj, "correct kind");
  assert(layout_helper_header_size(lh) == hsize, "correct decode");
  assert(layout_helper_element_type(lh) == etype, "correct decode");
  assert(1 << layout_helper_log2_element_size(lh) == esize, "correct decode");

  return lh;
}

bool Klass::can_be_primary_super_slow() const {
  if (super() == nullptr)
    return true;
  else if (super()->super_depth() >= primary_super_limit()-1)
    return false;
  else
    return true;
}

static void fullmul64(uint64_t& hi, uint64_t& lo, uint64_t op1, uint64_t op2) {
  __int128 x128 = op1, y128 = op2, xy128 = x128 * y128;
  hi = (uint64_t)(xy128 >> 64);
  lo = (uint64_t)(xy128 >>  0);
}

static void fullmul32(uint32_t& hi, uint32_t& lo, uint32_t op1, uint32_t op2) {
  uint64_t x64 = op1, y64 = op2, xy64 = x64 * y64;
  hi = (uint32_t)(xy64 >> 32);
  lo = (uint32_t)(xy64 >>  0);
}

static uint64_t ror(uint64_t x, uint64_t distance) {
  distance = distance & 0x3F;
  return (x >> distance) | (x << (64 - distance));
}

static uint64_t get_hash64(uint64_t x, uint64_t y) {
  const uint64_t M  = 0x8ADAE89C337954D5;
  const uint64_t A  = 0xAAAAAAAAAAAAAAAA; // REPAA
  const uint64_t H0 = (x ^ y), L0 = (x ^ A);

  uint64_t U0, V0; fullmul64(U0, V0, L0, M);
  const uint64_t Q0 = (H0 * M);
  const uint64_t L1 = (Q0 ^ U0);

  uint64_t U1, V1; fullmul64(U1, V1, L1, M);
  const uint64_t P1 = (V0 ^ M);
  const uint64_t Q1 = ror(P1, L1);
  const uint64_t L2 = (Q1 ^ U1);
  return V1 ^ L2;
}

static uint32_t get_hash32(uint32_t x, uint32_t y) {
  const uint32_t M  = 0x337954D5;
  const uint32_t A  = 0xAAAAAAAA; // REPAA
  const uint32_t H0 = (x ^ y), L0 = (x ^ A);

  uint32_t U0, V0; fullmul32(U0, V0, L0, M);
  const uint32_t Q0 = (H0 * M);
  const uint32_t L1 = (Q0 ^ U0);

  uint32_t U1, V1; fullmul32(U1, V1, L1, M);
  const uint32_t P1 = (V0 ^ M);
  const uint32_t Q1 = ror(P1, L1);
  const uint32_t L2 = (Q1 ^ U1);
  return V1 ^ L2;
}

static inline uint64_t get_next_hash(Thread* current) {
  uint64_t seed = current->_seed;
  uint64_t value = get_hash64(seed, 0xAAAAAAAA) + 1; // TODO: introduce t
  current->_seed = value;
  return value;
}

void Klass::initialize_supers(Klass* k, Array<InstanceKlass*>* transitive_interfaces, TRAPS) {
  if (k == nullptr) {
    set_super(nullptr);
    _primary_supers[0] = this;
    assert(super_depth() == 0, "Object must already be initialized properly");
  } else if (k != super() || k == vmClasses::Object_klass()) {
    assert(super() == nullptr || super() == vmClasses::Object_klass(),
           "initialize this only once to a non-trivial value");
    set_super(k);
    Klass* sup = k;
    int sup_depth = sup->super_depth();
    juint my_depth  = MIN2(sup_depth + 1, (int)primary_super_limit());
    if (!can_be_primary_super_slow())
      my_depth = primary_super_limit();
    for (juint i = 0; i < my_depth; i++) {
      _primary_supers[i] = sup->_primary_supers[i];
    }
    Klass* *super_check_cell;
    if (my_depth < primary_super_limit()) {
      _primary_supers[my_depth] = this;
      super_check_cell = &_primary_supers[my_depth];
    } else {
      // Overflow of the primary_supers array forces me to be secondary.
      super_check_cell = &_secondary_super_cache;
    }
    set_super_check_offset((address)super_check_cell - (address) this);

#ifdef ASSERT
    {
      juint j = super_depth();
      assert(j == my_depth, "computed accessor gets right answer");
      Klass* t = this;
      while (!t->can_be_primary_super()) {
        t = t->super();
        j = t->super_depth();
      }
      for (juint j1 = j+1; j1 < primary_super_limit(); j1++) {
        assert(primary_super_of_depth(j1) == nullptr, "super list padding");
      }
      while (t != nullptr) {
        assert(primary_super_of_depth(j) == t, "super list initialization");
        t = t->super();
        --j;
      }
      assert(j == (juint)-1, "correct depth count");
    }
#endif
  }

  _hash_code = get_next_hash(THREAD);

  if (secondary_supers() == nullptr) {
    initialize_secondary_supers(transitive_interfaces, CHECK);
  }
}

GrowableArray<Klass*>* Klass::compute_primary_supers(int num_extra_slots, GrowableArray<Klass*>* secondaries) {
  GrowableArray<Klass*>* primaries = new GrowableArray<Klass*>(num_extra_slots);

  if (num_extra_slots > 0) {
    assert(super() != nullptr, "");
    for (Klass* p = super(); !p->can_be_primary_super(); p = p->super()) {
      // Scan for overflow primaries being duplicates of 2nd'arys.
      //
      // This happens frequently for very deeply nested arrays: the
      // primary superclass chain overflows into the secondary.  The
      // secondary list contains the element_klass's secondaries with
      // an extra array dimension added.  If the element_klass's
      // secondary list already contains some primary overflows, they
      // (with the extra level of array-ness) will collide with the
      // normal primary superclass overflows.
      if (!secondaries->contains(p)) {
        primaries->push(p);
      }
    }
  }
  return primaries;
}

void Klass::initialize_secondary_supers(Array<InstanceKlass*>* transitive_interfaces, TRAPS) {
  // Now compute the list of secondary supertypes.
  // Secondaries can occasionally be on the super chain,
  // if the inline "_primary_supers" array overflows.
  int extras = 0;
  if (super() != nullptr) {
    for (Klass* p = super(); !p->can_be_primary_super(); p = p->super()) {
      ++extras;
    }
  }

  ResourceMark rm(THREAD);  // need to reclaim GrowableArrays allocated below

  // Compute the "real" non-extra secondaries.
  GrowableArray<Klass*>* secondaries = compute_secondary_supers(extras, transitive_interfaces);
  if (secondaries == nullptr) {
    return; // secondary_supers set by compute_secondary_supers
  }
  GrowableArray<Klass*>* primaries = compute_primary_supers(extras, secondaries);

  if (UseSecondarySupersTable && SecondarySupersMaxAttempts > 0) {
    initialize_secondary_supers_table(primaries, secondaries, CHECK);
  } else {
    // Combine the two arrays into a metadata object to pack the array.
    // The primaries are added in the reverse order, then the secondaries.
    int new_length = primaries->length() + secondaries->length();
    Array<Klass*>* s2 = MetadataFactory::new_array<Klass*>(
        class_loader_data(), new_length, CHECK);
    int fill_p = primaries->length();
    for (int j = 0; j < fill_p; j++) {
      s2->at_put(j, primaries->pop());  // add primaries in reverse order.
    }
    for( int j = 0; j < secondaries->length(); j++ ) {
      s2->at_put(j+fill_p, secondaries->at(j));  // add secondaries on the end.
    }
#ifdef ASSERT
    // We must not copy any NULL placeholders left over from bootstrap.
    for (int j = 0; j < s2->length(); j++) {
      assert(s2->at(j) != nullptr, "correct bootstrapping order");
    }
#endif
    set_secondary_supers(s2);
  }
  assert(secondary_supers() != nullptr, "");
}

static inline uintptr_t size_mask() {
  assert(is_power_of_2(SecondarySupersTableMaxSize), "");
  return ((SecondarySupersTableMaxSize << 1) - 1);
}

static inline uintptr_t size_shift() {
  assert(is_power_of_2(SecondarySupersTableMaxSize), "");
  return log2i_exact(SecondarySupersTableMaxSize) + 1;
}

static uint seed2size(uintptr_t seed) { return (seed >> 0 * size_shift()) & size_mask(); }
static uint seed2mask(uintptr_t seed) { return (seed >> 1 * size_shift()) & size_mask(); }


static inline uintptr_t compose_seed(uintptr_t h, uint table_size) {
  assert(table_size > 0, "");
  uintptr_t  seed_mask = ~right_n_bits(2 * size_shift());
  uintptr_t table_mask = (round_up_power_of_2(table_size) - 1);

  uintptr_t seed = (h & seed_mask) | (table_mask << size_shift()) | table_size;

  assert(seed2size(seed) == table_size, "");
  assert(seed2mask(seed) == (round_up_power_of_2(table_size) - 1), "");
  return seed;
}

static void put_element(uintptr_t seed,
                        Klass* const elem,
                        GrowableArray<Klass*>* table,
                        GrowableArray<Klass*>* secondary_list,
                        uint table_size, uint elem_count) {
  assert(elem != nullptr, "");
  assert(seed2size(seed) == table_size, "");
  assert(!table->contains(elem), "duplicate");

  int empty_slots = secondary_list->length() + table_size - elem_count;
  assert(empty_slots >= 0, "");
  assert(table_size > 0 || empty_slots == 0, "");
  if (empty_slots > 0) {
    uint idx = elem->index(seed, table_size);
    Klass* probe = table->at(idx);
    assert(probe != elem, "duplicated");
    if (probe == NULL) {
      table->at_put(idx, elem);
      return;
    }
  }
  secondary_list->push(elem);
}

static void pack_table(uintptr_t seed,
                       uint table_size,
                       GrowableArray<Klass*>* elements,
                       GrowableArray<Klass*>* table,
                       GrowableArray<Klass*>* conflicts) {
  for (uint idx = 0; idx < (uint) elements->length(); idx++) {
    put_element(seed, elements->at(idx), table, conflicts, table_size, idx);
  }
}

static void pack_table(uintptr_t seed,
                       uint table_size,
                       GrowableArray<Klass*>* primaries,
                       GrowableArray<Klass*>* secondaries,
                       GrowableArray<Klass*>* table,
                       GrowableArray<Klass*>* conflicts) {
  assert(seed2size(seed) == table_size, "");
  pack_table(seed, table_size, primaries,   table, conflicts);
  pack_table(seed, table_size, secondaries, table, conflicts);
  assert((uint) table->length() == table_size, "");
}

static uint resize_table(uint table_size, uint num_of_secondaries) {
  assert(table_size < SecondarySupersTableMaxSize, "");
  uint new_size = 0;
  bool is_power_of_2_sizes_only = (SecondarySupersTableSizingMode & 1) == 0;
  bool aggressive_sizing        = (SecondarySupersTableSizingMode & 4) != 0;
  if (is_power_of_2_sizes_only) {
    if (table_size > 0) {
      new_size = MIN2(table_size * 2, SecondarySupersTableMaxSize);
    } else {
      if (num_of_secondaries >= SecondarySupersTableMinSize) {
        if (aggressive_sizing) {
          new_size = round_up_power_of_2(num_of_secondaries);
          if (new_size == num_of_secondaries) {
            new_size = new_size * 2;
          }
        } else {
          new_size = round_down_power_of_2(num_of_secondaries);
        }
      }
    }
  } else {
    uint num_of_secondaries2 = num_of_secondaries / 2;
    if (table_size > 0) {
      new_size = MAX2(table_size - 1, MAX2(num_of_secondaries2, SecondarySupersTableMinSize)); // remove 1 extra slot at a time
    } else {
      if (num_of_secondaries >= SecondarySupersTableMinSize) {
        new_size = num_of_secondaries2 + MIN2(num_of_secondaries2, (uint)10);
      }
    }
  }
  new_size = MIN2(new_size, SecondarySupersTableMaxSize);

  assert(table_size > new_size || table_size == 0, "");
  return new_size;
}

Array<Klass*>* Klass::create_secondary_supers_table(uintptr_t seed1, GrowableArray<Klass*>* primary_table,
                                                    uintptr_t seed2, GrowableArray<Klass*>* secondary_table,
                                                    GrowableArray<Klass*>* conflicts, TRAPS) {
  assert(seed2size(seed1) == (uint)primary_table->length(), "");
  assert(seed2size(seed2) == (uint)secondary_table->length(), "");
  Array<Klass*>* secondary_supers = MetadataFactory::new_array<Klass*>(class_loader_data(),
                                                                       primary_table->length() + conflicts->length(), CHECK_NULL);
  int base_idx = 0;
  for (int j = 0; j < primary_table->length(); j++) {
    Klass* elem = primary_table->at(j);
    secondary_supers->at_put(base_idx + j, elem);
  }
  base_idx = base_idx + primary_table->length();
  for (int j = 0; j < secondary_table->length(); j++) {
    Klass* elem = secondary_table->at(j);
    secondary_supers->at_put(base_idx + j, elem);
  }
  base_idx = base_idx + secondary_table->length();
  for (int j = 0; j < conflicts->length(); j++) {
    secondary_supers->at_put(base_idx + j, conflicts->at(j));
  }
  return secondary_supers;
}

static bool is_done(uint table_size, uint num_of_conflicts, uint num_of_secondaries) {
  if (table_size == 0) {
    assert(num_of_conflicts == num_of_secondaries, "");
    return true; // empty table: nothing more to do
  }
  if (table_size + num_of_conflicts == num_of_secondaries) {
    return true; // table is full
  }
  return false;
}

static inline uintptr_t get_random_seed(Thread* t, uint table_size) {
  assert(table_size <= SecondarySupersTableMaxSize, "");
  if (table_size > 0) {
    uintptr_t seed = get_next_hash(t);
    return compose_seed(seed, table_size);
  }
  return 0;
}

static uint nz_count(Array<Klass*>* arr) {
  uint cnt = 0;
  for (int i = 0; i < arr->length(); i++) {
    if (arr->at(i) != nullptr && arr->at(i) != vmClasses::Object_klass()) {
      ++cnt;
    }
  }
  assert(cnt <= (uint)arr->length(), "");
  return cnt;
}

static uint nz_count(GrowableArray<Klass*>* arr) {
  uint cnt = 0;
  for (int i = 0; i < arr->length(); i++) {
    if (arr->at(i) != nullptr && arr->at(i) != vmClasses::Object_klass()) {
      ++cnt;
    }
  }
  assert(cnt <= (uint)arr->length(), "");
  return cnt;
}

static void print_entry(outputStream* st, int idx, Klass* k, uintptr_t seed, uint table_size) {
  st->print("| %3d: ", idx);
  if (k == nullptr) {
    st->print_cr("NULL");
  } else {
    st->print(UINTX_FORMAT_X_0 " %-40s h=" UINTX_FORMAT_X_0,
              (uintptr_t)k, k->external_name(), k->hash_code());
    if (table_size > 0) {
      uint idx = k->index(seed, table_size);
      st->print(" idx=%02d", idx);
    }
    st->cr();
  }
}

static GrowableArray<uint>* compute_conflicts(uintptr_t seed, GrowableArray<Klass*>* table, GrowableArray<Klass*>* tail) {
  uint table_size = table->length();
  GrowableArray<uint>* conflicts = new GrowableArray<uint>(table_size, table_size, 0);

  // Scan secondary table for possible conflicts with primary table.
  for (int i = 0; i < tail->length(); i++) {
    Klass* k = tail->at(i);
    assert(k != nullptr, ""); // no empty slots in tail part allowed
    uint primary_idx = k->index(seed, table_size);
    assert(table->at(primary_idx) != nullptr, "no conflict detected");
    conflicts->at_put(primary_idx, conflicts->at(primary_idx) + 1);
  }
  return conflicts;
}

// Compute weight of a negative lookup in the primary table.
static double compute_weight1(GrowableArray<Klass*>* table, GrowableArray<uint>* conflicts) {
  uint cnt = 0;
  for (int i = 0; i < table->length(); i++) {
    bool has_conflict = (conflicts->at(i) > 0);
    cnt += (has_conflict ? 1 : 0);
  }
  double conflict_prob = (1.0 * cnt) / table->length();
  return 1.0 - conflict_prob;
}

static double compute_weight(uintptr_t seed, GrowableArray<Klass*>* table, GrowableArray<Klass*>* tail) {
  uint table_size = table->length();

  if (table_size > 0) {
    GrowableArray<uint>* conflicts = compute_conflicts(seed, table, tail);
    double ncoeff1 = compute_weight1(table, conflicts);
    double ncoeff2 = 1.0 - ncoeff1;
    assert(ncoeff1 >= 0.0 && ncoeff2 >= 0.0, "");

    uint table_nz = nz_count(table);
    uint num_of_elements = table_nz + tail->length();
    double packing_density = (1.0 * table_nz) / table_size; // 0...1.0

    double packed = (1.0 * table_size) / num_of_elements;  // 0.5...1.0
    return 1.0 / (4.0 * packing_density + 1.0 * packed);
  } else {
    return tail->length(); // empty table
  }
}

static void print_table(outputStream* st, uintptr_t seed, GrowableArray<Klass*>* table, GrowableArray<Klass*>* tail, bool verbose) {
  uint table_size = table->length();

  double ncoeff1 = 0.0;

  if (table_size > 0) {
    GrowableArray<uint>* conflicts = compute_conflicts(seed, table, tail);
    ncoeff1 = compute_weight1(table, conflicts);

    st->print("-------------- PRIMARY -------------------");
    {
      uint primary_cnt = 0;
      uint empty_cnt = 0;
      for (uint i = 0; i < table_size; i++) {
        bool has_conflict = (conflicts->at(i) > 0);
        primary_cnt += (has_conflict ? 1 : 0);
        empty_cnt += (table->at(i) == nullptr ? 1 : 0);
      }
      st->print_cr(" empty=%d conflicts=%d size=%d", empty_cnt, primary_cnt, table_size);
    }
    if (verbose) {
      for (uint i = 0; i < table_size; i++) {
        bool has_conflict = (conflicts->at(i) > 0);
        st->print("%s", (has_conflict ? " * " : "   "));
        Klass* s = table->at(i);
        assert(!has_conflict || s != nullptr, "");
        print_entry(st, i, s, seed, table_size);
      }
    }
  }
  st->print("-------------- LINEAR --------------------");
  uint tail_size = tail->length();
  uint num_of_secondaries = nz_count(table) + tail_size;
  st->print_cr(" size=%d total=%d", tail_size, num_of_secondaries);
  if (verbose) {
    for (uint i = 0; i < tail_size; i++) {
      Klass* s = tail->at(i);
      assert(s != nullptr, "");
      st->print_raw("   ");
      print_entry(st, i, s, seed, table_size);
    }
  }
  st->print_cr("------------------------------------------");
  {
    double ncoeff2 = 1.0 - ncoeff1;
    double nweight = compute_weight(seed, table, tail);
    st->print_cr("negative: nweight=%f ncoeff1=%f ncoeff2=%f",
                 nweight, ncoeff1, ncoeff2);
  }
}

static uintptr_t initial_seed(Klass* k, GrowableArray<Klass*>* secondaries, uint table_size) {
  if (table_size > 0) {
    Klass* super = k->java_super();
    uintptr_t seed = super->secondary_supers_seed();
    uint  best_cnt = nz_count(super->secondary_supers());

    for (int i = 0; i < secondaries->length(); i++) {
      Klass* k1 = secondaries->at(i);
      if (k1->secondary_supers_table_size() > 0 && nz_count(k1->secondary_supers()) > best_cnt) {
        seed = k1->secondary_supers_seed();
      }
    }
    return compose_seed(seed, table_size);
  }
  return 0;
}

uint Klass::index(uintptr_t seed, uint table_size) {
  bool is_power_of_2_sizes_only = (SecondarySupersTableSizingMode & 1) == 0;
  bool mod_rounding_mode        = (SecondarySupersTableSizingMode & 8) == 0;

  assert(table_size > 0, "");
  assert(is_power_of_2(table_size) || !is_power_of_2_sizes_only, "");
  assert(seed2size(seed) == table_size, "");
  assert(seed2mask(seed) == (round_up_power_of_2(table_size) - 1), "");

  uint64_t h2 = get_hash64(seed, hash_code());
  if (is_power_of_2_sizes_only) {
    uint mask = table_size - 1;
    return h2 & mask;
  } else if (mod_rounding_mode) {
    return (h2 % table_size);
  } else {
    uint mask = round_up_power_of_2(table_size) - 1;
    uint offset = (table_size - 1);
    uint h3 = h2 & mask;
    uint h4 = uabs(h3 - offset);
    return h4;
  }
}

static void tmp(Klass* k, GrowableArray<Klass*>* secondaries, TRAPS) {
  ResourceMark rm(THREAD);

  uint num_of_secondaries = secondaries->length();

  uintptr_t best_seed = 0;
  double best_score = 1.0 * (num_of_secondaries + 1 + 2);
  GrowableArray<Klass*>* best_table = new GrowableArray<Klass*>(SecondarySupersTableMaxSize);
  GrowableArray<Klass*>* best_tail            = new GrowableArray<Klass*>(num_of_secondaries);

  GrowableArray<Klass*>* best_secondary_tail = new GrowableArray<Klass*>(num_of_secondaries);

  uint table_size = resize_table(0, num_of_secondaries);

  uint total_attempts = 0;
  bool is_in_progress = true;
  while (is_in_progress) {
    if (TraceSecondarySupers) {
      tty->print_cr("#%d: 2ND: secondary_supers_table: %s: total=%d table_size=%d best_table=%d best_tail=%d best_score=%f best_seed=" UINTX_FORMAT_X_0,
                    total_attempts, k->name()->as_C_string(), num_of_secondaries, table_size, best_table->length(), best_tail->length(), best_score, best_seed);
    }

    for (uint attempt = 0; attempt < SecondarySupersMaxAttempts && is_in_progress; attempt++, total_attempts++) {
      ResourceMark rm(THREAD);  // need to reclaim GrowableArrays allocated below

      GrowableArray<Klass*>* table = new GrowableArray<Klass*>(table_size, table_size, nullptr);
      GrowableArray<Klass*>* tail  = new GrowableArray<Klass*>(num_of_secondaries);

      uintptr_t seed = get_random_seed(THREAD, table_size);

      pack_table(seed, table_size, secondaries,
                 table, tail); // results
      double score = compute_weight(seed, table, tail);
      if (score < best_score) {
        best_score = score;
        best_seed  = seed;

        best_table->clear();
        assert(table->length() <= best_table->capacity(), "no resizing allowed");
        best_table->appendAll(table);

        best_tail->clear();
        best_tail->appendAll(tail);

        if (TraceSecondarySupers) {
          tty->print_cr("#%d: 2ND: secondary_supers_table: %s: total=%d size=%d num_of_conflicts=%d score=%f seed=" UINTX_FORMAT_X_0,
                        total_attempts, k->name()->as_C_string(), num_of_secondaries, table_size, tail->length(), best_score, best_seed);
          print_table(tty, seed, table, tail, false);
        }

        is_in_progress = !is_done(best_table->length(), best_tail->length(), num_of_secondaries);
      }
    }
    if (is_in_progress) {
      table_size = resize_table(table_size, num_of_secondaries); // try different size
      is_in_progress = (table_size > (num_of_secondaries / 2));
    }
  }
  assert(num_of_secondaries <= (uint) best_table->length() + (uint) best_tail->length(), "");

  if (TraceSecondarySupers) {
    ttyLocker ttyl;
    tty->print_cr("2ND: secondary_supers_table: END: %s: attempts=%d",
                  k->name()->as_C_string(), total_attempts);
    print_table(tty, best_seed, best_table, best_tail, true);
  }
}

void Klass::initialize_secondary_supers_table(GrowableArray<Klass*>* primaries, GrowableArray<Klass*>* secondaries, TRAPS) {
  ResourceMark rm(THREAD);  // need to reclaim GrowableArrays allocated below

  elapsedTimer et;
  et.start();

  uint num_of_secondaries = primaries->length() + secondaries->length();

  uintptr_t best_seed = 0;
  double best_score = 1.0 * (num_of_secondaries + 1 + 2);
  GrowableArray<Klass*>* best_primary_table   = new GrowableArray<Klass*>(SecondarySupersTableMaxSize);
  GrowableArray<Klass*>* best_secondary_table = new GrowableArray<Klass*>(SecondarySupersTableMaxSize);
  GrowableArray<Klass*>* best_tail            = new GrowableArray<Klass*>(num_of_secondaries);

  uint table_size = resize_table(0, num_of_secondaries);

  uint total_attempts = 0;
  bool is_in_progress = true;
  while (is_in_progress) {
    if (TraceSecondarySupers) {
      tty->print_cr("#%d: secondary_supers_table: %s: total=%d table_size=%d best_table=%d best_tail=%d best_score=%f best_seed=" UINTX_FORMAT_X_0,
                    total_attempts, name()->as_C_string(), num_of_secondaries, table_size, best_primary_table->length(), best_tail->length(), best_score, best_seed);
    }

    for (uint attempt = 0; attempt < SecondarySupersMaxAttempts && is_in_progress; attempt++, total_attempts++) {
      ResourceMark rm(THREAD);  // need to reclaim GrowableArrays allocated below

      GrowableArray<Klass*>* table = new GrowableArray<Klass*>(table_size, table_size, nullptr);
      GrowableArray<Klass*>* tail  = new GrowableArray<Klass*>(num_of_secondaries);

      uintptr_t seed = (attempt == 0 ? initial_seed(this, secondaries, table_size)
                                     : get_random_seed(THREAD, table_size));

      pack_table(seed, table_size, primaries, secondaries,
                 table, tail); // results
      double score = compute_weight(seed, table, tail);
      if (score < best_score) {
        best_score = score;
        best_seed  = seed;

        best_primary_table->clear();
        assert(table->length() <= best_primary_table->capacity(), "no resizing allowed");
        best_primary_table->appendAll(table);

        best_tail->clear();
        best_tail->appendAll(tail);

        if (TraceSecondarySupers) {
          tty->print_cr("#%d: secondary_supers_table: %s: total=%d size=%d num_of_conflicts=%d score=%f seed=" UINTX_FORMAT_X_0,
                        total_attempts, name()->as_C_string(), num_of_secondaries, table_size, tail->length(), best_score, best_seed);
          print_table(tty, seed, table, tail, false);
        }

        is_in_progress = !is_done(best_primary_table->length(), best_tail->length(), num_of_secondaries);
      }
    }
    if (is_in_progress) {
      table_size = resize_table(table_size, num_of_secondaries); // try different size
      is_in_progress = (table_size > (num_of_secondaries / 2));
    }
  }
  assert(num_of_secondaries <= (uint) best_primary_table->length() + (uint) best_tail->length(), "");

  Array<Klass*>* ss_table = create_secondary_supers_table(best_seed, best_primary_table, 0, best_secondary_table, best_tail, CHECK);
  assert(secondary_supers() == nullptr, "");
  set_secondary_supers(ss_table, best_seed);
  assert((uint)best_primary_table->length() == secondary_supers_table_size(), "mismatch");

  if (TraceSecondarySupers) {
    ttyLocker ttyl;
    tty->print_cr("1ST: secondary_supers_table: END: %s: attempts=%d",
                  name()->as_C_string(), total_attempts);
    dump_on(tty, true);
  }

  if (UseNewCode) {
    if ((uint)best_tail->length() > SecondarySupersTableMinSize) {
      tmp(this, best_tail, THREAD);
    }
  }

  et.stop();

  if (TraceSecondarySupers) {
    tty->print_cr("secondary_supers_table: END: %s: elapsed_time=%ld ms (ticks=%ld)",
                  name()->as_C_string(), et.milliseconds(), et.ticks());
  }
}

void Klass::dump_on(outputStream* st, bool verbose) {
  ResourceMark rm;
  uint table_size = secondary_supers_table_size();
  uintptr_t seed = secondary_supers_seed();

  st->print_cr("================= TABLE ==================");
  st->print_cr("--- %s table_size=%d seed=" UINTX_FORMAT_X_0 " ---",
               external_name(), table_size, seed);
  st->print_cr("------------------------------------------");
  if (secondary_supers() != nullptr) {
    // Array<Klass*> => 2 * GrowableArray<Klass*>
    GrowableArray<Klass*>* table = new GrowableArray<Klass*>(table_size, table_size, nullptr);
    for (uint i = 0; i < table_size; i++) {
      table->at_put(i, secondary_supers()->at(i));
    }
    uint num_of_conflicts = secondary_supers()->length() - table_size;
    GrowableArray<Klass*>* conflicts = new GrowableArray<Klass*>(num_of_conflicts, num_of_conflicts, nullptr);
    for (uint i = 0; i < num_of_conflicts; i++) {
      conflicts->at_put(i, secondary_supers()->at(table_size + i));
    }
    print_table(st, seed, table, conflicts, verbose);
  } else {
    st->print_cr("NULL");
  }
  st->print_cr("==========================================");
}


GrowableArray<Klass*>* Klass::compute_secondary_supers(int num_extra_slots,
                                                       Array<InstanceKlass*>* transitive_interfaces) {
  assert(num_extra_slots == 0, "override for complex klasses");
  assert(transitive_interfaces == nullptr, "sanity");
  set_secondary_supers(Universe::the_empty_klass_array());
  return nullptr;
}


// superklass links
InstanceKlass* Klass::superklass() const {
  assert(super() == nullptr || super()->is_instance_klass(), "must be instance klass");
  return _super == nullptr ? nullptr : InstanceKlass::cast(_super);
}

// subklass links.  Used by the compiler (and vtable initialization)
// May be cleaned concurrently, so must use the Compile_lock.
// The log parameter is for clean_weak_klass_links to report unlinked classes.
Klass* Klass::subklass(bool log) const {
  // Need load_acquire on the _subklass, because it races with inserts that
  // publishes freshly initialized data.
  for (Klass* chain = Atomic::load_acquire(&_subklass);
       chain != nullptr;
       // Do not need load_acquire on _next_sibling, because inserts never
       // create _next_sibling edges to dead data.
       chain = Atomic::load(&chain->_next_sibling))
  {
    if (chain->is_loader_alive()) {
      return chain;
    } else if (log) {
      if (log_is_enabled(Trace, class, unload)) {
        ResourceMark rm;
        log_trace(class, unload)("unlinking class (subclass): %s", chain->external_name());
      }
    }
  }
  return nullptr;
}

Klass* Klass::next_sibling(bool log) const {
  // Do not need load_acquire on _next_sibling, because inserts never
  // create _next_sibling edges to dead data.
  for (Klass* chain = Atomic::load(&_next_sibling);
       chain != nullptr;
       chain = Atomic::load(&chain->_next_sibling)) {
    // Only return alive klass, there may be stale klass
    // in this chain if cleaned concurrently.
    if (chain->is_loader_alive()) {
      return chain;
    } else if (log) {
      if (log_is_enabled(Trace, class, unload)) {
        ResourceMark rm;
        log_trace(class, unload)("unlinking class (sibling): %s", chain->external_name());
      }
    }
  }
  return nullptr;
}

void Klass::set_subklass(Klass* s) {
  assert(s != this, "sanity check");
  Atomic::release_store(&_subklass, s);
}

void Klass::set_next_sibling(Klass* s) {
  assert(s != this, "sanity check");
  // Does not need release semantics. If used by cleanup, it will link to
  // already safely published data, and if used by inserts, will be published
  // safely using cmpxchg.
  Atomic::store(&_next_sibling, s);
}

void Klass::append_to_sibling_list() {
  if (Universe::is_fully_initialized()) {
    assert_locked_or_safepoint(Compile_lock);
  }
  debug_only(verify();)
  // add ourselves to superklass' subklass list
  InstanceKlass* super = superklass();
  if (super == nullptr) return;     // special case: class Object
  assert((!super->is_interface()    // interfaces cannot be supers
          && (super->superklass() == nullptr || !is_interface())),
         "an interface can only be a subklass of Object");

  // Make sure there is no stale subklass head
  super->clean_subklass();

  for (;;) {
    Klass* prev_first_subklass = Atomic::load_acquire(&_super->_subklass);
    if (prev_first_subklass != nullptr) {
      // set our sibling to be the superklass' previous first subklass
      assert(prev_first_subklass->is_loader_alive(), "May not attach not alive klasses");
      set_next_sibling(prev_first_subklass);
    }
    // Note that the prev_first_subklass is always alive, meaning no sibling_next links
    // are ever created to not alive klasses. This is an important invariant of the lock-free
    // cleaning protocol, that allows us to safely unlink dead klasses from the sibling list.
    if (Atomic::cmpxchg(&super->_subklass, prev_first_subklass, this) == prev_first_subklass) {
      return;
    }
  }
  debug_only(verify();)
}

void Klass::clean_subklass() {
  for (;;) {
    // Need load_acquire, due to contending with concurrent inserts
    Klass* subklass = Atomic::load_acquire(&_subklass);
    if (subklass == nullptr || subklass->is_loader_alive()) {
      return;
    }
    // Try to fix _subklass until it points at something not dead.
    Atomic::cmpxchg(&_subklass, subklass, subklass->next_sibling());
  }
}

void Klass::clean_weak_klass_links(bool unloading_occurred, bool clean_alive_klasses) {
  if (!ClassUnloading || !unloading_occurred) {
    return;
  }

  Klass* root = vmClasses::Object_klass();
  Stack<Klass*, mtGC> stack;

  stack.push(root);
  while (!stack.is_empty()) {
    Klass* current = stack.pop();

    assert(current->is_loader_alive(), "just checking, this should be live");

    // Find and set the first alive subklass
    Klass* sub = current->subklass(true);
    current->clean_subklass();
    if (sub != nullptr) {
      stack.push(sub);
    }

    // Find and set the first alive sibling
    Klass* sibling = current->next_sibling(true);
    current->set_next_sibling(sibling);
    if (sibling != nullptr) {
      stack.push(sibling);
    }

    // Clean the implementors list and method data.
    if (clean_alive_klasses && current->is_instance_klass()) {
      InstanceKlass* ik = InstanceKlass::cast(current);
      ik->clean_weak_instanceklass_links();

      // JVMTI RedefineClasses creates previous versions that are not in
      // the class hierarchy, so process them here.
      while ((ik = ik->previous_versions()) != nullptr) {
        ik->clean_weak_instanceklass_links();
      }
    }
  }
}

void Klass::metaspace_pointers_do(MetaspaceClosure* it) {
  if (log_is_enabled(Trace, cds)) {
    ResourceMark rm;
    log_trace(cds)("Iter(Klass): %p (%s)", this, external_name());
  }

  it->push(&_name);
  it->push(&_secondary_super_cache);
  it->push(&_secondary_supers);
  for (int i = 0; i < _primary_super_limit; i++) {
    it->push(&_primary_supers[i]);
  }
  it->push(&_super);
  if (!Arguments::is_dumping_archive()) {
    // If dumping archive, these may point to excluded classes. There's no need
    // to follow these pointers anyway, as they will be set to null in
    // remove_unshareable_info().
    it->push((Klass**)&_subklass);
    it->push((Klass**)&_next_sibling);
    it->push(&_next_link);
  }

  vtableEntry* vt = start_of_vtable();
  for (int i=0; i<vtable_length(); i++) {
    it->push(vt[i].method_addr());
  }
}

#if INCLUDE_CDS
void Klass::remove_unshareable_info() {
  assert (Arguments::is_dumping_archive(),
          "only called during CDS dump time");
  JFR_ONLY(REMOVE_ID(this);)
  if (log_is_enabled(Trace, cds, unshareable)) {
    ResourceMark rm;
    log_trace(cds, unshareable)("remove: %s", external_name());
  }

  set_subklass(nullptr);
  set_next_sibling(nullptr);
  set_next_link(nullptr);

  // Null out class_loader_data because we don't share that yet.
  set_class_loader_data(nullptr);
  set_is_shared();
}

void Klass::remove_java_mirror() {
  Arguments::assert_is_dumping_archive();
  if (log_is_enabled(Trace, cds, unshareable)) {
    ResourceMark rm;
    log_trace(cds, unshareable)("remove java_mirror: %s", external_name());
  }
  // Just null out the mirror.  The class_loader_data() no longer exists.
  clear_java_mirror_handle();
}

void Klass::restore_unshareable_info(ClassLoaderData* loader_data, Handle protection_domain, TRAPS) {
  assert(is_klass(), "ensure C++ vtable is restored");
  assert(is_shared(), "must be set");
  JFR_ONLY(RESTORE_ID(this);)
  if (log_is_enabled(Trace, cds, unshareable)) {
    ResourceMark rm(THREAD);
    log_trace(cds, unshareable)("restore: %s", external_name());
  }

  // If an exception happened during CDS restore, some of these fields may already be
  // set.  We leave the class on the CLD list, even if incomplete so that we don't
  // modify the CLD list outside a safepoint.
  if (class_loader_data() == nullptr) {
    set_class_loader_data(loader_data);

    // Add to class loader list first before creating the mirror
    // (same order as class file parsing)
    loader_data->add_class(this);
  }

  Handle loader(THREAD, loader_data->class_loader());
  ModuleEntry* module_entry = nullptr;
  Klass* k = this;
  if (k->is_objArray_klass()) {
    k = ObjArrayKlass::cast(k)->bottom_klass();
  }
  // Obtain klass' module.
  if (k->is_instance_klass()) {
    InstanceKlass* ik = (InstanceKlass*) k;
    module_entry = ik->module();
  } else {
    module_entry = ModuleEntryTable::javabase_moduleEntry();
  }
  // Obtain java.lang.Module, if available
  Handle module_handle(THREAD, ((module_entry != nullptr) ? module_entry->module() : (oop)nullptr));

  if (this->has_archived_mirror_index()) {
    ResourceMark rm(THREAD);
    log_debug(cds, mirror)("%s has raw archived mirror", external_name());
    if (ArchiveHeapLoader::are_archived_mirrors_available()) {
      bool present = java_lang_Class::restore_archived_mirror(this, loader, module_handle,
                                                              protection_domain,
                                                              CHECK);
      if (present) {
        return;
      }
    }

    // No archived mirror data
    log_debug(cds, mirror)("No archived mirror data for %s", external_name());
    clear_java_mirror_handle();
    this->clear_archived_mirror_index();
  }

  // Only recreate it if not present.  A previous attempt to restore may have
  // gotten an OOM later but keep the mirror if it was created.
  if (java_mirror() == nullptr) {
    ResourceMark rm(THREAD);
    log_trace(cds, mirror)("Recreate mirror for %s", external_name());
    java_lang_Class::create_mirror(this, loader, module_handle, protection_domain, Handle(), CHECK);
  }
}
#endif // INCLUDE_CDS

#if INCLUDE_CDS_JAVA_HEAP
oop Klass::archived_java_mirror() {
  assert(has_archived_mirror_index(), "must have archived mirror");
  return HeapShared::get_root(_archived_mirror_index);
}

void Klass::clear_archived_mirror_index() {
  if (_archived_mirror_index >= 0) {
    HeapShared::clear_root(_archived_mirror_index);
  }
  _archived_mirror_index = -1;
}

// No GC barrier
void Klass::set_archived_java_mirror(int mirror_index) {
  assert(DumpSharedSpaces, "called only during dumptime");
  _archived_mirror_index = mirror_index;
}
#endif // INCLUDE_CDS_JAVA_HEAP

void Klass::check_array_allocation_length(int length, int max_length, TRAPS) {
  if (length > max_length) {
    if (!THREAD->in_retryable_allocation()) {
      report_java_out_of_memory("Requested array size exceeds VM limit");
      JvmtiExport::post_array_size_exhausted();
      THROW_OOP(Universe::out_of_memory_error_array_size());
    } else {
      THROW_OOP(Universe::out_of_memory_error_retry());
    }
  } else if (length < 0) {
    THROW_MSG(vmSymbols::java_lang_NegativeArraySizeException(), err_msg("%d", length));
  }
}

// Replace the last '+' char with '/'.
static char* convert_hidden_name_to_java(Symbol* name) {
  size_t name_len = name->utf8_length();
  char* result = NEW_RESOURCE_ARRAY(char, name_len + 1);
  name->as_klass_external_name(result, (int)name_len + 1);
  for (int index = (int)name_len; index > 0; index--) {
    if (result[index] == '+') {
      result[index] = JVM_SIGNATURE_SLASH;
      break;
    }
  }
  return result;
}

// In product mode, this function doesn't have virtual function calls so
// there might be some performance advantage to handling InstanceKlass here.
const char* Klass::external_name() const {
  if (is_instance_klass()) {
    const InstanceKlass* ik = static_cast<const InstanceKlass*>(this);
    if (ik->is_hidden()) {
      char* result = convert_hidden_name_to_java(name());
      return result;
    }
  } else if (is_objArray_klass() && ObjArrayKlass::cast(this)->bottom_klass()->is_hidden()) {
    char* result = convert_hidden_name_to_java(name());
    return result;
  }
  if (name() == nullptr)  return "<unknown>";
  return name()->as_klass_external_name();
}

const char* Klass::signature_name() const {
  if (name() == nullptr)  return "<unknown>";
  if (is_objArray_klass() && ObjArrayKlass::cast(this)->bottom_klass()->is_hidden()) {
    size_t name_len = name()->utf8_length();
    char* result = NEW_RESOURCE_ARRAY(char, name_len + 1);
    name()->as_C_string(result, (int)name_len + 1);
    for (int index = (int)name_len; index > 0; index--) {
      if (result[index] == '+') {
        result[index] = JVM_SIGNATURE_DOT;
        break;
      }
    }
    return result;
  }
  return name()->as_C_string();
}

const char* Klass::external_kind() const {
  if (is_interface()) return "interface";
  if (is_abstract()) return "abstract class";
  return "class";
}

// Unless overridden, jvmti_class_status has no flags set.
jint Klass::jvmti_class_status() const {
  return 0;
}


// Printing

void Klass::print_on(outputStream* st) const {
  ResourceMark rm;
  // print title
  st->print("%s", internal_name());
  print_address_on(st);
  st->cr();
}

#define BULLET  " - "

// Caller needs ResourceMark
void Klass::oop_print_on(oop obj, outputStream* st) {
  // print title
  st->print_cr("%s ", internal_name());
  obj->print_address_on(st);

  if (WizardMode) {
     // print header
     obj->mark().print_on(st);
     st->cr();
  }

  // print class
  st->print(BULLET"klass: ");
  obj->klass()->print_value_on(st);
  st->cr();
}

void Klass::oop_print_value_on(oop obj, outputStream* st) {
  // print title
  ResourceMark rm;              // Cannot print in debug mode without this
  st->print("%s", internal_name());
  obj->print_address_on(st);
}

// Verification

void Klass::verify_on(outputStream* st) {

  // This can be expensive, but it is worth checking that this klass is actually
  // in the CLD graph but not in production.
  assert(Metaspace::contains((address)this), "Should be");

  guarantee(this->is_klass(),"should be klass");

  if (super() != nullptr) {
    guarantee(super()->is_klass(), "should be klass");
  }
  if (_secondary_super_cache != nullptr) {
    Klass* ko = _secondary_super_cache;
    guarantee(ko->is_klass(), "should be klass");
  }
  for ( uint i = 0; i < primary_super_limit(); i++ ) {
    Klass* ko = _primary_supers[i];
    if (ko != nullptr) {
      guarantee(ko->is_klass(), "should be klass");
    }
  }

  if (java_mirror_no_keepalive() != nullptr) {
    guarantee(java_lang_Class::is_instance(java_mirror_no_keepalive()), "should be instance");
  }

  if (secondary_supers() != nullptr) {
    uint table_size = secondary_supers_table_size();
    if (table_size > 0) {
      bool is_power_of_2_sizes_only = (SecondarySupersTableSizingMode & 1) == 0;
      guarantee(!is_power_of_2_sizes_only || is_power_of_2(table_size), "");
      for (uint idx = 0; idx < table_size; idx++) {
        Klass* k = secondary_supers()->at(idx);
        if (k != nullptr) {
          uintptr_t seed = secondary_supers_seed();
          uint idx = k->index(seed, table_size);
          guarantee(idx == idx, "misplaced");
          guarantee(secondary_supers()->contains(k), "absent");
        }
      }
    }
  }
}

void Klass::oop_verify_on(oop obj, outputStream* st) {
  guarantee(oopDesc::is_oop(obj),  "should be oop");
  guarantee(obj->klass()->is_klass(), "klass field is not a klass");
}

bool Klass::is_valid(Klass* k) {
  if (!is_aligned(k, sizeof(MetaWord))) return false;
  if ((size_t)k < os::min_page_size()) return false;

  if (!os::is_readable_range(k, k + 1)) return false;
  if (!Metaspace::contains(k)) return false;

  if (!Symbol::is_valid(k->name())) return false;
  return ClassLoaderDataGraph::is_valid(k->class_loader_data());
}

Method* Klass::method_at_vtable(int index)  {
#ifndef PRODUCT
  assert(index >= 0, "valid vtable index");
  if (DebugVtables) {
    verify_vtable_index(index);
  }
#endif
  return start_of_vtable()[index].method();
}


#ifndef PRODUCT

bool Klass::verify_vtable_index(int i) {
  int limit = vtable_length()/vtableEntry::size();
  assert(i >= 0 && i < limit, "index %d out of bounds %d", i, limit);
  return true;
}

#endif // PRODUCT

// Caller needs ResourceMark
// joint_in_module_of_loader provides an optimization if 2 classes are in
// the same module to succinctly print out relevant information about their
// module name and class loader's name_and_id for error messages.
// Format:
//   <fully-qualified-external-class-name1> and <fully-qualified-external-class-name2>
//                      are in module <module-name>[@<version>]
//                      of loader <loader-name_and_id>[, parent loader <parent-loader-name_and_id>]
const char* Klass::joint_in_module_of_loader(const Klass* class2, bool include_parent_loader) const {
  assert(module() == class2->module(), "classes do not have the same module");
  const char* class1_name = external_name();
  size_t len = strlen(class1_name) + 1;

  const char* class2_description = class2->class_in_module_of_loader(true, include_parent_loader);
  len += strlen(class2_description);

  len += strlen(" and ");

  char* joint_description = NEW_RESOURCE_ARRAY_RETURN_NULL(char, len);

  // Just return the FQN if error when allocating string
  if (joint_description == nullptr) {
    return class1_name;
  }

  jio_snprintf(joint_description, len, "%s and %s",
               class1_name,
               class2_description);

  return joint_description;
}

// Caller needs ResourceMark
// class_in_module_of_loader provides a standard way to include
// relevant information about a class, such as its module name as
// well as its class loader's name_and_id, in error messages and logging.
// Format:
//   <fully-qualified-external-class-name> is in module <module-name>[@<version>]
//                                         of loader <loader-name_and_id>[, parent loader <parent-loader-name_and_id>]
const char* Klass::class_in_module_of_loader(bool use_are, bool include_parent_loader) const {
  // 1. fully qualified external name of class
  const char* klass_name = external_name();
  size_t len = strlen(klass_name) + 1;

  // 2. module name + @version
  const char* module_name = "";
  const char* version = "";
  bool has_version = false;
  bool module_is_named = false;
  const char* module_name_phrase = "";
  const Klass* bottom_klass = is_objArray_klass() ?
                                ObjArrayKlass::cast(this)->bottom_klass() : this;
  if (bottom_klass->is_instance_klass()) {
    ModuleEntry* module = InstanceKlass::cast(bottom_klass)->module();
    if (module->is_named()) {
      module_is_named = true;
      module_name_phrase = "module ";
      module_name = module->name()->as_C_string();
      len += strlen(module_name);
      // Use version if exists and is not a jdk module
      if (module->should_show_version()) {
        has_version = true;
        version = module->version()->as_C_string();
        // Include stlen(version) + 1 for the "@"
        len += strlen(version) + 1;
      }
    } else {
      module_name = UNNAMED_MODULE;
      len += UNNAMED_MODULE_LEN;
    }
  } else {
    // klass is an array of primitives, module is java.base
    module_is_named = true;
    module_name_phrase = "module ";
    module_name = JAVA_BASE_NAME;
    len += JAVA_BASE_NAME_LEN;
  }

  // 3. class loader's name_and_id
  ClassLoaderData* cld = class_loader_data();
  assert(cld != nullptr, "class_loader_data should not be null");
  const char* loader_name_and_id = cld->loader_name_and_id();
  len += strlen(loader_name_and_id);

  // 4. include parent loader information
  const char* parent_loader_phrase = "";
  const char* parent_loader_name_and_id = "";
  if (include_parent_loader &&
      !cld->is_builtin_class_loader_data()) {
    oop parent_loader = java_lang_ClassLoader::parent(class_loader());
    ClassLoaderData *parent_cld = ClassLoaderData::class_loader_data_or_null(parent_loader);
    // The parent loader's ClassLoaderData could be null if it is
    // a delegating class loader that has never defined a class.
    // In this case the loader's name must be obtained via the parent loader's oop.
    if (parent_cld == nullptr) {
      oop cl_name_and_id = java_lang_ClassLoader::nameAndId(parent_loader);
      if (cl_name_and_id != nullptr) {
        parent_loader_name_and_id = java_lang_String::as_utf8_string(cl_name_and_id);
      }
    } else {
      parent_loader_name_and_id = parent_cld->loader_name_and_id();
    }
    parent_loader_phrase = ", parent loader ";
    len += strlen(parent_loader_phrase) + strlen(parent_loader_name_and_id);
  }

  // Start to construct final full class description string
  len += ((use_are) ? strlen(" are in ") : strlen(" is in "));
  len += strlen(module_name_phrase) + strlen(" of loader ");

  char* class_description = NEW_RESOURCE_ARRAY_RETURN_NULL(char, len);

  // Just return the FQN if error when allocating string
  if (class_description == nullptr) {
    return klass_name;
  }

  jio_snprintf(class_description, len, "%s %s in %s%s%s%s of loader %s%s%s",
               klass_name,
               (use_are) ? "are" : "is",
               module_name_phrase,
               module_name,
               (has_version) ? "@" : "",
               (has_version) ? version : "",
               loader_name_and_id,
               parent_loader_phrase,
               parent_loader_name_and_id);

  return class_description;
}
