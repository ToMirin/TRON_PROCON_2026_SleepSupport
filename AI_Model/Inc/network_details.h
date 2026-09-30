/**
  ******************************************************************************
  * @file    network.h
  * @date    2026-09-17T21:41:18+0900
  * @brief   ST.AI Tool Automatic Code Generator for Embedded NN computing
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */
#ifndef STAI_NETWORK_DETAILS_H
#define STAI_NETWORK_DETAILS_H

#include "stai.h"
#include "layers.h"

const stai_network_details g_network_details = {
  .tensors = (const stai_tensor[7]) {
   { .size_bytes = 3920, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_S8, .shape = {4, (const int32_t[4]){1, 98, 40, 1}}, .scale = {1, (const float[1]){0.021447716280817986}}, .zeropoint = {1, (const int16_t[1]){2}}, .name = "serving_default_input_layer0_output" },
   { .size_bytes = 7840, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_S8, .shape = {4, (const int32_t[4]){1, 49, 20, 8}}, .scale = {1, (const float[1]){0.03679845109581947}}, .zeropoint = {1, (const int16_t[1]){-128}}, .name = "conv2d_0_output" },
   { .size_bytes = 3840, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_S8, .shape = {4, (const int32_t[4]){1, 24, 10, 16}}, .scale = {1, (const float[1]){0.051009561866521835}}, .zeropoint = {1, (const int16_t[1]){-128}}, .name = "conv2d_2_output" },
   { .size_bytes = 4992, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_S8, .shape = {4, (const int32_t[4]){1, 26, 12, 16}}, .scale = {1, (const float[1]){0.051009561866521835}}, .zeropoint = {1, (const int16_t[1]){-128}}, .name = "conv2d_4_pad_before_output" },
   { .size_bytes = 32, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_S8, .shape = {4, (const int32_t[4]){1, 1, 1, 32}}, .scale = {1, (const float[1]){0.013073241338133812}}, .zeropoint = {1, (const int16_t[1]){-128}}, .name = "conv2d_4_output" },
   { .size_bytes = 3, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_S8, .shape = {2, (const int32_t[2]){1, 3}}, .scale = {1, (const float[1]){0.10693375766277313}}, .zeropoint = {1, (const int16_t[1]){16}}, .name = "gemm_6_output" },
   { .size_bytes = 3, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_S8, .shape = {2, (const int32_t[2]){1, 3}}, .scale = {1, (const float[1]){0.00390625}}, .zeropoint = {1, (const int16_t[1]){-128}}, .name = "nl_7_output" }
  },
  .nodes = (const stai_node_details[6]){
    {.id = 1, .type = AI_LAYER_OPTIMIZED_CONV2D_TYPE, .input_tensors = {1, (const int32_t[1]){0}}, .output_tensors = {1, (const int32_t[1]){1}} }, /* conv2d_0 */
    {.id = 3, .type = AI_LAYER_OPTIMIZED_CONV2D_TYPE, .input_tensors = {1, (const int32_t[1]){1}}, .output_tensors = {1, (const int32_t[1]){2}} }, /* conv2d_2 */
    {.id = 4, .type = AI_LAYER_PAD_TYPE, .input_tensors = {1, (const int32_t[1]){2}}, .output_tensors = {1, (const int32_t[1]){3}} }, /* conv2d_4_pad_before */
    {.id = 5, .type = AI_LAYER_OPTIMIZED_CONV2D_TYPE, .input_tensors = {1, (const int32_t[1]){3}}, .output_tensors = {1, (const int32_t[1]){4}} }, /* conv2d_4 */
    {.id = 6, .type = AI_LAYER_DENSE_TYPE, .input_tensors = {1, (const int32_t[1]){4}}, .output_tensors = {1, (const int32_t[1]){5}} }, /* gemm_6 */
    {.id = 7, .type = AI_LAYER_SM_TYPE, .input_tensors = {1, (const int32_t[1]){5}}, .output_tensors = {1, (const int32_t[1]){6}} } /* nl_7 */
  },
  .n_nodes = 6
};
#endif

