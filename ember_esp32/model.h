// model.h — PLACEHOLDER. train.py overwrites this file with the trained model.
// Class order: normal, leak, fire. Feature order:
// gas_delta, gas_slope, temp_slope, light_delta, flicker
#pragma once
#define FEAT_N 5
#define CLASS_N 3
#define MODEL_READY 0          // 0 -> the sketch uses the fallback rules (mode = "rules")

const float FEAT_MEAN[FEAT_N]  = {0, 0, 0, 0, 0};
const float FEAT_SCALE[FEAT_N] = {1, 1, 1, 1, 1};
const float W[CLASS_N][FEAT_N] = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}};
const float B[CLASS_N]         = {0, 0, 0};
