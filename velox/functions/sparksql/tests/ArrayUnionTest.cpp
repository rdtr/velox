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
#include <bit>

#include "velox/functions/sparksql/tests/SparkFunctionBaseTest.h"

using namespace facebook::velox::test;

namespace facebook::velox::functions::sparksql::test {
namespace {

class ArrayUnionTest : public SparkFunctionBaseTest {
 protected:
  static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  // A NaN that is not the canonical NaN.
  static inline const double kOtherNaN =
      std::bit_cast<double>(0x7ff8000000000001ULL);

  void testExpression(
      const std::string& expression,
      const std::vector<VectorPtr>& input,
      const VectorPtr& expected) {
    auto result = evaluate(expression, makeRowVector(input));
    assertEqualVectors(expected, result);
    // assertEqualVectors treats -0.0 and 0.0 as equal, so check the bits.
    for (vector_size_t row = 0; row < result->size(); ++row) {
      expectCanonical(*result, row);
    }
  }

  template <typename T>
  static void expectCanonical(T value) {
    T expected = value == 0 ? 0 : value;
    if (std::isnan(value)) {
      expected = std::numeric_limits<T>::quiet_NaN();
    }
    EXPECT_EQ(std::memcmp(&value, &expected, sizeof(T)), 0)
        << "Not canonical: " << value;
  }

  // Checks that the REAL and DOUBLE values at 'row', including nested ones,
  // are in canonical form.
  static void expectCanonical(const BaseVector& vector, vector_size_t row) {
    DecodedVector decoded(vector);
    if (decoded.isNullAt(row)) {
      return;
    }
    const auto* base = decoded.base();
    const auto index = decoded.index(row);
    switch (base->typeKind()) {
      case TypeKind::REAL:
        expectCanonical(base->as<SimpleVector<float>>()->valueAt(index));
        break;
      case TypeKind::DOUBLE:
        expectCanonical(base->as<SimpleVector<double>>()->valueAt(index));
        break;
      case TypeKind::ARRAY: {
        const auto* array = base->as<ArrayVector>();
        for (auto i = 0; i < array->sizeAt(index); ++i) {
          expectCanonical(*array->elements(), array->offsetAt(index) + i);
        }
        break;
      }
      case TypeKind::ROW:
        for (const auto& child : base->as<RowVector>()->children()) {
          expectCanonical(*child, index);
        }
        break;
      default:
        break;
    }
  }
};

TEST_F(ArrayUnionTest, intArray) {
  const auto array1 = makeArrayVector<int64_t>(
      {{1, 2, 3, 4}, {3, 4, 5}, {7, 8, 9}, {10, 20, 30}, {}});
  const auto array2 =
      makeArrayVector<int64_t>({{2, 4, 5}, {3, 4, 5}, {}, {40, 50}, {}});

  testExpression(
      "array_union(c0, c1)",
      {array1, array2},
      makeArrayVector<int64_t>({
          {1, 2, 3, 4, 5},
          {3, 4, 5},
          {7, 8, 9},
          {10, 20, 30, 40, 50},
          {},
      }));
  testExpression(
      "array_union(c0, c1)",
      {array2, array1},
      makeArrayVector<int64_t>({
          {2, 4, 5, 1, 3},
          {3, 4, 5},
          {7, 8, 9},
          {40, 50, 10, 20, 30},
          {},
      }));
}

TEST_F(ArrayUnionTest, stringArray) {
  testExpression(
      "array_union(c0, c1)",
      {makeArrayVector<StringView>(
           {{"foo", "bar"}, {"foo", "a long string that is not inlined"}}),
       makeArrayVector<StringView>(
           {{"foo", "bar"}, {"a long string that is not inlined", "baz"}})},
      makeArrayVector<StringView>({
          {"foo", "bar"},
          {"foo", "a long string that is not inlined", "baz"},
      }));
}

TEST_F(ArrayUnionTest, nulls) {
  const auto array1 = makeNullableArrayVector<int64_t>({
      {{1, std::nullopt, 3, 4}},
      {7, 8, 9},
      {{10, std::nullopt, std::nullopt}},
  });
  const auto array2 = makeNullableArrayVector<int64_t>({
      {{std::nullopt, std::nullopt, 3, 5}},
      std::nullopt,
      {{1, 10}},
  });

  testExpression(
      "array_union(c0, c1)",
      {array1, array2},
      makeNullableArrayVector<int64_t>({
          {{1, std::nullopt, 3, 4, 5}},
          std::nullopt,
          {{10, std::nullopt, 1}},
      }));
  testExpression(
      "array_union(c0, c1)",
      {array2, array1},
      makeNullableArrayVector<int64_t>({
          {{std::nullopt, 3, 5, 1, 4}},
          std::nullopt,
          {{1, 10, std::nullopt}},
      }));
}

TEST_F(ArrayUnionTest, encodings) {
  // Dictionary-encoded inputs that reference the same array more than once.
  auto base = makeArrayVector<int64_t>({{1, 2}, {2, 3}});
  auto left = wrapInDictionary(makeIndices({0, 1, 0}), base);
  auto right = wrapInDictionary(makeIndices({1, 1, 0}), base);
  testExpression(
      "array_union(c0, c1)",
      {left, right},
      makeArrayVector<int64_t>({{1, 2, 3}, {2, 3}, {1, 2}}));

  // Constant input.
  testExpression(
      "array_union(c0, c1)",
      {BaseVector::wrapInConstant(3, 1, base), left},
      makeArrayVector<int64_t>({{2, 3, 1}, {2, 3}, {2, 3, 1}}));
}

TEST_F(ArrayUnionTest, unknownType) {
  // [null], [null, null]
  auto array = makeArrayVector(
      {0, 1}, BaseVector::createNullConstant(UNKNOWN(), 3, pool()));
  testExpression(
      "array_union(c0, c1)",
      {array, array},
      makeArrayVector(
          {0, 1}, BaseVector::createNullConstant(UNKNOWN(), 2, pool())));
}

TEST_F(ArrayUnionTest, complexTypes) {
  auto baseVector = makeArrayVector<int64_t>(
      {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}});
  // [[1, 1], [2, 2]], [[3, 3], [4, 4]], [[5, 5], [6, 6]]
  auto arrayOfArrays1 = makeArrayVector({0, 2, 4}, baseVector);
  // [[1, 1], [2, 2], [3, 3]], [[4, 4]], [[5, 5], [6, 6]]
  auto arrayOfArrays2 = makeArrayVector({0, 3, 4}, baseVector);

  testExpression(
      "array_union(c0, c1)",
      {arrayOfArrays1, arrayOfArrays2},
      makeArrayVector(
          {0, 3, 5},
          makeArrayVector<int64_t>(
              {{1, 1}, {2, 2}, {3, 3}, {3, 3}, {4, 4}, {5, 5}, {6, 6}})));
}

TEST_F(ArrayUnionTest, floatingPoint) {
  // -0.0 and 0.0 are equal, and so are all NaNs. The result has 0.0 and the
  // canonical NaN.
  testExpression(
      "array_union(c0, c1)",
      {makeArrayVector<double>({{-0.0, 1.0}, {kOtherNaN}, {1.5, kNaN}, {-0.0}}),
       makeArrayVector<double>({{0.0, kNaN}, {-0.0}, {kNaN, 2.5}, {}})},
      makeArrayVector<double>(
          {{0.0, 1.0, kNaN}, {kNaN, 0.0}, {1.5, kNaN, 2.5}, {0.0}}));

  testExpression(
      "array_union(c0, c1)",
      {makeArrayVector<float>({{-0.0f, std::nanf("")}}),
       makeArrayVector<float>({{0.0f}})},
      makeArrayVector<float>({{0.0f, std::nanf("")}}));
}

TEST_F(ArrayUnionTest, nestedFloatingPoint) {
  // [[-0.0]], [[1.0, -0.0]]
  auto left =
      makeArrayVector({0, 1}, makeArrayVector<double>({{-0.0}, {1.0, -0.0}}));
  // [[0.0]], [[2.0]]
  auto right = makeArrayVector({0, 1}, makeArrayVector<double>({{0.0}, {2.0}}));
  testExpression(
      "array_union(c0, c1)",
      {left, right},
      makeArrayVector(
          {0, 1}, makeArrayVector<double>({{0.0}, {1.0, 0.0}, {2.0}})));

  // [{-0.0, 1}], [{0.0, 1}, {-0.0, 2}]
  auto structs = makeRowVector({
      makeFlatVector<double>({-0.0, 0.0, -0.0}),
      makeFlatVector<int32_t>({1, 1, 2}),
  });
  testExpression(
      "array_union(c0, c1)",
      {makeArrayVector({0}, structs->slice(0, 1)),
       makeArrayVector({0}, structs->slice(1, 2))},
      makeArrayVector(
          {0},
          makeRowVector({
              makeFlatVector<double>({0.0, 0.0}),
              makeFlatVector<int32_t>({1, 2}),
          })));
}

TEST_F(ArrayUnionTest, nestedNull) {
  const auto array1 = makeNestedArrayVectorFromJson<int32_t>({
      "[[1], [null]]",
  });
  const auto array2 = makeNestedArrayVectorFromJson<int32_t>({
      "[[2], [null]]",
  });

  auto expected = makeNestedArrayVectorFromJson<int32_t>({
      "[[1], [null], [2]]",
  });

  testExpression("array_union(c0, c1)", {array1, array2}, expected);
}

} // namespace
} // namespace facebook::velox::functions::sparksql::test
