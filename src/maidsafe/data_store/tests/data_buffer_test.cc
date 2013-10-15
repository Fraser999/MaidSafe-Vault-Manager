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

#include "maidsafe/data_store/data_buffer.h"

#include <memory>

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"

#include "maidsafe/common/error.h"
#include "maidsafe/common/test.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/data_types/data_name_variant.h"

namespace fs = boost::filesystem;
namespace args = std::placeholders;

namespace maidsafe {
namespace data_store {

namespace test {

namespace {

typedef std::pair<uint64_t, uint64_t> MaxMemoryDiskUsage;
const uint64_t OneKB(1024);
const uint64_t kDefaultMaxMemoryUsage(1000);
const uint64_t kDefaultMaxDiskUsage(2000);
const uint32_t kMaxDataTagValue(14);

template <typename KeyType>
KeyType GenerateRandomKey() {
  return KeyType(RandomString(crypto::SHA512::DIGESTSIZE));
}

template <>
DataNameVariant GenerateRandomKey<DataNameVariant>() {
  Identity id(RandomString(crypto::SHA512::DIGESTSIZE));
  // Check the value of 'kMaxDataTagValue' is not too high
  GetDataNameVariant(static_cast<DataTagValue>(kMaxDataTagValue), id);

  DataTagValue tag_value(static_cast<DataTagValue>(RandomUint32() % kMaxDataTagValue));
  return GetDataNameVariant(tag_value, id);
}

template <typename KeyType>
KeyType GenerateKeyFromValue(const NonEmptyString& value) {
  return KeyType(crypto::Hash<crypto::SHA512>(value).string());
}

template <>
DataNameVariant GenerateKeyFromValue<DataNameVariant>(const NonEmptyString& value) {
  Identity id(crypto::Hash<crypto::SHA512>(value));
  // Check the value of 'kMaxDataTagValue' is not too high
  GetDataNameVariant(static_cast<DataTagValue>(kMaxDataTagValue), id);
  DataTagValue tag_value(static_cast<DataTagValue>(RandomUint32() % kMaxDataTagValue));
  return GetDataNameVariant(tag_value, id);
}

}  // unnamed namespace

class DataBufferTest {
 protected:
  typedef DataNameVariant KeyType;
  typedef DataBuffer<KeyType> DataBufferType;
  typedef std::unique_ptr<DataBufferType> DataBufferPtr;
  typedef DataBufferType::PopFunctor PopFunctor;
  typedef std::vector<std::pair<KeyType, NonEmptyString>> KeyValueVector;

  DataBufferTest()
      : max_memory_usage_(kDefaultMaxMemoryUsage),
        max_disk_usage_(kDefaultMaxDiskUsage),
        data_buffer_path_(),
        pop_functor_(),
        data_buffer_(new DataBufferType(max_memory_usage_, max_disk_usage_, pop_functor_)) {}

  void PopFunction(const KeyType& key, const NonEmptyString& value,
                   const std::vector<std::pair<KeyType, NonEmptyString>>& key_value_pairs,
                   size_t& index, std::mutex& mutex, std::condition_variable& cond_var) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      KeyType popped_key(key_value_pairs[index].first);
      NonEmptyString popped_value(key_value_pairs[index].second);
      GetIdentityVisitor get_identity;
      REQUIRE(boost::apply_visitor(get_identity, popped_key) ==
              boost::apply_visitor(get_identity, key));
      REQUIRE(popped_value == value);
      ++index;
    }
    cond_var.notify_one();
  }

  bool DeleteDirectory(const fs::path& directory) {
    boost::system::error_code error_code;
    fs::directory_iterator end;
    try {
      fs::directory_iterator it(directory);
      for (; it != end; ++it)
        fs::remove_all((*it).path(), error_code);
      if (error_code)
        return false;
    }
    catch (const std::exception& e) {
      LOG(kError) << e.what();
      return false;
    }
    return true;
  }

  KeyValueVector PopulateDataBuffer(size_t num_entries, size_t num_memory_entries,
                                    size_t num_disk_entries, maidsafe::test::TestPath test_path,
                                    const PopFunctor& pop_functor) {
    boost::system::error_code error_code;
    data_buffer_path_ = fs::path(*test_path / "data_buffer");
    KeyValueVector key_value_pairs;
    NonEmptyString value, recovered;
    KeyType key;

    REQUIRE(fs::create_directories(data_buffer_path_, error_code));
    REQUIRE(0 == error_code.value());
    REQUIRE(fs::exists(data_buffer_path_, error_code));
    REQUIRE(0 == error_code.value());

    for (size_t i = 0; i < num_entries; ++i) {
      value = NonEmptyString(std::string(RandomAlphaNumericString(static_cast<uint32_t>(OneKB))));
      key = GenerateKeyFromValue<KeyType>(value);
      key_value_pairs.push_back(std::make_pair(key, value));
    }
    data_buffer_.reset(new DataBufferType(MemoryUsage(num_memory_entries * OneKB),
                                          DiskUsage(num_disk_entries * OneKB), pop_functor,
                                          data_buffer_path_));
    for (auto key_value : key_value_pairs) {
      REQUIRE_NOTHROW(data_buffer_->Store(key_value.first, key_value.second));
      REQUIRE_NOTHROW(recovered = data_buffer_->Get(key_value.first));
      REQUIRE(key_value.second == recovered);
    }
    return key_value_pairs;
  }

  boost::filesystem::path GetkDiskBuffer(const DataBufferType& data_buffer) {
    return data_buffer.kDiskBuffer_;
  }

  std::string DebugKeyName(const KeyType& key) { return data_buffer_->DebugKeyName(key); }

  MemoryUsage max_memory_usage_;
  DiskUsage max_disk_usage_;
  fs::path data_buffer_path_;
  PopFunctor pop_functor_;
  DataBufferPtr data_buffer_;
};

TEST_CASE_METHOD(DataBufferTest, "DataBufferConstructor", "[Private][Behavioural]") {
  REQUIRE_NOTHROW(DataBufferType(MemoryUsage(0), DiskUsage(0), pop_functor_));
  REQUIRE_NOTHROW(DataBufferType(MemoryUsage(1), DiskUsage(1), pop_functor_));
  REQUIRE_THROWS_AS(DataBufferType(MemoryUsage(1), DiskUsage(0), pop_functor_), std::exception);
  REQUIRE_THROWS_AS(DataBufferType(MemoryUsage(2), DiskUsage(1), pop_functor_), std::exception);
  REQUIRE_THROWS_AS(DataBufferType(MemoryUsage(200001), DiskUsage(200000), pop_functor_),
                    std::exception);
  REQUIRE_NOTHROW(DataBufferType(MemoryUsage(199999), DiskUsage(200000), pop_functor_));

  // Create a path to a file, and check that it can't be used as the disk buffer path.
  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  REQUIRE(!test_path->empty());
  boost::filesystem::path file_path(*test_path / "File");
  REQUIRE(WriteFile(file_path, " "));
  REQUIRE_THROWS_AS(DataBufferType(MemoryUsage(199999), DiskUsage(200000), pop_functor_, file_path),
                    std::exception);
  REQUIRE_THROWS_AS(DataBufferType(MemoryUsage(199999), DiskUsage(200000), pop_functor_,
                                   file_path / "Directory"), std::exception);

  // Create a path to a directory, and check that it can be used as the disk buffer path.
  boost::filesystem::path directory_path(*test_path / "Directory");
  REQUIRE_NOTHROW(DataBufferType(MemoryUsage(1), DiskUsage(1), pop_functor_, directory_path));
  REQUIRE(fs::exists(directory_path));
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferDestructor", "[Private][Behavioural]") {
  boost::filesystem::path data_buffer_path;
  {
    DataBufferType data_buffer(MemoryUsage(1), DiskUsage(1), pop_functor_);
    data_buffer_path = GetkDiskBuffer(data_buffer);
    REQUIRE(fs::exists(data_buffer_path));
  }
  REQUIRE(!fs::exists(data_buffer_path));

  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  REQUIRE(!test_path->empty());
  data_buffer_path = *test_path / "Directory";
  {
    DataBufferType data_buffer(MemoryUsage(1), DiskUsage(1), pop_functor_, data_buffer_path);
    REQUIRE(fs::exists(data_buffer_path));
  }
  REQUIRE(fs::exists(data_buffer_path));
  REQUIRE(DeleteDirectory(*test_path));
  REQUIRE(!fs::exists(data_buffer_path));
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferSetMaxDiskMemoryUsage", "[Private][Behavioural]") {
  REQUIRE_NOTHROW(data_buffer_->SetMaxMemoryUsage(MemoryUsage(max_disk_usage_ - 1)));
  REQUIRE_NOTHROW(data_buffer_->SetMaxMemoryUsage(MemoryUsage(max_disk_usage_)));
  REQUIRE_THROWS_AS(data_buffer_->SetMaxMemoryUsage(MemoryUsage(max_disk_usage_ + 1)),
                    std::exception);
  REQUIRE_THROWS_AS(data_buffer_->SetMaxDiskUsage(DiskUsage(max_disk_usage_ - 1)),
                    std::exception);
  REQUIRE_NOTHROW(data_buffer_->SetMaxDiskUsage(DiskUsage(max_disk_usage_)));
  REQUIRE_NOTHROW(data_buffer_->SetMaxDiskUsage(DiskUsage(max_disk_usage_ + 1)));
  REQUIRE_THROWS_AS(data_buffer_->SetMaxMemoryUsage(MemoryUsage(static_cast<uint64_t>(-1))),
                    std::exception);
  REQUIRE_NOTHROW(data_buffer_->SetMaxMemoryUsage(MemoryUsage(static_cast<uint64_t>(1))));
  REQUIRE_THROWS_AS(data_buffer_->SetMaxDiskUsage(DiskUsage(static_cast<uint64_t>(0))),
                    std::exception);
  REQUIRE_NOTHROW(data_buffer_->SetMaxDiskUsage(DiskUsage(static_cast<uint64_t>(1))));
  REQUIRE_NOTHROW(data_buffer_->SetMaxMemoryUsage(MemoryUsage(static_cast<uint64_t>(0))));
  REQUIRE_NOTHROW(data_buffer_->SetMaxDiskUsage(DiskUsage(static_cast<uint64_t>(0))));
  REQUIRE_NOTHROW(data_buffer_->SetMaxDiskUsage(DiskUsage(std::numeric_limits<uint64_t>().max())));
  MemoryUsage memory_usage(std::numeric_limits<uint64_t>().max());
  REQUIRE_NOTHROW(data_buffer_->SetMaxMemoryUsage(MemoryUsage(memory_usage)));
  REQUIRE_THROWS_AS(data_buffer_->SetMaxDiskUsage(DiskUsage(kDefaultMaxDiskUsage)),
                    std::exception);
  REQUIRE_NOTHROW(data_buffer_->SetMaxMemoryUsage(MemoryUsage(kDefaultMaxMemoryUsage)));
  REQUIRE_NOTHROW(data_buffer_->SetMaxDiskUsage(DiskUsage(kDefaultMaxDiskUsage)));
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferRemoveDiskBuffer", "[Private][Behavioural]") {
  boost::system::error_code error_code;
  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  fs::path data_buffer_path(*test_path / "data_buffer");
  const uintmax_t kMemorySize(1), kDiskSize(2);

  data_buffer_.reset(new DataBufferType(MemoryUsage(kMemorySize), DiskUsage(kDiskSize), pop_functor_,
                                        data_buffer_path));
  auto key(GenerateRandomKey<KeyType>());
  NonEmptyString small_value(std::string(kMemorySize, 'a'));
  REQUIRE_NOTHROW(data_buffer_->Store(key, small_value));
  REQUIRE_NOTHROW(data_buffer_->Delete(key));
  REQUIRE(1 == fs::remove_all(data_buffer_path, error_code));
  REQUIRE_FALSE(fs::exists(data_buffer_path, error_code));
  // Fits into memory buffer successfully.  Background thread in future should throw, causing other
  // API functions to throw on next execution.
  REQUIRE_NOTHROW(data_buffer_->Store(key, small_value));
  Sleep(std::chrono::seconds(1));
  REQUIRE_THROWS_AS(data_buffer_->Store(key, small_value), std::exception);
  REQUIRE_THROWS_AS(data_buffer_->Get(key), std::exception);
  REQUIRE_THROWS_AS(data_buffer_->Delete(key), std::exception);

  data_buffer_.reset(new DataBufferType(MemoryUsage(kMemorySize), DiskUsage(kDiskSize), pop_functor_,
                                        data_buffer_path));
  NonEmptyString large_value(std::string(kDiskSize, 'a'));
  REQUIRE_NOTHROW(data_buffer_->Store(key, large_value));
  REQUIRE_NOTHROW(data_buffer_->Delete(key));
  REQUIRE(1 == fs::remove_all(data_buffer_path, error_code));
  REQUIRE_FALSE(fs::exists(data_buffer_path, error_code));
  // Skips memory buffer and goes straight to disk, causing exception.  Background thread in future
  // should finish, causing other API functions to throw on next execution.
  REQUIRE_THROWS_AS(data_buffer_->Store(key, large_value), std::exception);
  REQUIRE_THROWS_AS(data_buffer_->Get(key), std::exception);
  REQUIRE_THROWS_AS(data_buffer_->Delete(key), std::exception);
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferSuccessfulStore", "[Private][Behavioural]") {
  NonEmptyString value1(RandomAlphaNumericString(static_cast<uint32_t>(max_memory_usage_)));
  auto key1(GenerateKeyFromValue<KeyType>(value1));
  NonEmptyString value2(RandomAlphaNumericString(static_cast<uint32_t>(max_memory_usage_)));
  auto key2(GenerateKeyFromValue<KeyType>(value2));
  NonEmptyString recovered;

  REQUIRE_NOTHROW(data_buffer_->Store(key1, value1));
  REQUIRE_NOTHROW(data_buffer_->Store(key2, value2));
  REQUIRE_NOTHROW(recovered = data_buffer_->Get(key1));
  REQUIRE(recovered == value1);
  REQUIRE_NOTHROW(recovered = data_buffer_->Get(key2));
  REQUIRE(recovered == value2);
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferUnsuccessfulStore", "[Private][Behavioural]") {
  NonEmptyString value(std::string(static_cast<uint32_t>(max_disk_usage_ + 1), 'a'));
  auto key(GenerateKeyFromValue<KeyType>(value));
  REQUIRE_THROWS_AS(data_buffer_->Store(key, value), std::exception);
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferDeleteOnDiskBufferOverfill",
                 "[Private][Behavioural]") {
  const size_t num_entries(4), num_memory_entries(1), num_disk_entries(4);
  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  KeyValueVector key_value_pairs(PopulateDataBuffer(num_entries, num_memory_entries,
                                                    num_disk_entries, test_path, pop_functor_));
  NonEmptyString value, recovered;

  KeyType first_key(key_value_pairs[0].first), second_key(key_value_pairs[1].first);
  value = NonEmptyString(RandomAlphaNumericString(static_cast<uint32_t>(2 * OneKB)));
  auto key(GenerateKeyFromValue<KeyType>(value));
  auto async =
    std::async(std::launch::async, [this, key, value] { data_buffer_->Store(key, value); });
  REQUIRE_THROWS_AS(recovered = data_buffer_->Get(key), std::exception);
  REQUIRE_NOTHROW(data_buffer_->Delete(first_key));
  REQUIRE_NOTHROW(data_buffer_->Delete(second_key));
  REQUIRE_NOTHROW(async.wait());
  REQUIRE_NOTHROW(recovered = data_buffer_->Get(key));
  REQUIRE(recovered == value);
  REQUIRE(DeleteDirectory(data_buffer_path_));
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferPopOnDiskBufferOverfill", "[Private][Behavioural]") {
  size_t index(0);
  std::mutex mutex;
  std::condition_variable condition_variable;
  KeyValueVector key_value_pairs;
  PopFunctor pop_functor([&](const KeyType& key, const NonEmptyString & value) {
    PopFunction(key, value, key_value_pairs, index, mutex, condition_variable);
  });
  const size_t num_entries(4), num_memory_entries(1), num_disk_entries(4);
  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  key_value_pairs = PopulateDataBuffer(num_entries, num_memory_entries, num_disk_entries,
                                       test_path, pop_functor);
  REQUIRE(0 == index);

  NonEmptyString value, recovered;
  value = NonEmptyString(RandomAlphaNumericString(static_cast<uint32_t>(OneKB)));
  auto key(GenerateKeyFromValue<KeyType>(value));
  // Trigger pop.
  REQUIRE_NOTHROW(data_buffer_->Store(key, value));
  REQUIRE_NOTHROW(recovered = data_buffer_->Get(key));
  REQUIRE(recovered == value);
  {
    std::unique_lock<std::mutex> lock(mutex);
    auto result(condition_variable.wait_for(lock, std::chrono::seconds(1),
                                            [&]()->bool { return index == 1; }));
    REQUIRE(result);
  }
  REQUIRE(1 == index);

  value = NonEmptyString(std::string(RandomAlphaNumericString(static_cast<uint32_t>(2 * OneKB))));
  key = GenerateKeyFromValue<KeyType>(value);
  // Trigger pop.
  REQUIRE_NOTHROW(data_buffer_->Store(key, value));
  {
    std::unique_lock<std::mutex> lock(mutex);
    auto result(condition_variable.wait_for(lock, std::chrono::seconds(2),
                                            [&]()->bool { return index == 3; }));
    REQUIRE(result);
  }
  REQUIRE(3 == index);
  REQUIRE_NOTHROW(recovered = data_buffer_->Get(key));
  REQUIRE(recovered == value);
  REQUIRE(DeleteDirectory(data_buffer_path_));
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferAsyncDeleteOnDiskBufferOverfill",
                 "[Private][Behavioural]") {
  KeyValueVector old_key_value_pairs, new_key_value_pairs;
  const size_t num_entries(6), num_memory_entries(0), num_disk_entries(6);
  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  old_key_value_pairs = PopulateDataBuffer(num_entries, num_memory_entries, num_disk_entries,
                                           test_path, pop_functor_);

  NonEmptyString value, recovered;
  KeyType key;
  while (new_key_value_pairs.size() < num_entries) {
    value = NonEmptyString(std::string(RandomAlphaNumericString(static_cast<uint32_t>(OneKB))));
    key = GenerateKeyFromValue<KeyType>(value);
    new_key_value_pairs.push_back(std::make_pair(key, value));
  }

  std::vector<std::future<void>> async_stores;
  for (auto key_value : new_key_value_pairs) {
    value = key_value.second;
    key = key_value.first;
    async_stores.push_back(std::async(
        std::launch::async, [this, key, value] { data_buffer_->Store(key, value); }));
  }

  // Check the new Store attempts all block pending some Deletes
  for (auto& async_store : async_stores) {
    auto status(async_store.wait_for(std::chrono::milliseconds(250)));
    REQUIRE(std::future_status::timeout == status);
  }

  std::vector<std::future<NonEmptyString>> async_gets;
  for (auto key_value : new_key_value_pairs) {
    async_gets.push_back(std::async(std::launch::async, [this, key_value] {
      return data_buffer_->Get(key_value.first);
    }));
  }

  // Check Get attempts for the new Store values all block pending the Store attempts completing
  for (auto& async_get : async_gets) {
    auto status(async_get.wait_for(std::chrono::milliseconds(100)));
    REQUIRE(std::future_status::timeout == status);
  }

  // Delete the last new Store attempt before it has completed
  REQUIRE_NOTHROW(data_buffer_->Delete(new_key_value_pairs.back().first));
  // Delete the old values to allow the new Store attempts to complete
  for (auto key_value : old_key_value_pairs) {
    REQUIRE_NOTHROW(data_buffer_->Delete(key_value.first));
  }

  for (size_t i(0); i != num_entries - 1; ++i) {
    auto status(async_gets[i].wait_for(std::chrono::milliseconds(200)));
    REQUIRE(std::future_status::ready == status);
    REQUIRE_NOTHROW(recovered = async_gets[i].get());
    REQUIRE(new_key_value_pairs[i].second == recovered);
  }

  auto status(async_gets.back().wait_for(std::chrono::milliseconds(100)));
  REQUIRE(std::future_status::ready == status);
  REQUIRE_THROWS_AS(async_gets.back().get(), std::exception);
  REQUIRE(DeleteDirectory(this->data_buffer_path_));
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferAsyncPopOnDiskBufferOverfill",
                 "[Private][Behavioural]") {
  size_t index(0);
  std::mutex mutex;
  std::condition_variable condition_variable;
  KeyValueVector old_key_value_pairs, new_key_value_pairs;
  PopFunctor pop_functor([&](const KeyType& key, const NonEmptyString & value) {
    PopFunction(key, value, old_key_value_pairs, index, mutex, condition_variable);
  });
  const size_t num_entries(6), num_memory_entries(1), num_disk_entries(6);
  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  old_key_value_pairs = PopulateDataBuffer(num_entries, num_memory_entries, num_disk_entries,
                                           test_path, pop_functor);
  REQUIRE(0 == index);

  NonEmptyString value, recovered;
  KeyType key;
  while (new_key_value_pairs.size() < num_entries) {
    value = NonEmptyString(std::string(RandomAlphaNumericString(static_cast<uint32_t>(OneKB))));
    key = GenerateKeyFromValue<KeyType>(value);
    new_key_value_pairs.push_back(std::make_pair(key, value));
  }

  std::vector<std::future<void>> async_operations;
  for (auto key_value : new_key_value_pairs) {
    value = key_value.second;
    key = key_value.first;
    async_operations.push_back(std::async(
        std::launch::async, [this, key, value] { this->data_buffer_->Store(key, value); }));
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    auto result(condition_variable.wait_for(
        lock, std::chrono::seconds(2), [&]()->bool { return index == num_entries; }));
    REQUIRE(result);
  }
  for (auto key_value : new_key_value_pairs) {
    REQUIRE_NOTHROW(recovered = this->data_buffer_->Get(key_value.first));
    REQUIRE(key_value.second == recovered);
  }
  REQUIRE(num_entries == index);
  REQUIRE(DeleteDirectory(data_buffer_path_));
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferRepeatedlyStoreUsingSameKey",
                 "[Private][Behavioural]") {
  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  data_buffer_path_ = fs::path(*test_path / "data_buffer");
  PopFunctor pop_functor([this](const KeyType& key, const NonEmptyString & value) {
    LOG(kInfo) << "Pop called on " << DebugKeyName(key) << "with value "
               << HexSubstr(value.string());
  });
  data_buffer_.reset(new DataBufferType(MemoryUsage(kDefaultMaxMemoryUsage),
                                        DiskUsage(kDefaultMaxDiskUsage), pop_functor,
                                        data_buffer_path_));
  NonEmptyString value(RandomAlphaNumericString((RandomUint32() % 30) + 1)), recovered, last_value;
  auto key(GenerateKeyFromValue<KeyType>(value));
  auto async = std::async(std::launch::async,
                          [this, key, value] { data_buffer_->Store(key, value); });
  REQUIRE_NOTHROW(async.wait());
  REQUIRE(true == async.valid());
  REQUIRE_NOTHROW(async.get());
  REQUIRE_NOTHROW(recovered = data_buffer_->Get(key));
  REQUIRE(recovered == value);

  uint32_t events(RandomUint32() % 100);
  for (uint32_t i = 0; i != events; ++i) {
    last_value = value;
    while (last_value == value)
      last_value = NonEmptyString(RandomAlphaNumericString((RandomUint32() % 30) + 1));
    auto async =
        std::async(std::launch::async,
                   [this, key, last_value] { data_buffer_->Store(key, last_value); });
    REQUIRE_NOTHROW(async.wait());
    REQUIRE(true == async.valid());
    REQUIRE_NOTHROW(async.get());
  }
  Sleep(std::chrono::milliseconds(100));
  REQUIRE_NOTHROW(recovered = data_buffer_->Get(key));
  REQUIRE(value != recovered);
  REQUIRE(last_value == recovered);
  data_buffer_.reset();
  REQUIRE(DeleteDirectory(data_buffer_path_));
}

TEST_CASE_METHOD(DataBufferTest, "DataBufferRandomAsync", "[Private][Behavioural]") {
  maidsafe::test::TestPath test_path(maidsafe::test::CreateTestPath("MaidSafe_Test_DataBuffer"));
  data_buffer_path_ = fs::path(*test_path / "data_buffer");
  PopFunctor pop_functor([this](const KeyType& key, const NonEmptyString & value) {
    LOG(kInfo) << "Pop called on " << DebugKeyName(key) << "with value "
               << HexSubstr(value.string());
  });
  data_buffer_.reset(new DataBufferType(MemoryUsage(kDefaultMaxMemoryUsage),
                                        DiskUsage(kDefaultMaxDiskUsage), pop_functor,
                                        data_buffer_path_));

  KeyValueVector key_value_pairs;
  uint32_t events(RandomUint32() % 500);
  std::vector<std::future<void>> future_stores, future_deletes;
  std::vector<std::future<NonEmptyString>> future_gets;

  for (uint32_t i = 0; i != events; ++i) {
    NonEmptyString value(RandomAlphaNumericString((RandomUint32() % 300) + 1));
    auto key(GenerateKeyFromValue<KeyType>(value));
    key_value_pairs.push_back(std::make_pair(key, value));

    uint32_t event(RandomUint32() % 3);
    switch (event) {
      case 0: {
        if (!key_value_pairs.empty()) {
          KeyType event_key(key_value_pairs[RandomUint32() % key_value_pairs.size()].first);
          future_deletes.push_back(
              std::async([this, event_key] { data_buffer_->Delete(event_key); }));
        } else {
          future_deletes.push_back(std::async([this, key] { data_buffer_->Delete(key); }));
        }
        break;
      }
      case 1: {
        // uint32_t index(RandomUint32() % key_value_pairs.size());
        uint32_t index(i);
        KeyType event_key(key_value_pairs[index].first);
        NonEmptyString event_value(key_value_pairs[index].second);
        future_stores.push_back(std::async([this, event_key, event_value] {
            data_buffer_->Store(event_key, event_value);
        }));
        break;
      }
      case 2: {
        if (!key_value_pairs.empty()) {
          KeyType event_key(key_value_pairs[RandomUint32() % key_value_pairs.size()].first);
          future_gets.push_back(
              std::async([this, event_key] { return data_buffer_->Get(event_key); }));
        } else {
          future_gets.push_back(std::async([this, key] { return data_buffer_->Get(key); }));
        }
        break;
      }
    }
  }

  for (auto& future_store : future_stores) {
    REQUIRE_NOTHROW(future_store.get());
  }

  for (auto& future_delete : future_deletes) {
    try {
      future_delete.get();
    }
    catch (const std::exception&) {
    }
  }

  for (auto& future_get : future_gets) {
    try {
      NonEmptyString value(future_get.get());
      typedef KeyValueVector::value_type value_type;
      auto it = std::find_if(
          key_value_pairs.begin(), key_value_pairs.end(),
          [this, &value](const value_type & key_value) { return key_value.second == value; });
      REQUIRE(key_value_pairs.end() != it);
    }
    catch (const std::exception& e) {
      std::string msg(e.what());
      LOG(kInfo) << msg;
    }
  }
  // Need to destroy data_buffer_ so that test_path can be deleted.
  data_buffer_.reset();
}


class DataBufferValueParameterisedTest {
 protected:
  typedef DataNameVariant KeyType;
  typedef DataBuffer<KeyType> DataBufferType;
  typedef DataBufferType::PopFunctor PopFunctorType;
  typedef std::unique_ptr<DataBufferType> DataBufferPtr;

  DataBufferValueParameterisedTest()
      : pop_functor_(),
        data_buffer_() {}

  PopFunctorType pop_functor_;
  DataBufferPtr data_buffer_;
};

namespace {

struct MaxDataBufferUsage { uint64_t memory_usage, disk_usage; };
MaxDataBufferUsage max_data_buffer_usage[] = {{1, 2}, {1, 1024}, {8, 1024}, {1024, 2048},
                                              {1024, 1024}, {16, 16 * 1024}, {32, 32},
                                              {1000, 10000}, {10000, 1000000}};
}  // unnamed namespace

TEST_CASE_METHOD(DataBufferValueParameterisedTest, "DataBufferValueParameterisedStore",
                 "[Private][Behavioural]") {
  MaxDataBufferUsage* resource_usage = Catch::Generators::GENERATE(between(max_data_buffer_usage,
      &max_data_buffer_usage[sizeof(max_data_buffer_usage)/sizeof(MaxDataBufferUsage)-1]));

  uint64_t memory_usage(resource_usage->memory_usage), disk_usage(resource_usage->disk_usage),
           total_usage(disk_usage + memory_usage);

  MemoryUsage max_memory_usage(memory_usage);
  DiskUsage max_disk_usage(disk_usage);
  data_buffer_.reset(new DataBufferType(max_memory_usage, max_disk_usage, pop_functor_));

  while (total_usage != 0) {
    NonEmptyString value(std::string(RandomAlphaNumericString(
                      static_cast<uint32_t>(resource_usage->memory_usage))));
    KeyType key(GenerateKeyFromValue<KeyType>(value));
    REQUIRE_NOTHROW(data_buffer_->Store(key, value));
    NonEmptyString recovered;
    REQUIRE_NOTHROW(recovered = data_buffer_->Get(key));
    REQUIRE(value == recovered);
    if (disk_usage != 0) {
      disk_usage -= resource_usage->memory_usage;
      total_usage -= resource_usage->memory_usage;
    } else {
      total_usage -= resource_usage->memory_usage;
    }
  }
}

TEST_CASE_METHOD(DataBufferValueParameterisedTest, "DataBufferValueParameterisedDelete",
                 "[Private][Behavioural]") {
  MaxDataBufferUsage* resource_usage = Catch::Generators::GENERATE(between(max_data_buffer_usage,
      &max_data_buffer_usage[sizeof(max_data_buffer_usage)/sizeof(MaxDataBufferUsage)-1]));

  uint64_t memory_usage(resource_usage->memory_usage), disk_usage(resource_usage->disk_usage),
           total_usage(disk_usage + memory_usage);

  MemoryUsage max_memory_usage(memory_usage);
  DiskUsage max_disk_usage(disk_usage);
  data_buffer_.reset(new DataBufferType(max_memory_usage, max_disk_usage, pop_functor_));

  std::map<KeyType, NonEmptyString> key_value_pairs;
  while (total_usage != 0) {
    NonEmptyString value(
        std::string(RandomAlphaNumericString(static_cast<uint32_t>(resource_usage->memory_usage))));
    KeyType key(GenerateKeyFromValue<KeyType>(value));
    key_value_pairs[key] = value;
    REQUIRE_NOTHROW(data_buffer_->Store(key, value));
    if (disk_usage != 0) {
      disk_usage -= resource_usage->memory_usage;
      total_usage -= resource_usage->memory_usage;
    } else {
      total_usage -= resource_usage->memory_usage;
    }
  }
  NonEmptyString recovered;
  for (auto key_value : key_value_pairs) {
    KeyType key(key_value.first);
    REQUIRE_NOTHROW(recovered = data_buffer_->Get(key));
    REQUIRE(key_value.second == recovered);
    REQUIRE_NOTHROW(data_buffer_->Delete(key));
    REQUIRE_THROWS_AS(recovered = data_buffer_->Get(key), std::exception);
  }
}

}  // namespace test

}  // namespace data_store
}  // namespace maidsafe
