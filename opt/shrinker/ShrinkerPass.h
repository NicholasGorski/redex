/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "ShrinkerConfig.h"

class ShrinkerPass : public Pass {
 public:
  ShrinkerPass() : Pass("ShrinkerPass") {}

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  shrinker::ShrinkerConfig m_config;
};
