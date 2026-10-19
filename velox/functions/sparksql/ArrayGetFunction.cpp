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

#include "velox/functions/lib/SubscriptUtil.h"

namespace facebook::velox::functions::sparksql {

namespace {

// get(array, index) and checked_get(array, index) -> array[index]
//
// Spark's GetArrayItem: `get` and `arr[i]` with ANSI mode disabled, or `arr[i]`
// with ANSI mode enabled. The index starts at 0.
// - get: returns NULL for a negative or out-of-bounds index.
// - checked_get: throws for a negative or out-of-bounds index.
template <bool failOnError>
class ArrayGetFunction : public SubscriptImpl<
                             /* allowNegativeIndices */ !failOnError,
                             /* nullOnNegativeIndices */ !failOnError,
                             /* allowOutOfBound */ !failOnError,
                             /* indexStartsAtOne */ false> {
 public:
  ArrayGetFunction()
      : SubscriptImpl<!failOnError, !failOnError, !failOnError, false>(false) {}

  static std::vector<std::shared_ptr<exec::FunctionSignature>> signatures() {
    std::vector<std::shared_ptr<exec::FunctionSignature>> signatures;

    // array(T), tinyint|smallint|integer|bigint -> T
    for (const auto& indexType : {"tinyint", "smallint", "integer", "bigint"}) {
      signatures.push_back(
          exec::FunctionSignatureBuilder()
              .typeVariable("T")
              .returnType("T")
              .argumentType("array(T)")
              .argumentType(indexType)
              .build());
    }

    return signatures;
  }
};
} // namespace

VELOX_DECLARE_VECTOR_FUNCTION(
    udf_array_get,
    ArrayGetFunction<false>::signatures(),
    std::make_unique<ArrayGetFunction<false>>());

VELOX_DECLARE_VECTOR_FUNCTION(
    udf_checked_array_get,
    ArrayGetFunction<true>::signatures(),
    std::make_unique<ArrayGetFunction<true>>());

} // namespace facebook::velox::functions::sparksql
