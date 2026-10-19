/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/functions/sparksql/aggregates/CollectListAggregate.h"

#include "velox/exec/SimpleAggregateAdapter.h"
#include "velox/functions/lib/aggregates/ValueList.h"
#include "velox/vector/ConstantVector.h"

using namespace facebook::velox::aggregate;
using namespace facebook::velox::exec;

namespace facebook::velox::functions::aggregate::sparksql {

namespace {
class CollectListAggregate {
 public:
  using InputType = Row<Generic<T1>>;

  using IntermediateType = Array<Generic<T1>>;

  using OutputType = Array<Generic<T1>>;

  /// In Spark, when all inputs are null, the output is an empty array instead
  /// of null. Therefore, in the writeIntermediateResult and writeFinalResult,
  /// we still need to output the empty element_ when the group is null. This
  /// behavior can only be achieved when the default-null behavior is disabled.
  static constexpr bool default_null_behavior_ = false;

  // Retains one element for each non-null input row.
  static constexpr bool is_reducing_ = false;

  // Whether null input values should be ignored. Defaults to true, Spark's
  // default. Set from the constant ignoreNulls argument in setConstantInputs().
  // NOTE: toIntermediate() was intentionally removed because it is static and
  // cannot access the runtime ignoreNulls_ flag. Without it, partial
  // aggregation uses the accumulator path, which correctly respects the flag.
  bool ignoreNulls_{true};

  void setConstantInputs(const std::vector<VectorPtr>& constantInputs) {
    // Only addInput() reads the flag. combine() keeps every element of the
    // intermediate arrays, so steps that see only intermediate input do not
    // need the argument and keep the default.
    if (constantInputs.size() >= 2 && constantInputs[1] != nullptr &&
        !constantInputs[1]->isNullAt(0)) {
      ignoreNulls_ = constantInputs[1]->as<ConstantVector<bool>>()->valueAt(0);
    }
  }

  struct AccumulatorType {
    ValueList elements_;

    explicit AccumulatorType(
        HashStringAllocator* /*allocator*/,
        CollectListAggregate* fn)
        : elements_{}, ignoreNulls_(fn->ignoreNulls_) {}

    static constexpr bool is_fixed_size_ = false;

    bool addInput(
        HashStringAllocator* allocator,
        exec::optional_arg_type<Generic<T1>> data) {
      if (data.has_value()) {
        elements_.appendValue(data, allocator);
        return true;
      }
      if (!ignoreNulls_) {
        elements_.appendValue(data, allocator);
        return true;
      }
      return false;
    }

    bool ignoreNulls_;

    bool combine(
        HashStringAllocator* allocator,
        exec::optional_arg_type<IntermediateType> other) {
      if (!other.has_value()) {
        return false;
      }
      for (auto element : other.value()) {
        elements_.appendValue(element, allocator);
      }
      return true;
    }

    bool writeIntermediateResult(
        bool /*nonNullGroup*/,
        exec::out_type<IntermediateType>& out) {
      // If the group's accumulator is null, the corresponding intermediate
      // result is an empty array.
      copyValueListToArrayWriter(out, elements_);
      return true;
    }

    bool writeFinalResult(
        bool /*nonNullGroup*/,
        exec::out_type<OutputType>& out) {
      // If the group's accumulator is null, the corresponding result is an
      // empty array.
      copyValueListToArrayWriter(out, elements_);
      return true;
    }

    void destroy(HashStringAllocator* allocator) {
      elements_.free(allocator);
    }
  };
};

} // namespace

void registerCollectListAggregate(
    const std::vector<std::string>& names,
    bool withCompanionFunctions,
    bool overwrite) {
  std::vector<std::shared_ptr<exec::AggregateFunctionSignature>> signatures{
      // collect_list(E) -> array(E): ignores nulls (Spark default).
      exec::AggregateFunctionSignatureBuilder()
          .typeVariable("E")
          .returnType("array(E)")
          .intermediateType("array(E)")
          .argumentType("E")
          .build(),
      // collect_list(E, ignoreNulls) -> array(E): explicit ignoreNulls flag.
      exec::AggregateFunctionSignatureBuilder()
          .typeVariable("E")
          .returnType("array(E)")
          .intermediateType("array(E)")
          .argumentType("E")
          .constantArgumentType("boolean")
          .build()};
  exec::registerAggregateFunction(
      names,
      std::move(signatures),
      [names](
          core::AggregationNode::Step step,
          const std::vector<TypePtr>& argTypes,
          const TypePtr& resultType,
          const core::QueryConfig& config) -> std::unique_ptr<exec::Aggregate> {
        const std::string& name = names.front();
        VELOX_CHECK_LE(
            argTypes.size(), 2, "{} takes at most two arguments", name);
        return std::make_unique<SimpleAggregateAdapter<CollectListAggregate>>(
            step, argTypes, resultType, &config);
      },
      withCompanionFunctions,
      overwrite);
}
} // namespace facebook::velox::functions::aggregate::sparksql
