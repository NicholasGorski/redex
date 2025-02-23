/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Reachability.h"

#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/range/adaptor/map.hpp>

#include "BinarySerialization.h"
#include "DexAnnotation.h"
#include "DexUtil.h"
#include "ProguardConfiguration.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace reachability;

namespace bs = binary_serialization;
namespace mog = method_override_graph;

namespace {

static ReachableObject SEED_SINGLETON{};

} // namespace

namespace reachability {

DexMethodRef* TransitiveClosureMarker::s_class_forname = nullptr;

std::ostream& operator<<(std::ostream& os, const ReachableObject& obj) {
  switch (obj.type) {
  case ReachableObjectType::ANNO:
    return os << show_deobfuscated(obj.anno->type());
  case ReachableObjectType::CLASS:
    return os << show_deobfuscated(obj.cls);
  case ReachableObjectType::FIELD:
    return os << show_deobfuscated(obj.field);
  case ReachableObjectType::METHOD:
    return os << show_deobfuscated(obj.method);
  case ReachableObjectType::SEED: {
    if (obj.keep_reason) {
      return os << *obj.keep_reason;
    }
    return os << "<SEED>";
  }
  }
}

bool RootSetMarker::is_canary(const DexClass* cls) {
  return strstr(cls->get_name()->c_str(), "Canary");
}

bool RootSetMarker::should_mark_cls(const DexClass* cls) {
  return root(cls) || is_canary(cls);
}

void RootSetMarker::mark_all_as_seed(const Scope& scope) {
  walk::parallel::classes(scope, [&](const DexClass* cls) {
    TRACE(REACH, 3, "Visiting seed: %s", SHOW(cls));
    push_seed(cls);

    for (auto const& f : cls->get_ifields()) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
      push_seed(f);
    }
    for (auto const& f : cls->get_sfields()) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
      push_seed(f);
    }
    for (auto const& m : cls->get_dmethods()) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(m));
      push_seed(m);
    }
    for (auto const& m : cls->get_vmethods()) {
      TRACE(REACH, 3, "Visiting seed: %s (root)", SHOW(m));
      push_seed(m);
    }
  });
}

/*
 * Initializes the root set by marking and pushing nodes onto the work queue.
 * Also conditionally marks class member seeds.
 */
void RootSetMarker::mark(const Scope& scope) {
  walk::parallel::classes(scope, [&](const DexClass* cls) {
    if (should_mark_cls(cls)) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(cls));
      push_seed(cls);
    }
    for (auto const& f : cls->get_ifields()) {
      if (root(f) || is_volatile(f)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
        push_seed(f);
      }
    }
    for (auto const& f : cls->get_sfields()) {
      if (root(f)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
        push_seed(f);
      }
    }
    for (auto const& m : cls->get_dmethods()) {
      if (root(m)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(m));
        push_seed(m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (root(m)) {
        TRACE(REACH, 3, "Visiting seed: %s (root)", SHOW(m));
        push_seed(m);
      }
    }
  });

  mark_external_method_overriders();
}

void RootSetMarker::push_seed(const DexClass* cls) {
  if (!cls) return;
  record_is_seed(cls);
  m_reachable_objects->mark(cls);
  m_root_set->emplace(cls);
}

void RootSetMarker::push_seed(const DexField* field) {
  if (!field) return;
  m_cond_marked->fields.insert(field);
}

void RootSetMarker::push_seed(const DexMethod* method) {
  if (!method) return;
  m_cond_marked->methods.insert(method);
}

template <class Seed>
void RootSetMarker::record_is_seed(Seed* seed) {
  if (m_record_reachability) {
    redex_assert(seed != nullptr);
    m_reachable_objects->record_is_seed(seed);
  }
}

/*
 * Mark as seeds all methods that override or implement an external method.
 */
void RootSetMarker::mark_external_method_overriders() {
  std::unordered_set<const DexMethod*> visited;
  for (auto& pair : m_method_override_graph.nodes()) {
    auto method = pair.first;
    if (!method->is_external() || visited.count(method)) {
      continue;
    }
    const auto& overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, method);
    for (auto* overriding : overriding_methods) {
      // Avoid re-visiting methods found in overriding sets since we would
      // already have conditionally marked all their children.
      visited.emplace(overriding);
      if (!overriding->is_external()) {
        TRACE(REACH, 3, "Visiting seed: %s (implements %s)", SHOW(overriding),
              SHOW(method));
        push_seed(overriding);
      }
    }
  }
}

/*
 * Marks :obj and pushes its immediately reachable neighbors onto the local
 * task queue of the current worker.
 */
void TransitiveClosureMarker::visit(const ReachableObject& obj) {
  switch (obj.type) {
  case ReachableObjectType::CLASS:
    visit_cls(obj.cls);
    break;
  case ReachableObjectType::FIELD:
    visit_field_ref(obj.field);
    break;
  case ReachableObjectType::METHOD:
    visit_method_ref(obj.method);
    break;
  case ReachableObjectType::ANNO:
  case ReachableObjectType::SEED:
    not_reached_log("Unexpected ReachableObject type");
  }
}

template <class Parent, class InputIt>
void TransitiveClosureMarker::push(const Parent* parent,
                                   InputIt begin,
                                   InputIt end) {
  for (auto it = begin; it != end; ++it) {
    push(parent, *it);
  }
}

template <class Parent>
void TransitiveClosureMarker::push(const Parent* parent, const DexType* type) {
  type = type::get_element_type_if_array(type);
  push(parent, type_class(type));
}

void TransitiveClosureMarker::push(const DexMethodRef* parent,
                                   const DexType* type) {
  type = type::get_element_type_if_array(type);
  push(parent, type_class(type));
}

template <class Parent>
void TransitiveClosureMarker::push(const Parent* parent, const DexClass* cls) {
  if (!cls) {
    return;
  }
  record_reachability(parent, cls);
  if (m_reachable_objects->marked(cls)) {
    return;
  }
  m_reachable_objects->mark(cls);
  m_worker_state->push_task(ReachableObject(cls));
}

template <class Parent>
void TransitiveClosureMarker::push(const Parent* parent,
                                   const DexFieldRef* field) {
  if (!field) {
    return;
  }
  record_reachability(parent, field);
  if (m_reachable_objects->marked(field)) {
    return;
  }
  auto f = field->as_def();
  if (f) {
    gather_and_push(f);
  }
  m_reachable_objects->mark(field);
  m_worker_state->push_task(ReachableObject(field));
}

template <class Parent>
void TransitiveClosureMarker::push(const Parent* parent,
                                   const DexMethodRef* method) {
  if (!method) {
    return;
  }

  record_reachability(parent, method);
  if (m_reachable_objects->marked(method)) {
    return;
  }
  m_reachable_objects->mark(method);
  m_worker_state->push_task(ReachableObject(method));
}

void TransitiveClosureMarker::push(const DexMethodRef* parent,
                                   const DexMethodRef* method) {
  this->template push<DexMethodRef>(parent, method);
}

void TransitiveClosureMarker::push_cond(const DexMethod* method) {
  if (!method || m_reachable_objects->marked(method)) return;
  TRACE(REACH, 4, "Conditionally marking method: %s", SHOW(method));
  auto clazz = type_class(method->get_class());
  m_cond_marked->methods.insert(method);
  // If :clazz has been marked, we cannot count on visit(DexClass*) to move
  // the conditionally-marked methods into the actually-marked ones -- we have
  // to do it ourselves. Note that we must do this check after adding :method
  // to m_cond_marked to avoid a race condition where we add to m_cond_marked
  // after visit(DexClass*) has finished moving its contents over to
  // m_reachable_objects.
  if (m_reachable_objects->marked(clazz)) {
    push(clazz, method);
  }
}

template <class T>
static References generic_gather(T t) {
  References refs;
  t->gather_strings(refs.strings);
  t->gather_types(refs.types);
  t->gather_fields(refs.fields);
  t->gather_methods(refs.methods);
  return refs;
}

References TransitiveClosureMarker::gather(const DexAnnotation* anno) const {
  return generic_gather(anno);
}

References TransitiveClosureMarker::gather(const DexMethod* method) const {
  return generic_gather(method);
}

References TransitiveClosureMarker::gather(const DexField* field) const {
  return generic_gather(field);
}

bool TransitiveClosureMarker::has_class_forname(DexMethod* meth) {
  auto code = meth->get_code();
  if (!code || !s_class_forname) {
    return false;
  }
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->has_method() && insn->get_method() == s_class_forname) {
      return true;
    }
  }
  return false;
}

void TransitiveClosureMarker::gather_and_push(DexMethod* meth) {
  auto* type = meth->get_class();
  auto* cls = type_class(type);
  auto refs = gather(meth);
  bool check_strings = m_ignore_sets.keep_class_in_string;
  if (!check_strings && !refs.strings.empty() && has_class_forname(meth)) {
    check_strings = true;
  }
  if (m_ignore_sets.string_literals.count(type)) {
    ++m_stats->num_ignore_check_strings;
    check_strings = false;
  }
  if (cls && check_strings) {
    for (const auto& ignore_anno_type : m_ignore_sets.string_literal_annos) {
      if (has_anno(cls, ignore_anno_type)) {
        ++m_stats->num_ignore_check_strings;
        check_strings = false;
        break;
      }
    }
  }
  if (check_strings) {
    push_typelike_strings(meth, refs.strings);
  }
  push(meth, refs.types.begin(), refs.types.end());
  push(meth, refs.fields.begin(), refs.fields.end());
  push(meth, refs.methods.begin(), refs.methods.end());
  for (auto* cond_meth : refs.cond_methods) {
    push_cond(cond_meth);
  }
}

template <typename T>
void TransitiveClosureMarker::gather_and_push(T t) {
  auto refs = gather(t);
  push_typelike_strings(t, refs.strings);
  push(t, refs.types.begin(), refs.types.end());
  push(t, refs.fields.begin(), refs.fields.end());
  push(t, refs.methods.begin(), refs.methods.end());
}

template <class Parent>
void TransitiveClosureMarker::push_typelike_strings(
    const Parent* parent, const std::vector<const DexString*>& strings) {
  for (auto const& str : strings) {
    auto internal = java_names::external_to_internal(str->c_str());
    auto type = DexType::get_type(internal.c_str());
    if (!type) {
      continue;
    }
    push(parent, type);
  }
}

void TransitiveClosureMarker::visit_cls(const DexClass* cls) {
  TRACE(REACH, 4, "Visiting class: %s", SHOW(cls));
  for (auto& m : cls->get_dmethods()) {
    if (method::is_clinit(m)) {
      push(cls, m);
    } else if (method::is_init(m)) {
      // Push the parameterless constructor, in case it's constructed via
      // .class or Class.forName()
      // if m_remove_no_argument_constructors, make an exception. This is only
      // used for testing
      if (!m_remove_no_argument_constructors &&
          m->get_proto()->get_args()->empty()) {
        push(cls, m);
      }
    }
  }
  push(cls, type_class(cls->get_super_class()));
  for (auto const& t : *cls->get_interfaces()) {
    push(cls, t);
  }
  const DexAnnotationSet* annoset = cls->get_anno_set();
  if (annoset) {
    for (auto const& anno : annoset->get_annotations()) {
      if (m_ignore_sets.system_annos.count(anno->type())) {
        TRACE(REACH,
              5,
              "Stop marking from %s by system anno: %s",
              SHOW(cls),
              SHOW(anno->type()));
        continue;
      }
      record_reachability(cls, anno.get());
      gather_and_push(anno.get());
    }
  }
  for (auto const& m : cls->get_ifields()) {
    if (m_cond_marked->fields.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_sfields()) {
    if (m_cond_marked->fields.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_dmethods()) {
    if (m_cond_marked->methods.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_vmethods()) {
    if (m_cond_marked->methods.count(m)) {
      push(cls, m);
    }
  }
}

void TransitiveClosureMarker::visit_field_ref(const DexFieldRef* field) {
  TRACE(REACH, 4, "Visiting field: %s", SHOW(field));
  if (!field->is_concrete()) {
    auto const& realfield =
        resolve_field(field->get_class(), field->get_name(), field->get_type());
    push(field, realfield);
  }
  push(field, field->get_class());
  push(field, field->get_type());
}

DexMethod* TransitiveClosureMarker::resolve_without_context(
    const DexMethodRef* method, const DexClass* cls) {
  if (!cls) return nullptr;
  for (auto const& m : cls->get_vmethods()) {
    if (method::signatures_match(method, m)) {
      return m;
    }
  }
  for (auto const& m : cls->get_dmethods()) {
    if (method::signatures_match(method, m)) {
      return m;
    }
  }
  {
    auto const& superclass = type_class(cls->get_super_class());
    auto const resolved = resolve_without_context(method, superclass);
    if (resolved) {
      return resolved;
    }
  }
  for (auto const& interface : *cls->get_interfaces()) {
    auto const resolved =
        resolve_without_context(method, type_class(interface));
    if (resolved) {
      return resolved;
    }
  }
  return nullptr;
}

void TransitiveClosureMarker::visit_method_ref(const DexMethodRef* method) {
  TRACE(REACH, 4, "Visiting method: %s", SHOW(method));
  auto resolved_method =
      resolve_without_context(method, type_class(method->get_class()));
  if (resolved_method != nullptr) {
    TRACE(REACH, 5, "    Resolved to: %s", SHOW(resolved_method));
    push(method, resolved_method);
    gather_and_push(resolved_method);
  }
  push(method, method->get_class());
  push(method, method->get_proto()->get_rtype());
  for (auto const& t : *method->get_proto()->get_args()) {
    push(method, t);
  }
  auto m = method->as_def();
  if (!m) {
    return;
  }
  // If we're keeping an interface or virtual method, we have to keep its
  // implementations and overriding methods respectively.
  if (m->is_virtual() || !m->is_concrete()) {
    const auto& overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, m);
    for (auto* overriding : overriding_methods) {
      push_cond(overriding);
    }
  }
}

template <class Parent, class Object>
void TransitiveClosureMarker::record_reachability(Parent* parent,
                                                  Object* object) {
  if (m_record_reachability) {
    redex_assert(parent != nullptr && object != nullptr);
    m_reachable_objects->record_reachability(parent, object);
  }
}

std::unique_ptr<ReachableObjects> compute_reachable_objects(
    const DexStoresVector& stores,
    const IgnoreSets& ignore_sets,
    int* num_ignore_check_strings,
    bool record_reachability,
    bool should_mark_all_as_seed,
    std::unique_ptr<const mog::Graph>* out_method_override_graph,
    bool remove_no_argument_constructors) {
  Timer t("Marking");
  auto scope = build_class_scope(stores);
  auto reachable_objects = std::make_unique<ReachableObjects>();
  ConditionallyMarked cond_marked;
  auto method_override_graph = mog::build_graph(scope);

  ConcurrentSet<ReachableObject, ReachableObjectHash> root_set;
  RootSetMarker root_set_marker(*method_override_graph,
                                record_reachability,
                                &cond_marked,
                                reachable_objects.get(),
                                &root_set);

  if (should_mark_all_as_seed) {
    root_set_marker.mark_all_as_seed(scope);
  } else {
    root_set_marker.mark(scope);
  }

  size_t num_threads = redex_parallel::default_num_threads();
  auto stats_arr = std::make_unique<Stats[]>(num_threads);
  workqueue_run<ReachableObject>(
      [&](MarkWorkerState* worker_state, const ReachableObject& obj) {
        TransitiveClosureMarker transitive_closure_marker(
            ignore_sets, *method_override_graph, record_reachability,
            &cond_marked, reachable_objects.get(), worker_state,
            &stats_arr[worker_state->worker_id()],
            remove_no_argument_constructors);
        transitive_closure_marker.visit(obj);
        return nullptr;
      },
      root_set,
      num_threads,
      /*push_tasks_while_running=*/true);

  if (num_ignore_check_strings != nullptr) {
    for (size_t i = 0; i < num_threads; ++i) {
      *num_ignore_check_strings += stats_arr[i].num_ignore_check_strings;
    }
  }

  if (out_method_override_graph) {
    *out_method_override_graph = std::move(method_override_graph);
  }

  return reachable_objects;
}

void ReachableObjects::record_reachability(const DexMethodRef* member,
                                           const DexClass* cls) {
  // Each class member trivially retains its containing class; let's filter out
  // this uninteresting information from our diagnostics.
  if (member->get_class() == cls->get_type()) {
    return;
  }
  m_retainers_of.update(ReachableObject(cls),
                        [&](const ReachableObject&, ReachableObjectSet& set,
                            bool /* exists */) { set.emplace(member); });
}

void ReachableObjects::record_reachability(const DexFieldRef* member,
                                           const DexClass* cls) {
  if (member->get_class() == cls->get_type()) {
    return;
  }
  m_retainers_of.update(ReachableObject(cls),
                        [&](const ReachableObject&, ReachableObjectSet& set,
                            bool /* exists */) { set.emplace(member); });
}

template <class Object>
void ReachableObjects::record_reachability(Object* parent, Object* object) {
  if (parent == object) {
    return;
  }
  m_retainers_of.update(ReachableObject(object),
                        [&](const ReachableObject&, ReachableObjectSet& set,
                            bool /* exists */) { set.emplace(parent); });
}

template <class Parent, class Object>
void ReachableObjects::record_reachability(Parent* parent, Object* object) {
  m_retainers_of.update(ReachableObject(object),
                        [&](const ReachableObject&, ReachableObjectSet& set,
                            bool /* exists */) { set.emplace(parent); });
}

template <class Seed>
void ReachableObjects::record_is_seed(Seed* seed) {
  redex_assert(seed != nullptr);
  const auto& keep_reasons = seed->rstate.keep_reasons();
  m_retainers_of.update(
      ReachableObject(seed),
      [&](const ReachableObject&, ReachableObjectSet& set, bool /* exists */) {
        for (const auto& reason : keep_reasons) {
          // -keepnames rules are irrelevant when analyzing reachability
          if (reason->type == keep_reason::KEEP_RULE &&
              reason->keep_rule->allowshrinking) {
            continue;
          }
          set.emplace(reason);
        }
      });
}

/*
 * Remove unmarked classes / methods / fields. and add all swept objects to
 * :removed_symbols.
 */
template <class Container, typename EraseHookFn>
static void sweep_if_unmarked(const ReachableObjects& reachables,
                              EraseHookFn erase_hook,
                              Container* c,
                              ConcurrentSet<std::string>* removed_symbols) {
  auto p = [&](const auto& m) {
    if (reachables.marked_unsafe(m) == 0) {
      TRACE(RMU, 2, "Removing %s", SHOW(m));
      return false;
    }
    return true;
  };
  const auto it = std::partition(c->begin(), c->end(), p);
  if (removed_symbols) {
    for (auto i = it; i != c->end(); i++) {
      removed_symbols->insert(show_deobfuscated(*i));
      erase_hook(*i);
    }
  } else {
    for (auto i = it; i != c->end(); i++) {
      erase_hook(*i);
    }
  }
  c->erase(it, c->end());
}

void sweep(DexStoresVector& stores,
           const ReachableObjects& reachables,
           ConcurrentSet<std::string>* removed_symbols) {
  Timer t("Sweep");
  auto scope = build_class_scope(stores);

  std::unordered_set<DexClass*> sweeped_classes;
  for (auto& dex : DexStoreClassesIterator(stores)) {
    sweep_if_unmarked(
        reachables, [&](auto cls) { sweeped_classes.insert(cls); }, &dex,
        removed_symbols);
  }

  auto sweep_method = [&](DexMethodRef* m) {
    if (m->is_def()) {
      m->as_def()->release_code(); // Free code.
    }
    DexMethod::erase_method(m);
  };

  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (sweeped_classes.count(cls)) {
      for (auto field : cls->get_all_fields()) {
        DexField::delete_field_DO_NOT_USE(field);
      }
      cls->get_ifields().clear();
      cls->get_sfields().clear();
      for (auto method : cls->get_all_methods()) {
        sweep_method(method);
      }
      cls->get_dmethods().clear();
      cls->get_vmethods().clear();
      return;
    }
    sweep_if_unmarked(reachables, DexField::delete_field_DO_NOT_USE,
                      &cls->get_ifields(), removed_symbols);
    sweep_if_unmarked(reachables, DexField::delete_field_DO_NOT_USE,
                      &cls->get_sfields(), removed_symbols);
    sweep_if_unmarked(reachables, sweep_method, &cls->get_dmethods(),
                      removed_symbols);
    sweep_if_unmarked(reachables, sweep_method, &cls->get_vmethods(),
                      removed_symbols);
  });
}

ObjectCounts count_objects(const DexStoresVector& stores) {
  ObjectCounts counts;
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    counts.num_classes += dex.size();
    for (auto const& cls : dex) {
      counts.num_fields += cls->get_ifields().size();
      counts.num_fields += cls->get_sfields().size();
      counts.num_methods += cls->get_dmethods().size();
      counts.num_methods += cls->get_vmethods().size();
    }
  }
  return counts;
}

// Graph serialization helpers
namespace {

void write_reachable_object(std::ostream& os, const ReachableObject& obj) {
  bs::write<uint8_t>(os, static_cast<uint8_t>(obj.type));
  std::ostringstream ss;
  ss << obj;
  const auto& s = ss.str();
  bs::write<uint32_t>(os, s.size());
  os << s;
}

} // namespace

void dump_graph(std::ostream& os, const ReachableObjectGraph& retainers_of) {
  auto compare = [](const ReachableObject& lhs, const ReachableObject& rhs) {
    if (lhs.type != rhs.type) {
      return lhs.type < rhs.type;
    }
    switch (lhs.type) {
    case ReachableObjectType::CLASS:
      return compare_dexclasses(lhs.cls, rhs.cls);
    case ReachableObjectType::FIELD:
      return compare_dexfields(lhs.field, rhs.field);
    case ReachableObjectType::METHOD:
      return compare_dexmethods(lhs.method, rhs.method);

    case ReachableObjectType::ANNO:
    case ReachableObjectType::SEED: {
      // Pretty terrible, optimize.
      std::ostringstream oss1;
      oss1 << lhs;
      std::ostringstream oss2;
      oss2 << rhs;
      return oss1.str() < oss2.str();
    }
    }
    __builtin_unreachable();
  };

  bs::write_header(os, /* version */ 1);
  bs::GraphWriter<ReachableObject, ReachableObjectHash> gw(
      write_reachable_object,
      [&](const ReachableObject& obj) -> std::vector<ReachableObject> {
        if (!retainers_of.count(obj)) {
          return {};
        }
        const auto& preds = retainers_of.at(obj);
        std::vector<ReachableObject> preds_vec(preds.begin(), preds.end());
        // Gotta sort the reachables or the output is nondeterministic.
        std::sort(preds_vec.begin(), preds_vec.end(), compare);
        return preds_vec;
      });

  // Gotta sort the keys or the output is nondeterministic.
  auto key_adaptor = boost::adaptors::keys(retainers_of);
  std::vector<ReachableObject> keys(key_adaptor.begin(), key_adaptor.end());
  std::sort(keys.begin(), keys.end(), compare);
  gw.write(os, keys);
}

template void TransitiveClosureMarker::push<DexClass>(const DexClass* parent,
                                                      const DexType* type);
} // namespace reachability
