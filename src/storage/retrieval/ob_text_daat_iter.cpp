/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_project_relevance()
      && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    scoreless_union_ = sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE == mode_flag_
        && !function_lookup_mode_
        && !iter_param_->need_project_relevance()
        && !iter_param_->need_filter();
    if (scoreless_union_ && dim_iters_->count() > 0 && dim_iters_->count() <= SMALL_UNION_MAX) {
      const sql::ObDatumMeta &meta = iter_param_->id_proj_expr_->datum_meta_;
      common::ObDatumBasicFuncs *funcs = ObDatumFuncs::get_basic_func(meta.type_, meta.cs_type_);
      if (OB_ISNULL(funcs) || OB_ISNULL(small_union_cmp_ = funcs->null_first_cmp_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        small_union_size_ = dim_iters_->count();
        small_union_advance_ = (1u << small_union_size_) - 1;
        for (int64_t i = 0; i < small_union_size_; ++i) {
          small_union_ids_[i] = nullptr;
          small_union_iters_[i] = dim_iters_->at(i);
        }
      }
    }
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  scoreless_union_ = false;
  small_union_cmp_ = nullptr;
  small_union_size_ = 0;
  small_union_advance_ = 0;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  small_union_advance_ = (1u << small_union_size_) - 1;
  for (int64_t i = 0; i < small_union_size_; ++i) {
    small_union_ids_[i] = nullptr;
  }
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  if (!scoreless_union_) {
    ret = ObSRDaaTIterImpl::do_one_merge_round(count);
  } else if (small_union_size_ > 0) {
    ret = merge_small_scoreless_union(count);
  } else {
    // A natural-language filter consumes the union of visible postings. Keep
    // the same ordered heap and duplicate handling without collecting scores
    // or updating the relevance collector's per-document dimension sets.
    ObSRMergeItem item;
    for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
      const int64_t iter_idx = next_round_iter_idxes_[i];
      ObISRDaaTDimIter *dim_iter = nullptr;
      if (OB_ISNULL(dim_iter = dim_iters_->at(iter_idx))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(dim_iter->get_next_row())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(dim_iter->get_curr_id(iter_domain_ids_[iter_idx]))) {
      } else {
        item.iter_idx_ = iter_idx;
        if (OB_FAIL(merge_heap_->push(item))) {
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (merge_heap_->empty()) {
      ret = OB_ITER_END;
    } else if (next_round_cnt_ != 0 && OB_FAIL(merge_heap_->rebuild())) {
    } else {
      next_round_cnt_ = 0;
      bool curr_doc_end = false;
      const ObSRMergeItem *top_item = nullptr;
      int64_t iter_idx = -1;
      while (OB_SUCC(ret) && !merge_heap_->empty() && !curr_doc_end) {
        curr_doc_end = merge_heap_->is_unique_champion();
        if (OB_FAIL(merge_heap_->top(top_item))) {
        } else {
          iter_idx = top_item->iter_idx_;
          if (OB_FAIL(merge_heap_->pop())) {
          } else {
            next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
          }
        }
      }
      if (OB_SUCC(ret)) {
        const ObDatum *id_datum = nullptr;
        if (OB_UNLIKELY(iter_idx < 0)
            || OB_ISNULL(id_datum = iter_domain_ids_[iter_idx])) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(cache_result(count, *id_datum, 0.0))) {
        }
      }
    }
  }
  return ret;
}

int ObTextDaaTIter::merge_small_scoreless_union(int64_t &count)
{
  int ret = OB_SUCCESS;
  // For a handful of sorted postings streams, retain their current IDs and
  // advance only the streams that supplied the previous minimum. This avoids
  // generic heap push/pop/rebuild without changing order or deduplication.
  const ObDatum *minimum = nullptr;
  uint8_t next_advance = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < small_union_size_; ++i) {
    const uint8_t bit = 1u << i;
    if (small_union_advance_ & bit) {
      if (OB_FAIL(small_union_iters_[i]->get_next_row())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          small_union_ids_[i] = nullptr;
        }
      } else if (OB_FAIL(small_union_iters_[i]->get_curr_id(small_union_ids_[i]))) {
      }
    }
    if (OB_FAIL(ret) || nullptr == small_union_ids_[i]) {
    } else if (nullptr == minimum) {
      minimum = small_union_ids_[i];
      next_advance = bit;
    } else {
      int cmp = 0;
      if (OB_FAIL(small_union_cmp_(*small_union_ids_[i], *minimum, cmp, nullptr))) {
      } else if (cmp < 0) {
        minimum = small_union_ids_[i];
        next_advance = bit;
      } else if (cmp == 0) {
        next_advance |= bit;
      }
    }
  }
  small_union_advance_ = next_advance;
  if (OB_FAIL(ret)) {
  } else if (nullptr == minimum) {
    ret = OB_ITER_END;
  } else {
    ret = cache_result(count, *minimum, 0.0);
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
