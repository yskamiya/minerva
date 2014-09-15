#include "data_store.h"
#include <cstdlib>
#include <cstddef>
#include <glog/logging.h>

#ifdef HAS_CUDA
#include <cuda_runtime.h>
#endif

using namespace std;

namespace minerva {

DataStore::DataStore() {
}

DataStore::~DataStore() {
  for (auto& i: data_states_) {
    void* ptr;
    if ((ptr = i.second.data_ptrs[CPU])) {
      free(ptr);
    }
#ifdef HAS_CUDA
    if ((ptr = i.second.data_ptrs[GPU])) {
      CHECK_EQ(cudaFree(ptr), cudaSuccess);
    }
#endif
  }
}

void DataStore::CreateData(uint64_t id, MemTypes type, size_t length, int rc) {
  lock_guard<mutex> lck(access_mutex_);
  DLOG(INFO) << "create data_id=" << id << " length=" << length << " type=" << type;
  DataState& ds = data_states_[id];
  CHECK_EQ(ds.data_ptrs[type], static_cast<void*>(0)) << "id=" << id << " has already been created!";
  CHECK(ds.length == 0 || ds.length == length) << "id=" << id << " allocated length mismatch!";
  ds.length = length;
  ds.reference_count = rc;
  switch (type) {
    case CPU:
      ds.data_ptrs[type] = calloc(length, sizeof(float));
      break;
    case GPU:
#ifdef HAS_CUDA
      CHECK_EQ(cudaMalloc(&ds.data_ptrs[type], length * sizeof(float)), cudaSuccess);
#else
      CHECK(false);
#endif
      break;
    default:
      CHECK(false) << "invalid storage type";
  }
}

float* DataStore::GetData(uint64_t id, MemTypes type) {
  lock_guard<mutex> lck(access_mutex_);
  CHECK(CheckValidity(id)) << "id=" << id << " was not created!";
  DataState& ds = data_states_[id];
  CHECK_NE(ds.data_ptrs[type], static_cast<void*>(0)) << "id=" << id << " was not created!";
  return (float*) data_states_[id].data_ptrs[type];
}

bool DataStore::IncrReferenceCount(uint64_t id, int amount) {
  return DecrReferenceCount(id, -amount);
}

bool DataStore::DecrReferenceCount(uint64_t id, int amount) {
  lock_guard<mutex> lck(access_mutex_);
  CHECK(CheckValidity(id)) << "id=" << id << " was not created!";
  DataState& ds = data_states_[id];
  CHECK_GE(ds.reference_count, amount) << "decrease rc more than it has";
  ds.reference_count -= amount;
  if(ds.reference_count == 0) {
    // do GC
    GC(id);
    return true;
  }
  return false;
}

bool DataStore::SetReferenceCount(uint64_t id, int rc) {
  lock_guard<mutex> lck(access_mutex_);
  CHECK(CheckValidity(id)) << "id=" << id << " was not created!";
  CHECK_GE(rc, 0) << "invalid rc value: " << rc;
  DataState& ds = data_states_[id];
  ds.reference_count = rc;
  if(ds.reference_count == 0) {
    // do GC
    GC(id);
    return true;
  }
  return false;
}

int DataStore::GetReferenceCount(uint64_t id) const {
  lock_guard<mutex> lck(access_mutex_);
  CHECK(CheckValidity(id)) << "id=" << id << " was not created!";
  return data_states_.find(id)->second.reference_count;
}

size_t DataStore::GetTotalBytes(MemTypes memtype) const {
  size_t total_bytes = 0;
  for (auto it : data_states_) {
    const DataState& ds = it.second;
    if (ds.data_ptrs[memtype]) {
      total_bytes += ds.length * sizeof(float);
    }
  }
  return total_bytes;
}

DataStore::DataState::DataState(): length(0), reference_count(0) {
  for (int i = 0; i < NUM_MEM_TYPES; ++i) {
    data_ptrs[i] = 0;
  }
}

/* similar to ExistData, but without lock protection. Only for private usage. */
inline bool DataStore::CheckValidity(uint64_t id) const {
  return data_states_.find(id) != data_states_.end();
}

void DataStore::FreeData(uint64_t id) {
  lock_guard<mutex> lck(access_mutex_);
  CHECK(CheckValidity(id)) << "id=" << id << " was not created!";
  GC(id);
}

void DataStore::GC(uint64_t id) {
  DLOG(INFO) << "GC data with id=" << id;
  DataState& ds = data_states_[id];
  void* ptr;
  if ((ptr = ds.data_ptrs[CPU])) {
    free(ptr);
  }
#ifdef HAS_CUDA
  if ((ptr = ds.data_ptrs[GPU])) {
    CHECK_EQ(cudaFree(ptr), cudaSuccess);
  }
#endif
  data_states_.erase(id);
}

} // end of namespace minerva