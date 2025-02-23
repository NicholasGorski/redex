/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>

#include "Pass.h"

class DexMethod;

namespace instrument {

enum class ProfileTypeFlags {
  NotSpecified = 0,
  MethodCallCount = 1,
  MethodCallOrder = 2,
  BlockCoverage = 4,
  SimpleMethodTracing = 1 | 2,
  BasicBlockTracing = 1 | 2 | 4,
};

class InstrumentPass : public Pass {
 public:
  InstrumentPass() : Pass("InstrumentPass") {}

  void bind_config() override;
  void eval_pass(DexStoresVector& stores,
                 ConfigFiles& conf,
                 PassManager& mgr) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  // Helper functions for both method and block instrumentations.
  //
  constexpr static const char* STATS_FIELD_NAME = "sMethodStats";

  static void patch_array_size(DexClass* analysis_cls,
                               const std::string_view array_name,
                               const int array_size);
  static void patch_static_field(DexClass* analysis_cls,
                                 const std::string_view field_name,
                                 const int new_number);
  static bool is_included(const DexMethod* method,
                          const std::unordered_set<std::string>& set);

  static std::unordered_map<int /*shard_num*/, DexFieldRef*>
  patch_sharded_arrays(
      DexClass* cls,
      const size_t num_shards,
      const std::map<int /*shard_num*/, std::string>& suggested_names = {});

  static std::pair<std::unordered_map<int /*shard_num*/, DexMethod*>,
                   std::unordered_set<std::string>>
  generate_sharded_analysis_methods(
      DexClass* cls,
      const std::string& template_method_full_name,
      const std::unordered_map<int /*shard_num*/, DexFieldRef*>& array_fields,
      const size_t num_shards);

  struct Options {
    std::string instrumentation_strategy;
    std::string analysis_class_name;
    std::string analysis_method_name;
    std::unordered_set<std::string> blocklist;
    std::unordered_set<std::string> allowlist;
    std::string blocklist_file_name;
    std::string metadata_file_name;
    int64_t num_stats_per_method;
    int64_t num_shards;
    bool only_cold_start_class;
    std::unordered_map<DexMethod*, DexMethod*> methods_replacement;
    std::vector<std::string> analysis_method_names;
    int64_t max_num_blocks;
    size_t shrink_defer_reg_threshold{0};
    bool instrument_catches;
    bool instrument_blocks_without_source_block;
    bool instrument_only_root_store;
  };

 private:
  Options m_options;
};

} // namespace instrument
