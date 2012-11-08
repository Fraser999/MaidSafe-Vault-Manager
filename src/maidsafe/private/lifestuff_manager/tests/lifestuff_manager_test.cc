/***************************************************************************************************
 *  Copyright 2012 maidsafe.net limited                                                            *
 *                                                                                                 *
 *  The following source code is property of MaidSafe.net limited and is not meant for external    *
 *  use. The use of this code is governed by the licence file licence.txt found in the root of     *
 *  this directory and also on www.maidsafe.net.                                                   *
 *                                                                                                 *
 *  You are not free to copy, amend or otherwise use this source code without the explicit written *
 *  permission of the board of directors of MaidSafe.net.                                          *
 **************************************************************************************************/

#include "boost/date_time/posix_time/posix_time_duration.hpp"
#include "boost/algorithm/string.hpp"

#include "maidsafe/common/error.h"
#include "maidsafe/common/log.h"
#include "maidsafe/common/test.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/private/lifestuff_manager/client_controller.h"
#include "maidsafe/private/lifestuff_manager/vault_controller.h"
#include "maidsafe/private/lifestuff_manager/lifestuff_manager.h"
#include "maidsafe/private/lifestuff_manager/vault_info_pb.h"
#include "maidsafe/private/lifestuff_manager/utils.h"


namespace bptime = boost::posix_time;
namespace fs = boost::filesystem;

namespace maidsafe {

namespace priv {

namespace lifestuff_manager {

namespace test {

namespace {

int GetNumRunningProcesses() {
  std::string dummy(detail::kDummyName);
#ifdef MAIDSAFE_WIN32
  std::string command("tasklist /fi \"imagename eq " + dummy + ".exe\" /nh > process_count.txt");
#else
  std::string command("ps -ef | grep " + dummy + " | grep -v grep | wc -l > process_count.txt");
#endif
  int result(system(command.c_str()));
  if (result != 0) {
    LOG(kError) << "Failed to execute command that checks processes: " << command;
    return -1;
  }

  try {
#ifdef MAIDSAFE_WIN32
    int num_processes(0);
    char process_info[256];
    std::streamsize number_of_characters(256);
    fs::path file_path(fs::path(".") / "process_count.txt");
    std::ifstream file(file_path.string().c_str(), std::ios_base::binary);
    if (!file.good())
      return num_processes;
    while (file.getline(process_info, number_of_characters))
      ++num_processes;
    num_processes -= 1;
#else
    std::string process_string;
    ReadFile(fs::path(".") / "process_count.txt", &process_string);
    boost::trim(process_string);
    // In UNIX, adjust for the two extra commands containing kDUmmyName that we invoked - the
    // overall ps and the piped grep
    int num_processes(boost::lexical_cast<int>(process_string));
#endif
    return num_processes;
  }
  catch(const std::exception& e) {
    LOG(kError) << e.what();
    return -1;
  }
}

}  // namespace

TEST(LifeStuffManagerTest, FUNC_StartStop) {
//  maidsafe::log::Logging::instance().AddFilter("private", maidsafe::log::kVerbose);

  // test case for startup (non-existent config file)
  boost::system::error_code error_code;
  {
    if (fs::exists(GetUserAppDir() / detail::kGlobalConfigFilename, error_code))
      fs::remove(GetUserAppDir() / detail::kGlobalConfigFilename, error_code);
    ASSERT_FALSE(fs::exists(GetUserAppDir() / detail::kGlobalConfigFilename, error_code));
    LifeStuffManager lifestuff_manager;
//    Sleep(boost::posix_time::seconds(20));
    ClientController client_controller([](const NonEmptyString&){});  // NOLINT (Fraser)
    int max_seconds = LifeStuffManager::kMaxUpdateInterval().total_seconds();
    EXPECT_FALSE(client_controller.SetUpdateInterval(bptime::seconds(max_seconds + 1)));
    EXPECT_TRUE(client_controller.SetUpdateInterval(bptime::seconds(max_seconds)));
    int min_seconds = LifeStuffManager::kMinUpdateInterval().total_seconds();
    EXPECT_TRUE(client_controller.SetUpdateInterval(bptime::seconds(min_seconds)));
    EXPECT_FALSE(client_controller.SetUpdateInterval(bptime::seconds(min_seconds - 1)));
    Sleep(boost::posix_time::seconds(2));
    EXPECT_TRUE(fs::exists(GetUserAppDir() / detail::kGlobalConfigFilename, error_code));
    EXPECT_EQ(0, GetNumRunningProcesses());
  }
  std::string config_contents;
  maidsafe::ReadFile(GetUserAppDir() / detail::kGlobalConfigFilename, &config_contents);
  protobuf::LifeStuffManagerConfig lifestuff_manager_config;
  lifestuff_manager_config.ParseFromString(config_contents);
  EXPECT_EQ(0, lifestuff_manager_config.vault_info_size());

  // test case for existing config file with minimum content (generated in previous test case)
  // One vault is started. This should then be shut down and saved to the config file when the
  // LifeStuffManager is destroyed.
  Fob first_fob;
  {
    LifeStuffManager lifestuff_manager;
    ClientController client_controller([](const maidsafe::NonEmptyString&){});  // NOLINT (Fraser)
    first_fob = utils::GenerateFob(nullptr);
    EXPECT_TRUE(client_controller.StartVault(first_fob, first_fob.identity.string(), ""));
    Sleep(boost::posix_time::seconds(1));
    EXPECT_EQ(1, GetNumRunningProcesses());
    Sleep(boost::posix_time::seconds(1));
    EXPECT_TRUE(fs::exists(GetUserAppDir() / detail::kGlobalConfigFilename, error_code));
  }
  EXPECT_EQ(0, GetNumRunningProcesses());
  config_contents = "";
  maidsafe::ReadFile(GetUserAppDir() / detail::kGlobalConfigFilename, &config_contents);
  lifestuff_manager_config.ParseFromString(config_contents);
  EXPECT_EQ(1, lifestuff_manager_config.vault_info_size());

  protobuf::Bootstrap end_points(lifestuff_manager_config.bootstrap_endpoints());
  int max_index(end_points.bootstrap_contacts_size());
  int endpoint_matches(0);
  for (int n(0); n < max_index; ++n) {
      if (end_points.bootstrap_contacts(n).ip().compare("127.0.0.46") &&
          (end_points.bootstrap_contacts(n).port() == 3658)) {
        ++endpoint_matches;
      }
  }
  EXPECT_GE(1, endpoint_matches);

  // test case for existing config file with one vault (generated in previous test case)
  // Two vaults are started - one by config, one by a client. They should then be shut down and
  // both saved to the config file when the LifeStuffManager is destroyed.
  Fob second_fob(utils::GenerateFob(nullptr));
  {
    LifeStuffManager lifestuff_manager;
    ClientController client_controller([](const NonEmptyString&){});  // NOLINT (Fraser)
    EXPECT_TRUE(client_controller.StartVault(second_fob, "G", ""));
    Sleep(boost::posix_time::seconds(2));
    EXPECT_EQ(2, GetNumRunningProcesses());
    Sleep(boost::posix_time::seconds(1));
    EXPECT_TRUE(fs::exists(GetUserAppDir() / detail::kGlobalConfigFilename, error_code));
    std::vector<std::pair<std::string, uint16_t> > bootstrap_endpoints;
    client_controller.GetBootstrapNodes(bootstrap_endpoints);
    endpoint_matches = 0;
    for (size_t n(0); n < bootstrap_endpoints.size(); ++n) {
        if (bootstrap_endpoints[n].first.compare("127.0.0.46") &&
            (bootstrap_endpoints[n].second == 3658)) {
          ++endpoint_matches;
        }
    }
    EXPECT_GE(1, endpoint_matches);
  }
  EXPECT_EQ(0, GetNumRunningProcesses());
  config_contents = "";
  maidsafe::ReadFile(GetUserAppDir() / detail::kGlobalConfigFilename, &config_contents);
  lifestuff_manager_config.ParseFromString(config_contents);
  EXPECT_EQ(2, lifestuff_manager_config.vault_info_size());

  // test case for existing config file with two vaults (generated in previous test case)
  // Two vaults are started - both by config. One is then shut down by the client and one is shut
  // down when the LifeStuffManager is destroyed. both should saved to the config file.
  {
    LifeStuffManager lifestuff_manager;
    ClientController client_controller([](const NonEmptyString&){});  // NOLINT (Fraser)
    Sleep(boost::posix_time::seconds(2));
    EXPECT_EQ(2, GetNumRunningProcesses());
    asymm::PlainText data(RandomString(64));
    asymm::Signature signature1(asymm::Sign(data, first_fob.keys.private_key));
    asymm::Signature signature2(asymm::Sign(data, second_fob.keys.private_key));
    EXPECT_TRUE(client_controller.StopVault(data, signature1, first_fob.identity));
    Sleep(boost::posix_time::seconds(2));
    EXPECT_EQ(1, GetNumRunningProcesses());
  }
  EXPECT_EQ(0, GetNumRunningProcesses());
  config_contents = "";
  maidsafe::ReadFile(GetUserAppDir() / detail::kGlobalConfigFilename, &config_contents);
  lifestuff_manager_config.ParseFromString(config_contents);
  ASSERT_EQ(2, lifestuff_manager_config.vault_info_size());
  int run_count(0);
  if (lifestuff_manager_config.vault_info(0).requested_to_run())
    ++run_count;
  if (lifestuff_manager_config.vault_info(1).requested_to_run())
    ++run_count;
  EXPECT_EQ(1, run_count);

  // test case for existing config file with two vaults (generated in previous test case, one
  // deactivated). One vault is started by config. Two clients are then used to start 30 new vaults.
  {
    EXPECT_EQ(0, GetNumRunningProcesses());
    config_contents.clear();
    maidsafe::ReadFile(GetUserAppDir() / detail::kGlobalConfigFilename, &config_contents);
    lifestuff_manager_config.ParseFromString(config_contents);
    EXPECT_EQ(2, lifestuff_manager_config.vault_info_size());
    LifeStuffManager lifestuff_manager;
    Sleep(boost::posix_time::seconds(2));
    EXPECT_EQ(1, GetNumRunningProcesses());
    ClientController client_controller1([](const NonEmptyString&) {}),
                     client_controller2([](const NonEmptyString&) {});  // NOLINT (Fraser)

    for (int i(0); i < 50; ++i) {
      Fob fob(utils::GenerateFob(nullptr));
      if (i % 2 == 0)
        EXPECT_TRUE(client_controller1.StartVault(fob, fob.identity.string(), ""));
      else
        EXPECT_TRUE(client_controller2.StartVault(fob, fob.identity.string(), ""));
    }
    EXPECT_EQ(51, GetNumRunningProcesses());
  }
  EXPECT_EQ(0, GetNumRunningProcesses());
  config_contents = "";
  maidsafe::ReadFile(GetUserAppDir() / detail::kGlobalConfigFilename, &config_contents);
  lifestuff_manager_config.ParseFromString(config_contents);
  EXPECT_EQ(52, lifestuff_manager_config.vault_info_size());
  boost::system::error_code error;
  fs::remove(GetUserAppDir() / detail::kGlobalConfigFilename, error);
}

}  // namespace test

}  // namespace lifestuff_manager

}  // namespace priv

}  // namespace maidsafe