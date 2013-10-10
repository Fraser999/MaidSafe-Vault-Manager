/*  Copyright 2013 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#ifndef MAIDSAFE_DATA_STORE_DATA_BUFFER_H_
#define MAIDSAFE_DATA_STORE_DATA_BUFFER_H_

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "boost/filesystem/convenience.hpp"
#include "boost/filesystem/path.hpp"
#include "boost/variant.hpp"

#include "maidsafe/common/log.h"
#include "maidsafe/common/tagged_value.h"
#include "maidsafe/common/types.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/passport/types.h"

#include "maidsafe/data_types/data_name_variant.h"
#include "maidsafe/data_types/data_type_values.h"

namespace maidsafe {

namespace data_store {

namespace test {

template <typename Key>
class DataBufferTest;
template <typename StoragePolicy>
class DataStoreTest;

}  // namespace test

template <typename Key>
class DataBuffer {
 public:
  typedef Key KeyType;
  typedef std::function<void(const KeyType&, const NonEmptyString&)> PopFunctor;
  // Throws if max_memory_usage >= max_disk_usage.  Throws if a writable folder can't be created in
  // temp_directory_path().  Starts a background worker thread which copies values from memory to
  // disk.  If pop_functor is valid, the disk cache will pop excess items when it is full,
  // otherwise Store will block until there is space made via Delete calls.
  DataBuffer(MemoryUsage max_memory_usage, DiskUsage max_disk_usage, PopFunctor pop_functor);
  // Throws if max_memory_usage >= max_disk_usage.  Throws if a writable folder can't be created in
  // "disk_buffer".  Starts a background worker thread which copies values from memory to disk.  If
  // pop_functor is valid, the disk cache will pop excess items when it is full, otherwise Store
  // will block until there is space made via Delete calls.
  DataBuffer(MemoryUsage max_memory_usage, DiskUsage max_disk_usage, PopFunctor pop_functor,
             const boost::filesystem::path& disk_buffer);
  ~DataBuffer();
  // Throws if the background worker has thrown (e.g. the disk has become inaccessible).  Throws if
  // the size of value is greater than the current specified maximum disk usage, or if the value
  // can't be written to disk (e.g. value is not initialised).  If there is not enough space to
  // store to memory, blocks until there is enough space to store to disk.  Space will be made
  // available via external calls to Delete, and also automatically if pop_functor_ is not NULL.
  void Store(const KeyType& key, const NonEmptyString& value);
  // Throws if the background worker has thrown (e.g. the disk has become inaccessible).  Throws if
  // the value can't be read from disk.  If the value isn't in memory and has started to be stored
  // to disk, blocks while waiting for the storing to complete.
  NonEmptyString Get(const KeyType& key);
  // Throws if the background worker has thrown (e.g. the disk has become inaccessible).  Throws if
  // the value was written to disk and can't be removed.
  void Delete(const KeyType& key);
  // Throws if max_memory_usage > max_disk_usage_.
  void SetMaxMemoryUsage(MemoryUsage max_memory_usage);
  // Throws if max_memory_usage_ > max_disk_usage.
  void SetMaxDiskUsage(DiskUsage max_disk_usage);

  template <typename KeyType>
  friend class test::DataBufferTest;
  template <typename StoragePolicy>
  friend class test::DataStoreTest;

 private:
  DataBuffer(const DataBuffer&);
  DataBuffer(DataBuffer&&);
  DataBuffer& operator=(DataBuffer);

  template <typename UsageType, typename IndexType>
  struct Storage {
    typedef IndexType index_type;
    explicit Storage(UsageType max_in)
        : max(std::move(max_in)),  // NOLINT
          current(0),
          index(),
          mutex(),
          cond_var() {}
    UsageType max, current;
    IndexType index;
    std::mutex mutex;
    std::condition_variable cond_var;
  };

  enum class StoringState {
    kNotStarted,
    kStarted,
    kCancelled,
    kCompleted
  };

  struct MemoryElement {
    MemoryElement(KeyType key_in, NonEmptyString value_in)
        : key(std::move(key_in)),
          value(std::move(value_in)),
          also_on_disk(StoringState::kNotStarted) {}
    KeyType key;
    NonEmptyString value;
    StoringState also_on_disk;
  };

  typedef std::deque<MemoryElement> MemoryIndex;

  struct DiskElement {
    explicit DiskElement(KeyType key_in) : key(std::move(key_in)), state(StoringState::kStarted) {}
    KeyType key;
    StoringState state;
  };
  typedef std::deque<DiskElement> DiskIndex;

  void Init();

  std::unique_lock<std::mutex> StoreInMemory(const KeyType& key, const NonEmptyString& value);
  void WaitForSpaceInMemory(uint64_t required_space,
                            std::unique_lock<std::mutex>& memory_store_lock);
  void StoreOnDisk(const KeyType& key, const NonEmptyString& value,
                   std::unique_lock<std::mutex>&& disk_store_lock);
  void WaitForSpaceOnDisk(const KeyType& key, uint64_t required_space,
                          std::unique_lock<std::mutex>& disk_store_lock, bool& cancelled);
  void DeleteFromMemory(const KeyType& key, StoringState& also_on_disk);
  void DeleteFromDisk(const KeyType& key);
  void RemoveFile(const KeyType& key, NonEmptyString* value);

  void CopyQueueToDisk();
  void CheckWorkerIsStillRunning();
  void StopRunning();
  boost::filesystem::path GetFilename(const KeyType& key) const;

  template <typename T>
  bool HasSpace(const T& store, uint64_t required_space) const;

  template <typename T>
  typename T::index_type::iterator Find(T& store, const KeyType& key);

  typename MemoryIndex::iterator FindOldestInMemoryOnly();
  typename MemoryIndex::iterator FindMemoryRemovalCandidate(
      uint64_t required_space, std::unique_lock<std::mutex>& memory_store_lock);

  typename DiskIndex::iterator FindStartedToStoreOnDisk(const KeyType& key);
  typename DiskIndex::iterator FindOldestOnDisk();

  typename DiskIndex::iterator FindAndThrowIfCancelled(const KeyType& key);

  std::string DebugKeyName(const KeyType& key);

  Storage<MemoryUsage, MemoryIndex> memory_store_;
  Storage<DiskUsage, DiskIndex> disk_store_;
  const PopFunctor kPopFunctor_;
  const boost::filesystem::path kDiskBuffer_;
  const bool kShouldRemoveRoot_;
  std::atomic<bool> running_;
  std::mutex worker_mutex_;
  std::future<void> worker_;
};

// ==================== Implementation =============================================================
template <typename Key>
DataBuffer<Key>::DataBuffer(MemoryUsage max_memory_usage, DiskUsage max_disk_usage,
                            PopFunctor pop_functor)
    : memory_store_(max_memory_usage),
      disk_store_(max_disk_usage),
      kPopFunctor_(std::move(pop_functor)),
      kDiskBuffer_(boost::filesystem::unique_path(boost::filesystem::temp_directory_path() /
                                                  "DB-%%%%-%%%%-%%%%-%%%%")),
      kShouldRemoveRoot_(true),
      running_(true),
      worker_mutex_(),
      worker_() {
  Init();
}

template <typename Key>
DataBuffer<Key>::DataBuffer(MemoryUsage max_memory_usage, DiskUsage max_disk_usage,
                            PopFunctor pop_functor, const boost::filesystem::path& disk_buffer)
    : memory_store_(max_memory_usage),
      disk_store_(max_disk_usage),
      kPopFunctor_(std::move(pop_functor)),
      kDiskBuffer_(disk_buffer),
      kShouldRemoveRoot_(false),
      running_(true),
      worker_mutex_(),
      worker_() {
  Init();
}

template <typename Key>
void DataBuffer<Key>::Init() {
  if (memory_store_.max > disk_store_.max) {
    LOG(kError) << "Max memory usage must be < max disk usage.";
    ThrowError(CommonErrors::invalid_parameter);
  }
  boost::system::error_code error_code;
  if (!boost::filesystem::exists(kDiskBuffer_, error_code)) {
    if (!boost::filesystem::create_directories(kDiskBuffer_, error_code)) {
      LOG(kError) << "Can't create disk root at " << kDiskBuffer_ << ": " << error_code.message();
      ThrowError(CommonErrors::uninitialised);
      return;
    }
  }
  // Check kDiskBuffer_ is writable
  auto test_file(kDiskBuffer_ / "TestFile");
  if (!WriteFile(test_file, "Test")) {
    LOG(kError) << "Can't write file " << test_file;
    ThrowError(CommonErrors::uninitialised);
    return;
  }
  boost::filesystem::remove(test_file);
  worker_ = std::async(std::launch::async, &DataBuffer<KeyType>::CopyQueueToDisk, this);
}

template <typename Key>
DataBuffer<Key>::~DataBuffer() {
  {
    std::lock(memory_store_.mutex, disk_store_.mutex);
    std::lock_guard<std::mutex> memory_store_lock(memory_store_.mutex, std::adopt_lock);
    std::lock_guard<std::mutex> disk_store_lock(disk_store_.mutex, std::adopt_lock);
    running_ = false;
  }
  {
    std::unique_lock<std::mutex> worker_lock(worker_mutex_);
    while (worker_.valid() &&
           worker_.wait_for(std::chrono::seconds(0)) == std::future_status::timeout) {
      worker_lock.unlock();
      memory_store_.cond_var.notify_all();
      disk_store_.cond_var.notify_all();
      std::this_thread::yield();
      worker_lock.lock();
    }
    if (worker_.valid()) {
      try {
        worker_.get();
      }
      catch (const std::exception& e) {
        LOG(kError) << e.what();
      }
    }
  }

  if (kShouldRemoveRoot_) {
    boost::system::error_code error_code;
    boost::filesystem::remove_all(kDiskBuffer_, error_code);
    if (error_code)
      LOG(kWarning) << "Failed to remove " << kDiskBuffer_ << ": " << error_code.message();
  }
}

template <typename Key>
void DataBuffer<Key>::Store(const KeyType& key, const NonEmptyString& value) {
  try {
    Delete(key);
    LOG(kInfo) << "Re-storing value " << HexEncode(value) << " with key " << DebugKeyName(key);
  }
  catch (const std::exception&) {
    LOG(kInfo) << "Storing value " << HexEncode(value) << " with key " << DebugKeyName(key);
  }

  CheckWorkerIsStillRunning();
  auto disk_store_lock(StoreInMemory(key, value));
  if (disk_store_lock)
    StoreOnDisk(key, value, std::move(disk_store_lock));
}

template <typename Key>
std::unique_lock<std::mutex> DataBuffer<Key>::StoreInMemory(const KeyType& key,
                                                            const NonEmptyString& value) {
  {
    uint64_t required_space(value.string().size());
    std::unique_lock<std::mutex> memory_store_lock(memory_store_.mutex);
    if (required_space > memory_store_.max)
      return std::move(std::unique_lock<std::mutex>(disk_store_.mutex));

    WaitForSpaceInMemory(required_space, memory_store_lock);

    if (!running_) {
      {
        std::lock_guard<std::mutex> worker_lock(worker_mutex_);
        if (worker_.valid())
          worker_.get();
      }
      return std::move(std::unique_lock<std::mutex>());
    }

    memory_store_.current.data += required_space;
    memory_store_.index.emplace_back(key, value);
  }
  memory_store_.cond_var.notify_all();
  return std::move(std::unique_lock<std::mutex>());
}

template <typename Key>
void DataBuffer<Key>::WaitForSpaceInMemory(uint64_t required_space,
                                           std::unique_lock<std::mutex>& memory_store_lock) {
  while (!HasSpace(memory_store_, required_space)) {
    auto itr(FindMemoryRemovalCandidate(required_space, memory_store_lock));
    if (!running_)
      return;

    if (itr != memory_store_.index.end()) {
      memory_store_.current.data -= (*itr).value.string().size();
      memory_store_.index.erase(itr);
    }
  }
}

template <typename Key>
void DataBuffer<Key>::StoreOnDisk(const KeyType& key, const NonEmptyString& value,
                                  std::unique_lock<std::mutex>&& disk_store_lock) {
  assert(disk_store_lock);
  if (value.string().size() > disk_store_.max) {
    LOG(kError) << "Cannot store " << DebugKeyName(key) << " since its " << value.string().size()
                << " bytes exceeds max of " << disk_store_.max << " bytes.";
    StopRunning();
    ThrowError(CommonErrors::cannot_exceed_limit);
  }
  disk_store_.index.emplace_back(key);

  bool cancelled(false);
  WaitForSpaceOnDisk(key, value.string().size(), disk_store_lock, cancelled);
  if (!running_)
    return;

  if (!cancelled) {
    if (!WriteFile(GetFilename(key), value.string())) {
      LOG(kError) << "Failed to move " << DebugKeyName(key) << " to disk.";
      StopRunning();
      ThrowError(CommonErrors::filesystem_io_error);
    }
    auto itr(FindStartedToStoreOnDisk(key));
    if (itr != disk_store_.index.end())
      (*itr).state = StoringState::kCompleted;

    disk_store_.current.data += value.string().size();
  }
  disk_store_lock.unlock();
  disk_store_.cond_var.notify_all();
}

template <typename Key>
void DataBuffer<Key>::WaitForSpaceOnDisk(const KeyType& key, uint64_t required_space,
                                         std::unique_lock<std::mutex>& disk_store_lock,
                                         bool& cancelled) {
  while (!HasSpace(disk_store_, required_space) && running_) {
    auto itr(Find(disk_store_, key));
    if (itr == disk_store_.index.end()) {
      cancelled = true;
      return;
    }

    if ((*itr).state == StoringState::kCancelled) {
      disk_store_.index.erase(itr);
      cancelled = true;
      return;
    }

    if (kPopFunctor_) {
      itr = FindOldestOnDisk();
      assert((*itr).state != StoringState::kStarted);
      if ((*itr).state == StoringState::kCompleted) {
        KeyType oldest_key(itr->key);
        NonEmptyString oldest_value;
        RemoveFile(oldest_key, &oldest_value);
        disk_store_.index.erase(itr);
        kPopFunctor_(oldest_key, oldest_value);
      }
    } else {
      // Rely on client of this class to call Delete until enough space becomes available
      if (running_)
        disk_store_.cond_var.wait(disk_store_lock);
    }
  }
}

template <typename Key>
NonEmptyString DataBuffer<Key>::Get(const KeyType& key) {
  CheckWorkerIsStillRunning();
  {
    std::lock_guard<std::mutex> memory_store_lock(memory_store_.mutex);
    auto itr(Find(memory_store_, key));
    if (itr != memory_store_.index.end())
      return (*itr).value;
  }
  std::unique_lock<std::mutex> disk_store_lock(disk_store_.mutex);
  auto itr(FindAndThrowIfCancelled(key));
  if ((*itr).state == StoringState::kStarted) {
    disk_store_.cond_var.wait(disk_store_lock, [this, &key]()->bool {
      auto itr(Find(disk_store_, key));
      return (itr == disk_store_.index.end() || (*itr).state != StoringState::kStarted);
    });
    itr = FindAndThrowIfCancelled(key);
  }
  return ReadFile(GetFilename(key));
  // TODO(Fraser#5#): 2012-11-23 - There should maybe be another background task moving the item
  //                               from wherever it's found to the back of the memory index.
}

template <typename Key>
void DataBuffer<Key>::Delete(const KeyType& key) {
  CheckWorkerIsStillRunning();
  StoringState also_on_disk(StoringState::kNotStarted);
  DeleteFromMemory(key, also_on_disk);
  if (also_on_disk != StoringState::kNotStarted)
    DeleteFromDisk(key);
}

template <typename Key>
void DataBuffer<Key>::DeleteFromMemory(const KeyType& key, StoringState& also_on_disk) {
  bool changed(false);
  {
    std::lock_guard<std::mutex> memory_store_lock(memory_store_.mutex);
    auto itr(Find(memory_store_, key));
    if (itr != memory_store_.index.end()) {
      also_on_disk = (*itr).also_on_disk;
      memory_store_.current.data -= (*itr).value.string().size();
      memory_store_.index.erase(itr);
      changed = true;
    } else {
      // Assume it's on disk so as to invoke a DeleteFromDisk
      also_on_disk = StoringState::kCompleted;
    }
  }
  if (changed)
    memory_store_.cond_var.notify_all();
}

template <typename Key>
void DataBuffer<Key>::DeleteFromDisk(const KeyType& key) {
  {
    std::lock_guard<std::mutex> disk_store_lock(disk_store_.mutex);
    auto itr(Find(disk_store_, key));
    if (itr == disk_store_.index.end()) {
      LOG(kWarning) << DebugKeyName(key) << " is not in the disk index.";
      ThrowError(CommonErrors::no_such_element);
    }

    if ((*itr).state == StoringState::kStarted) {
      (*itr).state = StoringState::kCancelled;
    } else if ((*itr).state == StoringState::kCompleted) {
      RemoveFile(itr->key, nullptr);
      disk_store_.index.erase(itr);
    }
  }
  disk_store_.cond_var.notify_all();
}

template <typename Key>
void DataBuffer<Key>::RemoveFile(const KeyType& key, NonEmptyString* value) {
  auto path(GetFilename(key));
  boost::system::error_code error_code;
  uint64_t size(boost::filesystem::file_size(path, error_code));
  if (error_code) {
    LOG(kError) << "Error getting file size of " << path << ": " << error_code.message();
    ThrowError(CommonErrors::filesystem_io_error);
  }
  if (value)
    *value = ReadFile(path);
  if (!boost::filesystem::remove(path, error_code) || error_code) {
    LOG(kError) << "Error removing " << path << ": " << error_code.message();
    ThrowError(CommonErrors::filesystem_io_error);
  }
  disk_store_.current.data -= size;
}

template <typename Key>
void DataBuffer<Key>::CopyQueueToDisk() {
  KeyType key;
  NonEmptyString value;
  for (;;) {
    {
      // Get oldest value not yet stored to disk
      std::unique_lock<std::mutex> memory_store_lock(memory_store_.mutex);
      auto itr(memory_store_.index.end());
      memory_store_.cond_var.wait(memory_store_lock, [this, &itr]()->bool {
        itr = FindOldestInMemoryOnly();
        return itr != memory_store_.index.end() || !running_;
      });
      if (!running_)
        return;

      key = (*itr).key;
      value = (*itr).value;
      (*itr).also_on_disk = StoringState::kStarted;
      std::unique_lock<std::mutex> disk_store_lock(disk_store_.mutex);
      memory_store_lock.unlock();
      StoreOnDisk(key, value, std::move(disk_store_lock));
      memory_store_lock.lock();
      itr = Find(memory_store_, key);
      if (itr != memory_store_.index.end())
        (*itr).also_on_disk = StoringState::kCompleted;
    }
    memory_store_.cond_var.notify_all();
  }
}

template <typename Key>
void DataBuffer<Key>::CheckWorkerIsStillRunning() {
  // if this goes ready then we have an exception so get that (throw basically)
  {
    std::lock_guard<std::mutex> worker_lock(worker_mutex_);
    if (worker_.valid() && worker_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
      worker_.get();
  }
  if (!running_) {
    LOG(kError) << "Worker is no longer running.";
    ThrowError(CommonErrors::filesystem_io_error);
  }
}

template <typename Key>
void DataBuffer<Key>::StopRunning() {
  running_ = false;
  memory_store_.cond_var.notify_all();
  disk_store_.cond_var.notify_all();
}

template <typename Key>
void DataBuffer<Key>::SetMaxMemoryUsage(MemoryUsage max_memory_usage) {
  {
    std::lock_guard<std::mutex> memory_store_lock(memory_store_.mutex);
    if (max_memory_usage > disk_store_.max) {
      LOG(kError) << "Max memory usage must be <= max disk usage.";
      ThrowError(CommonErrors::invalid_parameter);
    }
    memory_store_.max = max_memory_usage;
  }
  memory_store_.cond_var.notify_all();
}

template <typename Key>
void DataBuffer<Key>::SetMaxDiskUsage(DiskUsage max_disk_usage) {
  bool increased(false);
  {
    std::lock_guard<std::mutex> disk_store_lock(disk_store_.mutex);
    if (memory_store_.max > max_disk_usage) {
      LOG(kError) << "Max memory usage must be <= max disk usage.";
      ThrowError(CommonErrors::invalid_parameter);
    }
    increased = (max_disk_usage > disk_store_.max);
    disk_store_.max = max_disk_usage;
  }
  if (increased)
    disk_store_.cond_var.notify_all();
}

template <typename Key>
boost::filesystem::path DataBuffer<Key>::GetFilename(const KeyType& key) const {
  return kDiskBuffer_ / HexEncode(key);
}

template <>
boost::filesystem::path DataBuffer<DataNameVariant>::GetFilename(const DataNameVariant& key) const;

template <typename Key>
template <typename T>
bool DataBuffer<Key>::HasSpace(const T& store, uint64_t required_space) const {
  assert(store.max >= required_space);
  return store.current <= store.max - required_space;
}

template <typename Key>
template <typename T>
typename T::index_type::iterator DataBuffer<Key>::Find(T& store, const KeyType& key) {
  return std::find_if(store.index.begin(), store.index.end(),
                      [&key](const typename T::index_type::value_type &
                             key_value) { return key_value.key == key; });
}

template <typename Key>
typename DataBuffer<Key>::MemoryIndex::iterator DataBuffer<Key>::FindOldestInMemoryOnly() {
  return std::find_if(memory_store_.index.begin(), memory_store_.index.end(),
                      [](const MemoryElement & key_value) {
    return key_value.also_on_disk == StoringState::kNotStarted;
  });
}

template <typename Key>
typename DataBuffer<Key>::MemoryIndex::iterator DataBuffer<Key>::FindMemoryRemovalCandidate(
    uint64_t required_space, std::unique_lock<std::mutex>& memory_store_lock) {
  auto itr(memory_store_.index.end());
  memory_store_.cond_var.wait(memory_store_lock, [this, &itr, &required_space]()->bool {
    itr = std::find_if(memory_store_.index.begin(), memory_store_.index.end(),
                       [](const MemoryElement & key_value) {
      return key_value.also_on_disk == StoringState::kCompleted;
    });
    return itr != memory_store_.index.end() || HasSpace(memory_store_, required_space) || !running_;
  });
  return itr;
}

template <typename Key>
typename DataBuffer<Key>::DiskIndex::iterator DataBuffer<Key>::FindStartedToStoreOnDisk(
    const KeyType& key) {
  return std::find_if(disk_store_.index.begin(), disk_store_.index.end(),
                      [&key](const DiskElement & entry) {
    return entry.state == StoringState::kStarted && entry.key == key;
  });
}

template <typename Key>
typename DataBuffer<Key>::DiskIndex::iterator DataBuffer<Key>::FindOldestOnDisk() {
  return disk_store_.index.begin();
}

template <typename Key>
typename DataBuffer<Key>::DiskIndex::iterator DataBuffer<Key>::FindAndThrowIfCancelled(
    const KeyType& key) {
  auto itr(Find(disk_store_, key));
  if (itr == disk_store_.index.end() || (*itr).state == StoringState::kCancelled) {
    LOG(kWarning) << DebugKeyName(key) << " is not in the disk index or is cancelled.";
    ThrowError(CommonErrors::no_such_element);
  }
  return itr;
}

template <typename Key>
std::string DataBuffer<Key>::DebugKeyName(const KeyType& key) {
  return HexEncode(key);
}

template <>
std::string DataBuffer<DataNameVariant>::DebugKeyName(const DataNameVariant& key);

}  // namespace data_store

}  // namespace maidsafe

#endif  // MAIDSAFE_DATA_STORE_DATA_BUFFER_H_
