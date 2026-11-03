/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  pthread_mutex_t search_mutex;
  float* query_buffer;
  unsigned int* id_buffer;
  float* distance_buffer;
  size_t query_capacity;
  size_t id_capacity;
  size_t distance_capacity;
  cuvsCagraSearchParams_t search_params;
  struct cuvsCagraSearchParams search_defaults;
} sk_cuvs_index;

static int sk_matrix_bytes(long rows, long columns, size_t element_size, size_t* bytes)
{
  if (rows <= 0 || columns <= 0 || element_size == 0 || !bytes) return 1;
  if ((uintmax_t)rows > (uintmax_t)SIZE_MAX ||
      (uintmax_t)columns > (uintmax_t)SIZE_MAX) return 1;
  if ((size_t)rows > SIZE_MAX / (size_t)columns) return 1;
  size_t count = (size_t)rows * (size_t)columns;
  if (count > SIZE_MAX / element_size) return 1;
  *bytes = count * element_size;
  return 0;
}

/* Called with search_mutex held, or before the handle is published. */
static void sk_workspace_destroy(sk_cuvs_index* h)
{
  if (h->search_params) cuvsCagraSearchParamsDestroy(h->search_params);
  if (h->distance_buffer) cuvsRMMFree(h->res, h->distance_buffer, h->distance_capacity);
  if (h->id_buffer) cuvsRMMFree(h->res, h->id_buffer, h->id_capacity);
  if (h->query_buffer) cuvsRMMFree(h->res, h->query_buffer, h->query_capacity);
  h->search_params = 0;
  h->query_buffer = 0;
  h->id_buffer = 0;
  h->distance_buffer = 0;
  h->query_capacity = h->id_capacity = h->distance_capacity = 0;
}

/* Prepare all replacements before committing any of them. Allocation failure
 * leaves the old workspace intact and releases every temporary allocation. */
static int sk_workspace_reserve(sk_cuvs_index* h, size_t queries, size_t ids, size_t distances)
{
  float* q = 0;
  unsigned int* n = 0;
  float* d = 0;
  int rc = 0;
  if (queries > h->query_capacity &&
      cuvsRMMAlloc(h->res, (void**)&q, queries) != CUVS_SUCCESS) { rc = 10; goto fail; }
  if (ids > h->id_capacity &&
      cuvsRMMAlloc(h->res, (void**)&n, ids) != CUVS_SUCCESS) { rc = 11; goto fail; }
  if (distances > h->distance_capacity &&
      cuvsRMMAlloc(h->res, (void**)&d, distances) != CUVS_SUCCESS) { rc = 12; goto fail; }
  if (q) {
    if (h->query_buffer) cuvsRMMFree(h->res, h->query_buffer, h->query_capacity);
    h->query_buffer = q; h->query_capacity = queries;
  }
  if (n) {
    if (h->id_buffer) cuvsRMMFree(h->res, h->id_buffer, h->id_capacity);
    h->id_buffer = n; h->id_capacity = ids;
  }
  if (d) {
    if (h->distance_buffer) cuvsRMMFree(h->res, h->distance_buffer, h->distance_capacity);
    h->distance_buffer = d; h->distance_capacity = distances;
  }
  return 0;
fail:
  if (d) cuvsRMMFree(h->res, d, distances);
  if (n) cuvsRMMFree(h->res, n, ids);
  if (q) cuvsRMMFree(h->res, q, queries);
  return rc;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  size_t dataset_bytes = 0;
  if (!base || sk_matrix_bytes(n, dim, sizeof(float), &dataset_bytes)) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (pthread_mutex_init(&h->search_mutex, NULL) != 0) { free(h); return NULL; }
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) {
    pthread_mutex_destroy(&h->search_mutex); free(h); return NULL;
  }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 64;
  // A smaller GPU graph is paired with a larger CAGRA search candidate set.
  h->ip->intermediate_graph_degree = 64;
  // Complete SQL query-set validation is recorded in AMD_V5_PERFORMANCE_3.md.
  h->ip->nn_descent_niter = 4;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: sk_workspace_destroy(h); cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->search_mutex); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || topk > h->n) return 1;
  size_t query_bytes = 0, id_bytes = 0, distance_bytes = 0;
  if (sk_matrix_bytes(nq, h->dim, sizeof(float), &query_bytes) ||
      sk_matrix_bytes(nq, topk, sizeof(unsigned int), &id_bytes) ||
      sk_matrix_bytes(nq, topk, sizeof(float), &distance_bytes) ||
      (uintmax_t)topk > (uintmax_t)(SIZE_MAX / 4)) return 1;
  if (pthread_mutex_lock(&h->search_mutex) != 0) return 2;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  rc = sk_workspace_reserve(h, query_bytes, id_bytes, distance_bytes);
  if (rc) goto done;
  qd = h->query_buffer; nd = h->id_buffer; dd = h->distance_buffer;
  if (cudaMemcpy(qd, query, query_bytes, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (!h->search_params) {
      cuvsCagraSearchParams_t created = 0;
      if (cuvsCagraSearchParamsCreate(&created) != CUVS_SUCCESS) {
        if (created) cuvsCagraSearchParamsDestroy(created);
        rc=14; goto done;
      }
      h->search_params = created;
      h->search_defaults = *created;
    }
    sp = h->search_params;
    *sp = h->search_defaults;
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 4096) required_itopk = 4096;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      // MULTI_CTA supports candidate sets beyond SINGLE_CTA's 512-entry limit.
      sp->search_width = 32;
      sp->algo = MULTI_CTA;
      sp->max_queries = 128;
      // Apply the same GPU search configuration to every query and dataset.
      sp->num_random_samplings = 1;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, id_bytes, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpy(out_dist, dd, distance_bytes, cudaMemcpyDefault) != cudaSuccess) { rc=17; goto done; }
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  pthread_mutex_unlock(&h->search_mutex);
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  /* As with the original API, the owner must quiesce callers before free. */
  pthread_mutex_lock(&h->search_mutex);
  sk_workspace_destroy(h);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_unlock(&h->search_mutex);
  pthread_mutex_destroy(&h->search_mutex);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (pthread_mutex_lock(&index->search_mutex) != 0) return 1;
  cuvsError_t status = cuvsCagraSerialize(index->res, filename, index->idx, true);
  pthread_mutex_unlock(&index->search_mutex);
  if (status != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  size_t dataset_bytes = 0;
  if (!filename || sk_matrix_bytes(n, dim, sizeof(float), &dataset_bytes)) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (pthread_mutex_init(&index->search_mutex, NULL) != 0) { free(index); return NULL; }
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  sk_workspace_destroy(index);
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  pthread_mutex_destroy(&index->search_mutex); free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
