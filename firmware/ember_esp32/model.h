// Placeholder — overwritten by ml/train.py. MODEL_READY 0 -> firmware uses the fallback rules.
#pragma once
#define FEAT_N 5
#define CLASS_N 3
#define MODEL_READY 0
static const float FEAT_MEAN[FEAT_N]  = {0, 0, 0, 0, 0};
static const float FEAT_SCALE[FEAT_N] = {1, 1, 1, 1, 1};
static const float W[CLASS_N][FEAT_N] = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}};
static const float B[CLASS_N] = {0, 0, 0};
