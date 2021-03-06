/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <cpuinfo.h>
#include <cassert>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include "fbgemm/Fbgemm.h"

namespace fbgemm {

template <typename T, typename accT>
PackAWithRowOffset<T, accT>::PackAWithRowOffset(
    matrix_op_t trans,
    uint32_t nRow,
    uint32_t nCol,
    const T* smat,
    uint32_t ld,
    inpType* pmat,
    uint32_t groups,
    int32_t zero_pt,
    int32_t* row_offset)
    : PackMatrix<PackAWithRowOffset<T, accT>, T, accT>(
          nRow,
          nCol,
          pmat,
          zero_pt),
      trans_(trans),
      smat_(smat),
      ld_(ld),
      G_(groups),
      row_offset_(row_offset) {
  assert(G_ == 1 && "Groups != 1 not supported yet");

  rowOffsetAllocatedHere = false;

  if (cpuinfo_has_x86_avx512f()) {
    BaseType::brow_ = PackingTraits<T, accT, inst_set_t::avx512>::MCB;
    BaseType::bcol_ = PackingTraits<T, accT, inst_set_t::avx512>::KCB;
    row_interleave_B_ =
        PackingTraits<T, accT, inst_set_t::avx512>::ROW_INTERLEAVE;
  } else if (cpuinfo_has_x86_avx2()) {
    BaseType::brow_ = PackingTraits<T, accT, inst_set_t::avx2>::MCB;
    BaseType::bcol_ = PackingTraits<T, accT, inst_set_t::avx2>::KCB;
    row_interleave_B_ =
        PackingTraits<T, accT, inst_set_t::avx2>::ROW_INTERLEAVE;
  } else {
    // TODO: Have default slower path
    assert(0 && "unknown architecure");
  }
  if (pmat) {
    BaseType::buf_ = pmat;
  } else {
    BaseType::bufAllocatedHere_ = true;
    BaseType::buf_ = (T*)fbgemmAlignedAlloc(
        64, BaseType::brow_ * BaseType::bcol_ * sizeof(T));
  }
  if (!row_offset_) {
    rowOffsetAllocatedHere = true;
    row_offset_ = static_cast<int32_t*>(
        fbgemmAlignedAlloc(64, BaseType::brow_ * sizeof(int32_t)));
  }
}

template <typename T, typename accT>
void PackAWithRowOffset<T, accT>::pack(const block_type_t& block) {
  assert(block.row_start % BaseType::blockRowSize() == 0);
  assert(block.col_start % BaseType::blockColSize() == 0);
  assert(block.row_size <= BaseType::blockRowSize());
  assert(block.col_size <= BaseType::blockColSize());

  block_type_t block_p = {block.row_start,
                          block.row_size,
                          block.col_start,
                          (block.col_size + row_interleave_B_ - 1) /
                              row_interleave_B_ * row_interleave_B_};
  assert(block_p.col_size <= BaseType::blockColSize());
  BaseType::packedBlock(block_p);

  T* out = BaseType::getBuf();
  bool tr = (trans_ == matrix_op_t::Transpose);
  // accumulate into row offset?
  bool row_offset_acc = (block.col_start != 0);
  int32_t* row_offset_buf = getRowOffsetBuffer();
  if (tr) {
    for (int i = block.row_start; i < block.row_start + block.row_size; ++i) {
      int32_t row_sum =
          row_offset_acc ? row_offset_buf[i - block.row_start] : 0;
      for (int j = block.col_start; j < block.col_start + block.col_size; ++j) {
        T val = smat_[i + ld_ * j];
        row_sum += val;
        out[(i - block.row_start) * BaseType::blockColSize() +
            (j - block.col_start)] = val;
      }
      row_offset_buf[i - block.row_start] = row_sum;
      // zero fill
      // Please see the comment in PackAMatrix.cc on zero vs zero_pt fill.
      for (int j = block.col_start + block.col_size;
           j < block_p.col_start + block_p.col_size;
           ++j) {
        out[(i - block.row_start) * BaseType::blockColSize() +
            (j - block.col_start)] = 0;
      }
    }
  } else {
    for (int i = block.row_start; i < block.row_start + block.row_size; ++i) {
      int buf_idx = i - block.row_start;
      memcpy(
          out + buf_idx * BaseType::blockColSize(),
          smat_ + i * ld_ + block.col_start,
          block.col_size * sizeof(T));
      // zero fill
      for (int j = block.col_size; j < block_p.col_size; ++j) {
        out[buf_idx * BaseType::blockColSize() + j] = 0;
      }
      int32_t row_sum =
          row_offset_acc ? row_offset_buf[i - block.row_start] : 0;
      __m256i sum_v = _mm256_setzero_si256();
      __m256i one_epi16_v = _mm256_set1_epi16(1);
      __m256i one_epi8_v = _mm256_set1_epi8(1);
      for (int j = block.col_start;
           j < block.col_start + block.col_size / 32 * 32;
           j += 32) {
        __m256i src_v = _mm256_loadu_si256(
            reinterpret_cast<__m256i const*>(smat_ + i * ld_ + j));
        sum_v = _mm256_add_epi32(
            sum_v,
            _mm256_madd_epi16(
                _mm256_maddubs_epi16(src_v, one_epi8_v), one_epi16_v));
      }
      for (int j = block.col_start + block.col_size / 32 * 32;
           j < block.col_start + block.col_size;
           ++j) {
        row_sum += smat_[i * ld_ + j];
      }
      // alignas(64) std::array<int32_t, 8> temp;
      alignas(64) std::int32_t temp[8];
      //_mm256_store_si256(reinterpret_cast<__m256i*>(temp.data()), sum_v);
      _mm256_store_si256(reinterpret_cast<__m256i*>(temp), sum_v);
      for (int k = 0; k < 8; ++k) {
        row_sum += temp[k];
      }
      row_offset_buf[i - block.row_start] = row_sum;
    }
  }
}

template <typename T, typename accT>
int32_t PackAWithRowOffset<T, accT>::addr(int32_t r, int32_t c) const {
  int32_t block_row_id = r / BaseType::blockRowSize();
  int32_t brow_offset = (block_row_id * BaseType::blockCols()) *
      (BaseType::blockRowSize() * BaseType::blockColSize());

  int32_t block_col_id = c / BaseType::blockColSize();
  int32_t bcol_offset =
      block_col_id * BaseType::blockRowSize() * BaseType::blockColSize();
  int32_t block_offset = brow_offset + bcol_offset;
  int32_t inblock_offset =
      (r % BaseType::blockRowSize()) * BaseType::blockColSize() +
      (c % BaseType::blockColSize());

  int32_t index = block_offset + inblock_offset;

  return index;
}

template <typename T, typename accT>
void PackAWithRowOffset<T, accT>::printPackedMatrix(std::string name) {
  std::cout << name << ":"
            << "[" << BaseType::numPackedRows() << ", "
            << BaseType::numPackedCols() << "]" << std::endl;

  T* out = BaseType::getBuf();
  for (auto r = 0; r < BaseType::numPackedRows(); ++r) {
    for (auto c = 0; c < BaseType::numPackedCols(); ++c) {
      T val = out[addr(r, c)];
      if (std::is_integral<T>::value) {
        // cast to int64 because cout doesn't print int8_t type directly
        std::cout << std::setw(5) << static_cast<int64_t>(val) << " ";
      } else {
        std::cout << std::setw(5) << val << " ";
      }
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

template <typename T, typename accT>
int PackAWithRowOffset<T, accT>::rowOffsetBufferSize() {
  if (cpuinfo_initialize()) {
    if (cpuinfo_has_x86_avx512f()) {
      return PackingTraits<T, accT, inst_set_t::avx512>::MCB;
    } else if (cpuinfo_has_x86_avx2()) {
      return PackingTraits<T, accT, inst_set_t::avx2>::MCB;
    } else {
      // TODO: Have default slower path
      assert(0 && "unsupported architecture");
      return -1;
    }
  } else {
    throw std::runtime_error("Failed to initialize cpuinfo!");
  }
}

template class PackAWithRowOffset<uint8_t, int32_t>;
template class PackAWithRowOffset<uint8_t, int16_t>;

} // namespace fbgemm
