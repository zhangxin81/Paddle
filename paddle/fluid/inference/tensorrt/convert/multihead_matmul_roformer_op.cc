/* Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See
the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/inference/tensorrt/convert/op_converter.h"
#include "paddle/fluid/inference/tensorrt/plugin/multihead_matmul_roformer_plugin.h"

namespace paddle {
namespace inference {
namespace tensorrt {

class MultiheadMatMulRoformerOpConverter : public OpConverter {
 public:
  void operator()(const framework::proto::OpDesc& op,
                  const framework::Scope& scope,
                  bool test_mode) override {
    VLOG(3) << "convert a fluid multihead_mamul_roformer op to a corresponding "
               "tensorrt "
               "network structure";
    framework::OpDesc op_desc(op, nullptr);
    // Declare inputs
    auto* input = engine_->GetITensor(op_desc.Input("Input").front());
    int input_cos_len = op_desc.Input("Input_cos").front().length();
    std::string new_input_cos = op_desc.Input("Input_cos").front().substr(0, input_cos_len - 1);
    int input_sin_len = op_desc.Input("Input_sin").front().length();
    std::string new_input_sin = op_desc.Input("Input_sin").front().substr(0, input_sin_len - 1);

    auto* input_cos = engine_->GetITensor(new_input_cos);
    auto* input_sin = engine_->GetITensor(new_input_sin);
    // fc weights and fc bias
    auto weight_name = op_desc.Input("W").front();
    auto bias_name = op_desc.Input("Bias").front();

    auto* weight_v = scope.FindVar(weight_name);
    auto* weight_t = weight_v->GetMutable<phi::DenseTensor>();

    auto* bias_v = scope.FindVar(bias_name);
    auto* bias_t = bias_v->GetMutable<phi::DenseTensor>();

    float* weight_data = nullptr;
    float in_scale = 0.;

    if (op_desc.HasAttr("Input_scale")) {
      in_scale = PADDLE_GET_CONST(float, op_desc.GetAttr("Input_scale"));
      engine_->SetTensorDynamicRange(input, in_scale);
    }
    weight_data = const_cast<float*>(static_cast<const float*>(
        engine_->GetFp32TrtWeight(weight_name, *weight_t).get().values));

    float* bias_data = const_cast<float*>(static_cast<const float*>(
        engine_->GetFp32TrtWeight(bias_name, *bias_t).get().values));
    std::vector<float> weight_data_tmp;
    weight_data_tmp.reserve(weight_t->numel());
    memcpy(
        weight_data_tmp.data(), weight_data, weight_t->numel() * sizeof(float));

    // (hidden_in, 3, hidden_out)
    auto& weight_dims = weight_t->dims();

    int hidden_in = weight_dims[0];   // channels_in
    int three = weight_dims[1];       // channels_out
    int hidden_out = weight_dims[2];  // channels_out
    int m = hidden_in;
    int n = three * hidden_out;
    auto tranpose_weight = [](const float* src, float* dst, int m, int n) {
      for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
          dst[j * m + i] = src[i * n + j];
        }
      }
    };
    tranpose_weight(weight_data_tmp.data(), weight_data, m, n);

    int head_number = PADDLE_GET_CONST(int, op_desc.GetAttr("head_number"));

    nvinfer1::ILayer* layer = nullptr;
    auto output_name = op_desc.Output("Out")[0];
    bool flag_varseqlen = engine_->use_varseqlen() &&
                          engine_->tensorrt_transformer_posid() != "" &&
                          engine_->tensorrt_transformer_maskid() != "";

    if (engine_->with_dynamic_shape()) {
      if (flag_varseqlen) {
        if (engine_->precision() == AnalysisConfig::Precision::kFloat32) {
          PADDLE_THROW(platform::errors::Fatal(
              "use use_oss must be int8 or half, not float32."));
        }
	if (engine_->with_interleaved()) {
          PADDLE_THROW(platform::errors::Fatal(
              "not support interleaved in roformer yet"));
	}
	nvinfer1::Weights weight{nvinfer1::DataType::kFLOAT,
                                 static_cast<void*>(weight_data),
                                 static_cast<int32_t>(weight_t->numel())};
        nvinfer1::Weights bias{nvinfer1::DataType::kFLOAT,
                               static_cast<void*>(bias_data),
                               static_cast<int32_t>(bias_t->numel())};

	int head_size = hidden_out / head_number;
        // [3, head_number, head_size, hidden_in] -> [head_number, 3,
        // head_size, hidden_in]
        auto transpose_weight_v2 = [](const float* src,
                                      float* dst,
                                      int three,
                                      int head_number,
                                      int head_size,
                                      int hidden_in) {
          const int HH = head_size * hidden_in;
          for (int i = 0; i < three; ++i) {
            for (int n = 0; n < head_number; ++n) {
              for (int hh = 0; hh < HH; ++hh) {
                dst[n * three * HH + i * HH + hh] =
                    src[i * head_number * HH + n * HH + hh];
              }
            }
          }
        };
        // [3, head_number, head_size] -> [head_number, 3, head_size]
        auto transpose_bias_v2 =
            [](const float* src, float* dst, int N, int H) {
              for (int i = 0; i < 3; ++i) {
                for (int n = 0; n < N; ++n) {
                  for (int h = 0; h < H; ++h) {
                    dst[n * 3 * H + i * H + h] = src[i * N * H + n * H + h];
                  }
                }
              }
            };
        memcpy(weight_data_tmp.data(),
               weight_data,
               weight_t->numel() * sizeof(float));
        transpose_weight_v2(weight_data_tmp.data(),
                            weight_data,
                            three,
                            head_number,
                            head_size,
                            hidden_in);
        std::vector<float> bias_data_tmp;
        bias_data_tmp.reserve(bias_t->numel());
        memcpy(
            bias_data_tmp.data(), bias_data, bias_t->numel() * sizeof(float));
        transpose_bias_v2(
            bias_data_tmp.data(), bias_data, head_number, head_size);
        nvinfer1::ILayer* fc_layer = nullptr;
        float dp_probs = 1.0 / 127.0;
        if (op_desc.HasAttr("Input_scale")) {
          nvinfer1::DimsHW nv_ksize(1, 1);
          fc_layer = TRT_ENGINE_ADD_LAYER(
              engine_, Convolution, *input, n, nv_ksize, weight, bias);
        } else {
          fc_layer = TRT_ENGINE_ADD_LAYER(
              engine_, FullyConnected, *input, n, weight, bias);
        }
        if (op_desc.HasAttr("fc_out_threshold")) {
          PADDLE_ENFORCE_EQ(op_desc.HasAttr("fc_out_threshold"),
                            true,
                            platform::errors::InvalidArgument(
                                "must have out threshold in multihead layers "
                                "in int8 mode"));
          float out_scale =
              PADDLE_GET_CONST(float, op_desc.GetAttr("fc_out_threshold"));
          engine_->SetTensorDynamicRange(fc_layer->getOutput(0), out_scale);
          if (qkv2context_plugin_int8) {
            dp_probs =
                PADDLE_GET_CONST(float, op_desc.GetAttr("dp_probs")) / 127.0;
          }
        }
        // where to insert roformer kernel
        std::vector<nvinfer1::ITensor*> roformerplugin_inputs;
        roformerplugin_inputs.push_back(fc_layer->getOutput(0));
        roformerplugin_inputs.push_back(input_cos);
        roformerplugin_inputs.push_back(input_sin);
        plugin::DynamicPluginTensorRT* roformerplugin =
            new plugin::RoformerPlugin(
                "roformerplugin", head_number, head_size);
        auto roformerlayer = engine_->AddDynamicPlugin(
            roformerplugin_inputs.data(), 3, roformerplugin);
        roformerlayer->setName(
            ("roformerlayer(Output: " + output_name + ")").c_str());
        //roformerlayer->setPrecision(nvinfer1::DataType::kHALF);
        auto mask_tensor = engine_->GetITensor("qkv_plugin_mask");
        auto creator = GetPluginRegistry()->getPluginCreator(
            "CustomQKVToContextPluginDynamic", "2");
        assert(creator != nullptr);
        int type = static_cast<int>(nvinfer1::DataType::kHALF);
        if (qkv2context_plugin_int8 &&
            (engine_->precision() == AnalysisConfig::Precision::kInt8)) {
          type = static_cast<int>(nvinfer1::DataType::kINT8);
        }
        bool has_mask = true;
        int var_seqlen = 1;
        std::vector<nvinfer1::PluginField> fields{
            {"type_id", &type, nvinfer1::PluginFieldType::kINT32, 1},
            {"hidden_size",
             &hidden_out,
             nvinfer1::PluginFieldType::kINT32,
             1},
            {"num_heads", &head_number, nvinfer1::PluginFieldType::kINT32, 1},
            {"has_mask", &has_mask, nvinfer1::PluginFieldType::kINT32, 1},
            {"var_seqlen",
             &var_seqlen,
             nvinfer1::PluginFieldType::kINT32,
             1}};
        if (qkv2context_plugin_int8) {
          fields.push_back({"dq_probs",
                            &dp_probs,
                            nvinfer1::PluginFieldType::kFLOAT32,
                            1});
        }
        nvinfer1::PluginFieldCollection* plugin_collection =
            static_cast<nvinfer1::PluginFieldCollection*>(malloc(
                sizeof(*plugin_collection) +
                fields.size() *
                    sizeof(nvinfer1::PluginField)));  // remember to free
        plugin_collection->nbFields = static_cast<int>(fields.size());
        plugin_collection->fields = fields.data();
        auto plugin = creator->createPlugin("CustomQKVToContextPluginDynamic",
                                            plugin_collection);
        free(plugin_collection);
        std::vector<nvinfer1::ITensor*> plugin_inputs;
        // remeber to modify
        plugin_inputs.emplace_back(roformerlayer->getOutput(0));
        plugin_inputs.emplace_back(mask_tensor);
        if (engine_->Has("ernie_pos_name")) {
          plugin_inputs.emplace_back(engine_->GetITensor(
              engine_->Get<std::string>("ernie_pos_name")));
        } else {
          plugin_inputs.emplace_back(engine_->GetITensor(
              engine_->network()
                  ->getInput(2)
                  ->getName()));  // cu_seqlens, eval_placeholder_2
        }
        auto max_seqlen_tensor =
            engine_->GetITensor(engine_->network()->getInput(3)->getName());
        auto* shuffle_layer = TRT_ENGINE_ADD_LAYER(
            engine_,
            Shuffle,
            *const_cast<nvinfer1::ITensor*>(max_seqlen_tensor));
        nvinfer1::Dims shape_dim;
        shape_dim.nbDims = 1;
        shape_dim.d[0] = -1;
        shuffle_layer->setReshapeDimensions(shape_dim);
        engine_->SetTensorDynamicRange(shuffle_layer->getOutput(0), 1.0f);
        plugin_inputs.emplace_back(
            shuffle_layer->getOutput(0));  // max_seqlen, eval_placeholder_3
        auto plugin_layer = engine_->network()->addPluginV2(
            plugin_inputs.data(), plugin_inputs.size(), *plugin);
        layer = plugin_layer;

      } else { // no varlen
        PADDLE_ENFORCE_EQ(
            input->getDimensions().nbDims,
            3,
            platform::errors::InvalidArgument(
                "The Input dim of the MultiheadMatMul should be 3, "
                "but it's (%d) now.",
                input->getDimensions().nbDims));
        // transpose weight_data from m * n to  n * m
        auto* input_bias_qk =
            engine_->GetITensor(op_desc.Input("BiasQK").front());

        TensorRTEngine::Weight weight{nvinfer1::DataType::kFLOAT,
                                      static_cast<void*>(weight_data),
                                      static_cast<size_t>(weight_t->numel())};
        weight.dims.assign({n, m});

        TensorRTEngine::Weight bias{nvinfer1::DataType::kFLOAT,
                                    static_cast<void*>(bias_data),
                                    static_cast<size_t>(bias_t->numel())};
        // add shuffle before fc
        nvinfer1::Dims reshape_before_fc_dim;
        reshape_before_fc_dim.nbDims = 5;
        reshape_before_fc_dim.d[0] = 0;
        reshape_before_fc_dim.d[1] = 0;
        reshape_before_fc_dim.d[2] = 0;
        reshape_before_fc_dim.d[3] = 1;
        reshape_before_fc_dim.d[4] = 1;
        auto* reshape_before_fc_layer =
            TRT_ENGINE_ADD_LAYER(engine_, Shuffle, *input);
        if (op_desc.HasAttr("Input_scale")) {
          engine_->SetTensorDynamicRange(reshape_before_fc_layer->getOutput(0),
                                         in_scale);
        }
        reshape_before_fc_layer->setReshapeDimensions(reshape_before_fc_dim);
        reshape_before_fc_layer->setName(
            ("shuffle_before_multihead_matmul(Output: " + output_name + ")")
                .c_str());

        // add layer fc
        nvinfer1::ILayer* fc_layer = nullptr;
        if (op_desc.HasAttr("Input_scale")) {
          nvinfer1::DimsHW nv_ksize(1, 1);
          fc_layer =
              TRT_ENGINE_ADD_LAYER(engine_,
                                   Convolution,
                                   *reshape_before_fc_layer->getOutput(0),
                                   n,
                                   nv_ksize,
                                   weight.get(),
                                   bias.get());
        } else {
          fc_layer =
              TRT_ENGINE_ADD_LAYER(engine_,
                                   FullyConnected,
                                   *reshape_before_fc_layer->getOutput(0),
                                   n,
                                   weight.get(),
                                   bias.get());
        }

        if (op_desc.HasAttr("fc_out_threshold")) {
          PADDLE_ENFORCE_EQ(
              op_desc.HasAttr("fc_out_threshold"),
              true,
              platform::errors::InvalidArgument(
                  "must have out threshold in multihead layers in int8 mode"));
          float out_scale =
              PADDLE_GET_CONST(float, op_desc.GetAttr("fc_out_threshold"));
          engine_->SetTensorDynamicRange(fc_layer->getOutput(0), out_scale);
        }
        fc_layer->setName(
            ("multihead_matmul_fc(Output: " + output_name + ")").c_str());

        // no need to add shuffle after fc, just change it in
        // QkvToContextPluginDynamic

        // add qkv to context
        int head_size = hidden_out / head_number;
        float scale = PADDLE_GET_CONST(float, op_desc.GetAttr("alpha"));

        std::vector<nvinfer1::ITensor*> plugin_inputs;
        plugin_inputs.push_back(fc_layer->getOutput(0));
        plugin_inputs.push_back(input_cos);
        plugin_inputs.push_back(input_sin);
        plugin_inputs.push_back(input_bias_qk);
        bool with_fp16 =
            engine_->WithFp16() && !engine_->disable_trt_plugin_fp16();

        if (engine_->precision() == AnalysisConfig::Precision::kInt8) {
          with_fp16 = true;
        }
        plugin::DynamicPluginTensorRT* plugin =
            new plugin::MultiheadMatmulRoformerPlugin(
                hidden_in, head_number, head_size, scale, with_fp16);
        layer = engine_->AddDynamicPlugin(plugin_inputs.data(), 4, plugin);
      }
    } else {
      PADDLE_THROW(platform::errors::Fatal(
          "You are running the Ernie(Bert) model in static shape mode, which "
          "is not supported for the time being.\n"
          "You can use the config.SetTRTDynamicShapeInfo(...) interface to set "
          "the shape information to run the dynamic shape mode."));
    }
    RreplenishLayerAndOutput(
        layer, "multihead_matmul_roformer", {output_name}, test_mode);
  }
};

}  // namespace tensorrt
}  // namespace inference
}  // namespace paddle

REGISTER_TRT_OP_CONVERTER(multihead_matmul_roformer,
                          MultiheadMatMulRoformerOpConverter);
