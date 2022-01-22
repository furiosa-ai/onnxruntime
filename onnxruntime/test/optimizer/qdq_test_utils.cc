// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "graph_transform_test_builder.h"

#include "core/optimizer/qdq_transformer/selectors_actions/qdq_selector_action_transformer.h"
#include "core/session/inference_session.h"

#include "test/util/include/asserts.h"
#include "test/util/include/inference_session_wrapper.h"

namespace onnxruntime {
namespace test {

using BuildTestCaseFn = std::function<void(ModelTestBuilder& builder)>;
using CheckTestGraphFn = std::function<void(InferenceSessionWrapper& session)>;

template <typename T>
typename std::enable_if<IsTypeQuantLinearCompatible<T>::value, NodeArg*>::type
AddQDQNodePair(ModelTestBuilder& builder, NodeArg* q_input, float scale, T zp) {
  auto* q_output = builder.MakeIntermediate();
  auto* dq_output = builder.MakeIntermediate();
  builder.AddQuantizeLinearNode<T>(q_input, scale, zp, q_output);
  builder.AddDequantizeLinearNode<T>(q_output, scale, zp, dq_output);
  return dq_output;
}

template <typename T>
typename std::enable_if<IsTypeQuantLinearCompatible<T>::value, NodeArg*>::type
AddQDQNodePair(ModelTestBuilder& builder, NodeArg* q_input, float scale) {
  auto* q_output = builder.MakeIntermediate();
  auto* dq_output = builder.MakeIntermediate();
  builder.AddQuantizeLinearNode(q_input, scale, q_output);
  builder.AddDequantizeLinearNode<T>(q_output, scale, dq_output);
  return dq_output;
}

// TODO: for now it just builds a conv qdq graph. 
// can be modified and made it shared among different qdq test graphs associated with other operators
template <typename InputType, typename WeightType, typename BiasType, typename OutputType>
BuildTestCaseFn BuildTestCase(const std::vector<int64_t>& input_shape, const std::vector<int64_t>& weights_shape) {
  return [&](ModelTestBuilder& builder) {
    auto* input_arg = builder.MakeInput<float>(input_shape, -1.f, 1.f);
    auto* output_arg = builder.MakeOutput();

    typedef std::numeric_limits<InputType> InputLimits;
    typedef std::numeric_limits<WeightType> WeightLimits;
    typedef std::numeric_limits<OutputType> OutputLimits;

    InputType input_min_value = InputLimits::min();
    InputType input_max_value = InputLimits::max();

    WeightType weight_min_value = WeightLimits::min();
    WeightType weight_max_value = WeightLimits::max();
    if (std::is_same<WeightType, int8_t>::value) {
      weight_min_value /= 2;
      weight_max_value /= 2;
    }

    auto* dq_w_output = builder.MakeIntermediate();
    auto* weight = builder.MakeInitializer<WeightType>(weights_shape, weight_min_value, weight_max_value);
    builder.AddDequantizeLinearNode<WeightType>(weight, .03f,
                                                (weight_min_value + weight_max_value) / 2 + 1,
                                                dq_w_output);

    auto* dq_bias_output = builder.MakeIntermediate();
    auto* bias = builder.MakeInitializer<BiasType>({weights_shape[0]}, static_cast<BiasType>(0), static_cast<BiasType>(127));
    builder.AddDequantizeLinearNode<BiasType>(bias, .0012f,
                                              0,
                                              dq_bias_output);

    auto* conv_output = builder.MakeIntermediate();
    auto* dq_output = AddQDQNodePair<InputType>(builder, input_arg, .04f,
                                                (input_min_value + input_max_value) / 2 + 1);
    builder.AddNode("Conv", {dq_output, dq_w_output, dq_bias_output}, {conv_output});

    auto* q_output = builder.MakeIntermediate();
    builder.AddQuantizeLinearNode<OutputType>(conv_output, .039f,
                                              (OutputLimits::min() + OutputLimits::max()) / 2 + 1,
                                              q_output);

    builder.AddDequantizeLinearNode<OutputType>(q_output, .039f,
                                                (OutputLimits::min() + OutputLimits::max()) / 2 + 1,
                                                output_arg);
  };
}

template <typename InputType, typename WeightType, typename BiasType, typename OutputType>
CheckTestGraphFn CheckTestGraph() {
  return [&](InferenceSessionWrapper& session) {
    auto op_to_count = CountOpsInGraph(session.GetGraph());
    if constexpr (std::is_same<InputType, OutputType>::value &&
                  std::is_same<BiasType, int32_t>::value &&
                  (std::is_same<InputType, uint8_t>::value ||
                   QDQIsInt8Allowed() && std::is_same<WeightType, int8_t>::value)) {
      EXPECT_EQ(op_to_count["QLinearConv"], 1);
      EXPECT_EQ(op_to_count["QuantizeLinear"], 1);
      EXPECT_EQ(op_to_count["DequantizeLinear"], 1);
    } else {
      EXPECT_EQ(op_to_count["Conv"], 1);
      EXPECT_EQ(op_to_count["QLinearConv"], 0);
      EXPECT_EQ(op_to_count["QuantizeLinear"], 2);
      EXPECT_EQ(op_to_count["DequantizeLinear"], 4);
    }
  };
}

}  // namespace test
}  // namespace onnxruntime