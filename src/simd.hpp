#pragma once
#include <cstddef>
namespace mvdb {
// Public float-output kernel. Values outside float range can overflow or
// underflow on conversion; SQL ranking uses the widened overload below.
void l2_sq_f32_batch4(const float* query,const float* v0,const float* v1,
                      const float* v2,const float* v3,size_t dim,float out[4]);
// One query, four independent vectors. safe requires a prior finite/range
// check on all five vectors; false uses widened double accumulation.
void l2_sq_f32_batch4(const float* query,const float* const vectors[4],
                      size_t n,double output[4],bool safe=true);
}
