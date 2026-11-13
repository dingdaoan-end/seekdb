#pragma once
#include <cstddef>
#include <cstdint>
namespace mvdb {
struct L2PruneCounters {
  uint64_t trials=0,pruned32=0,pruned64=0;
};
// A finite, nonnegative threshold enables 32/64 coordinate prefix checks.
// A rejected result is a sentinel: the next representable double above the
// threshold (or its widened exact distance for the tiny-distance fallback).
// It certifies rejection and is a lower bound, NOT the full candidate distance.
// Survivors exactly match the corresponding unpruned kernel.
// safe has the same prior finite/range-check contract as the batch API below.
// Counters count eligible candidates, rather than calls (four per batch).
double l2_sq_f32_pruned(const float* a,const float* b,size_t n,double threshold,
                       bool safe=true,L2PruneCounters* counters=nullptr);
void l2_sq_f32_batch4_pruned(const float* query,const float* const vectors[4],
                            size_t n,double threshold,double output[4],
                            bool safe=true,L2PruneCounters* counters=nullptr);

// Public float-output kernel. Values outside float range can overflow or
// underflow on conversion; SQL ranking uses the widened overload below.
void l2_sq_f32_batch4(const float* query,const float* v0,const float* v1,
                      const float* v2,const float* v3,size_t dim,float out[4]);
// One query, four independent vectors. safe requires a prior finite/range
// check on all five vectors; false uses widened double accumulation.
void l2_sq_f32_batch4(const float* query,const float* const vectors[4],
                      size_t n,double output[4],bool safe=true);
}
