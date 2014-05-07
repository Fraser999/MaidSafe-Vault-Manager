/*  Copyright 2014 MaidSafe.net limited

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

#include "maidsafe/vault_manager/vault_manager.h"

//#include <chrono>
//#include <iostream>
//
//#include "boost/filesystem/path.hpp"
//#include "boost/filesystem/operations.hpp"
//
#include "maidsafe/common/application_support_directories.h"
#include "maidsafe/common/error.h"
#include "maidsafe/common/log.h"
#include "maidsafe/common/process.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/passport/passport.h"

//#include "maidsafe/vault_manager/controller_messages.pb.h"
#include "maidsafe/vault_manager/tcp_connection.h"
#include "maidsafe/vault_manager/utils.h"
#include "maidsafe/vault_manager/vault_info.pb.h"

namespace fs = boost::filesystem;

namespace maidsafe {

namespace vault_manager {

namespace {

fs::path GetPath(const fs::path& p) {
#ifdef TESTING
  return (GetTestEnvironmentRootDir().empty() ? GetUserAppDir() : GetTestEnvironmentRootDir()) / p;
#else
  return GetSystemAppSupportDir() / p;
#endif
}

fs::path GetConfigFilePath() {
  return GetPath(kConfigFilename);
}

fs::path GetDefaultChunkstorePath() {
  return GetPath(kChunkstoreDirname);
}

fs::path GetVaultExecutablePath() {
#ifdef TESTING
  if (!GetPathToVault().empty())
    return GetPathToVault();
#endif
  return process::GetOtherExecutablePath(fs::path{ "vault" });
}

Port GetInitialLocalPort() {
#ifdef TESTING
  return GetTestVaultManagerPort() == 0 ? kLivePort + 100 : GetTestVaultManagerPort();
#else
  return kDefaultPort();
#endif
}

}  // unnamed namespace

VaultManager::VaultManager()
    : symm_key_(),
      symm_iv_(),
      config_file_path_(GetConfigFilePath()),
      vault_executable_path_(GetVaultExecutablePath()),
      listener_([this](TcpConnectionPtr connection) { HandleNewConnection(connection); },
          GetInitialLocalPort()),
      process_manager_(),
      client_connection_() {
  boost::system::error_code error_code;
  if (!fs::exists(config_file_path_, error_code) ||
    error_code.value() == boost::system::errc::no_such_file_or_directory) {
    CreateConfigFile();
  }
  ReadConfigFileAndStartVaults();
  LOG(kInfo) << "VaultManager started";
}

VaultManager::~VaultManager() {
                                                                                                      //  std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 1" << std::endl;
                                                                                                      //  need_to_stop_ = true;
                                                                                                      //  std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 2" << std::endl;
                                                                                                      //  process_manager_.LetAllProcessesDie();
                                                                                                      //  std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 3" << std::endl;
                                                                                                      //  StopAllVaults();
                                                                                                      //  std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 4" << std::endl;
                                                                                                      //  {
                                                                                                      //    std::lock_guard<std::mutex> lock(update_mutex_);
                                                                                                      //    std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 5" << std::endl;
                                                                                                      //    update_timer_.cancel();
                                                                                                      //    std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 6" << std::endl;
                                                                                                      //  }
                                                                                                      //  std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 7" << std::endl;
                                                                                                      //  transport_->StopListening();
                                                                                                      //  std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 8" << std::endl;
                                                                                                      //  asio_service_.Stop();
                                                                                                      //  std::cout << "~~~~~~~~~~~~~~~~~~~~~~ 9" << std::endl;
}

void VaultManager::CreateConfigFile() {
  symm_key_ = crypto::AES256Key{ RandomString(crypto::AES256_KeySize) };
  symm_iv_ = crypto::AES256InitialisationVector{ RandomString(crypto::AES256_IVSize) };
  protobuf::VaultManagerConfig config;
  config.set_aes256key(symm_key_.string());
  config.set_aes256iv(symm_iv_.string());

  boost::system::error_code error_code;
  if (!fs::exists(config_file_path_.parent_path(), error_code)) {
    if (!fs::create_directories(config_file_path_.parent_path(), error_code) || error_code) {
      LOG(kError) << "Failed to create directories for config file " << config_file_path_ << ": "
                  << error_code.message();
      BOOST_THROW_EXCEPTION(MakeError(CommonErrors::filesystem_io_error));
    }
  }

  if (!WriteFile(config_file_path_, config.SerializeAsString())) {
    LOG(kError) << "Failed to create config file " << config_file_path_;
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::filesystem_io_error));
  }
  LOG(kInfo) << "Created config file " << config_file_path_;
}

void VaultManager::ReadConfigFileAndStartVaults() {
  NonEmptyString content{ ReadFile(config_file_path_) };

  protobuf::VaultManagerConfig config;
  if (!config.ParseFromString(content.string())) {
    LOG(kError) << "Failed to parse config file " << config_file_path_;
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::parsing_error));
  }

  symm_key_ = crypto::AES256Key{ config.aes256key() };
  symm_iv_ = crypto::AES256InitialisationVector{ config.aes256iv() };

  if (config.vault_info_size()) {
    for (int i(0); i != config.vault_info_size(); ++i) {
      VaultInfo vault_info;
      FromProtobuf(symm_key_, symm_iv_, config.vault_info(i), vault_info);
      SetExecutablePath(vault_executable_path_, vault_info);
      process_manager_.AddProcess(std::move(vault_info));
    }
  } else {
    // If the config file is empty
    VaultInfo vault_info;
    vault_info.chunkstore_path = GetDefaultChunkstorePath();
    SetExecutablePath(vault_executable_path_, vault_info);
    process_manager_.AddProcess(std::move(vault_info));
  }
}

void VaultManager::WriteConfigFile() const {
  protobuf::VaultManagerConfig config;
  config.set_aes256key(symm_key_.string());
  config.set_aes256iv(symm_iv_.string());
  process_manager_.WriteToConfigFile(symm_key_, symm_iv_, config);

  if (!WriteFile(config_file_path_, config.SerializeAsString())) {
    LOG(kError) << "Failed to write config file " << config_file_path_;
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::filesystem_io_error));
  }
}

void VaultManager::HandleNewConnection(TcpConnectionPtr connection) {
  MessageReceivedFunctor on_message{ [=](const std::string& message) {
    HandleReceivedMessage(connection, message);
  } };
  ConnectionClosedFunctor on_closed{ [=] {
    process_manager_.HandleConnectionClosed(connection);
  } };
  connection->Start(on_message, on_closed);
}

void VaultManager::HandleReceivedMessage(TcpConnectionPtr connection,
                                         const std::string& wrapped_message) {
  try {
    MessageAndType message_and_type{ UnwrapMessage(wrapped_message) };
    LOG(kVerbose) << "Received " << message_and_type.second;
    std::string response;
    switch (message_and_type.second) {
      case MessageType::kVaultStarted:
        HandleVaultStarted(connection, message_and_type.first, response);
        break;
      //case MessageType::kClientRegistrationRequest:
      //  HandleClientRegistrationRequest(payload, response);
      //  break;
      //case MessageType::kStartVaultRequest:
      //  HandleStartVaultRequest(payload, response);
      //  break;
      //case MessageType::kVaultIdentityRequest:
      //  HandleVaultIdentityRequest(payload, response);
      //  break;
      //case MessageType::kVaultJoinedNetwork:
      //  HandleVaultJoinedNetworkRequest(payload, response);
      //  break;
      //case MessageType::kStopVaultRequest:
      //  HandleStopVaultRequest(payload, response);
      //  break;
      //case MessageType::kUpdateIntervalRequest:
      //  HandleUpdateIntervalRequest(payload, response);
      //  break;
      //case MessageType::kSendEndpointToVaultManagerRequest:
      //  HandleSendEndpointToVaultManagerRequest(payload, response);
      //  break;
      //case MessageType::kBootstrapRequest:
      //  HandleBootstrapRequest(payload, response);
      //  break;
      default:
        return;
    }
    if (!response.empty())
      connection->Send(response);
  }
  catch (const std::exception& e) {
    LOG(kError) << "Failed to handle incoming message: " << boost::diagnostic_information(e);
  }
}

void VaultManager::HandleVaultStarted(TcpConnectionPtr connection, const std::string& /*request*/,
                                      std::string& /*response*/) {
  process_manager_.HandleNewConnection(connection);
}

/*
void VaultManager::HandleClientRegistrationRequest(const std::string& request,
                                                       std::string& response) {
  protobuf::ClientRegistrationRequest client_request;
  if (!client_request.ParseFromString(request)) {  // Silently drop
    LOG(kError) << "Failed to parse client registration request.";
    return;
  }

  Port request_port(static_cast<Port>(client_request.listening_port()));
  {
    std::lock_guard<std::mutex> lock(client_ports_mutex_);
    client_ports_and_versions_[request_port] = client_request.version();
  }

  protobuf::ClientRegistrationResponse client_response;
  if (endpoints_.empty()) {
    protobuf::VaultManagerConfig config;
    if (!ReadFileToVaultManagerConfig(config_file_path_, config)) {
      // TODO(Team): Should have counter for failures to trigger recreation?
      LOG(kError) << "Failed to read & parse config file " << config_file_path_;
    }
    if (!ObtainBootstrapInformation(config)) {
      LOG(kError) << "Failed to get endpoints from bootstrap server";
    } else {
      std::lock_guard<std::mutex> lock(config_file_mutex_);
      if (!WriteFile(config_file_path_, config.SerializeAsString())) {
        LOG(kError) << "Failed to write config file after obtaining bootstrap info.";
      }
    }
  } else {
    std::for_each(endpoints_.begin(), endpoints_.end(),
                  [&client_response](const EndPoint & element) {
      client_response.add_bootstrap_endpoint_ip(element.first);
      client_response.add_bootstrap_endpoint_port(element.second);
    });
  }

  LOG(kVerbose) << "Version that we might inform the user "
                << download_manager_.latest_remote_version();
  LOG(kVerbose) << "Version that the user reported " << client_request.version();

  if (client_request.version() < VersionToInt(download_manager_.latest_remote_version()))
    client_response.set_path_to_new_installer(latest_local_installer_path_.string());

  response = detail::WrapMessage(MessageType::kClientRegistrationResponse,
                                 client_response.SerializeAsString());
}

void VaultManager::HandleStartVaultRequest(const std::string& request, std::string& response) {
  protobuf::StartVaultRequest start_vault_request;
  if (!start_vault_request.ParseFromString(request)) {
    // Silently drop
    LOG(kError) << "Failed to parse StartVaultRequest.";
    return;
  }

  auto set_response([&response](bool result) {
    protobuf::StartVaultResponse start_vault_response;
    start_vault_response.set_result(result);
    response = detail::WrapMessage(MessageType::kStartVaultResponse,
                                   start_vault_response.SerializeAsString());
  });

  Port client_port(static_cast<Port>(start_vault_request.client_port()));
  {
    std::lock_guard<std::mutex> lock(client_ports_mutex_);
    auto client_itr(client_ports_and_versions_.find(client_port));
    if (client_itr == client_ports_and_versions_.end()) {
      LOG(kError) << "Client is not registered with VaultManager.";
      return set_response(false);
    }
  }

  VaultInfoPtr vault_info(std::make_shared<VaultInfo>());
#ifdef TESTING
  std::cout << "Vault index to pass to vault: " << start_vault_request.identity_index()
            << std::endl;
  vault_info->identity_index = start_vault_request.identity_index();
#endif
  passport::Pmid request_pmid(
      passport::detail::ParsePmid(NonEmptyString(start_vault_request.pmid())));
  {
    std::lock_guard<std::mutex> lock(vault_infos_mutex_);
    auto itr(FindFromPmidName(request_pmid.name()));
    bool existing_vault(false);
    if (itr != vault_infos_.end()) {
      existing_vault = true;
      if (!asymm::CheckSignature(asymm::PlainText(start_vault_request.token()),
                                 asymm::Signature(start_vault_request.token_signature()),
                                 vault_info->pmid->public_key())) {
        LOG(kError) << "Communication from someone that does not validate as owner.";
        return set_response(false);  // TODO(Team): Drop silienty?
      }

      if (!start_vault_request.credential_change()) {
        if (!(*itr)->joined_network) {
          (*itr)->client_port = client_port;
          (*itr)->requested_to_run = true;
          process_manager_.StartProcess((*itr)->process_index);
        }
      } else {
        if ((*itr)->joined_network) {
          // TODO(Team): Stop and restart with new credentials
        } else {
          // TODO(Team): Start with new credentials
          (*itr)->account_name = start_vault_request.account_name();
          (*itr)->pmid.reset(
              new passport::Pmid(passport::ParsePmid(NonEmptyString(start_vault_request.pmid()))));
          (*itr)->client_port = client_port;
          (*itr)->requested_to_run = true;
        }
      }
    } else {
      // The vault is not already registered.
      vault_info->pmid.reset(new passport::Pmid(request_pmid));
      vault_info->account_name = start_vault_request.account_name();
      bool exists(true);
      while (exists) {
        std::string random_appendix(RandomAlphaNumericString(16));
        if (start_vault_request.has_chunkstore_path()) {
          vault_info->chunkstore_path =
              (fs::path(start_vault_request.chunkstore_path()) / random_appendix).string();
        } else {
          vault_info->chunkstore_path =
              (config_file_path_.parent_path() / random_appendix).string();
        }
        boost::system::error_code error_code;
        exists = fs::exists(vault_info->chunkstore_path, error_code);
      }
      vault_info->client_port = client_port;
      if (!StartVaultProcess(vault_info)) {
        LOG(kError) << "Failed to start a process for vault ID: "
                    << Base64Substr(vault_info->pmid->name().value);
        return set_response(false);
      }
    }
    if (!AmendVaultDetailsInConfigFile(vault_info, existing_vault)) {
      LOG(kError) << "Failed to amend details in config file for vault ID: "
                  << Base64Substr(vault_info->pmid->name().value);
      return set_response(false);
    }
  }

  set_response(true);
}

void VaultManager::HandleVaultIdentityRequest(const std::string& request,
                                                  std::string& response) {
  protobuf::VaultIdentityRequest vault_identity_request;
  if (!vault_identity_request.ParseFromString(request)) {
    // Silently drop
    LOG(kError) << "Failed to parse VaultIdentityRequest.";
    return;
  }

  protobuf::VaultIdentityResponse vault_identity_response;
  bool successful_response(false);
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  NonEmptyString serialised_pmid;
  auto itr(FindFromProcessIndex(vault_identity_request.process_index()));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with process_index " << vault_identity_request.process_index()
                << " hasn't been added.";
    successful_response = false;
    // TODO(Team): Should this be dropped silently?
  } else {
    serialised_pmid = passport::SerialisePmid(*(*itr)->pmid);
    if (endpoints_.empty()) {
      protobuf::VaultManagerConfig config;
      if (!ReadFileToVaultManagerConfig(config_file_path_, config)) {
        // TODO(Team): Should have counter for failures to trigger recreation?
        LOG(kError) << "Failed to read & parse config file " << config_file_path_;
        successful_response = false;
      }
      if (!ObtainBootstrapInformation(config)) {
        LOG(kError) << "Failed to get endpoints for process_index "
                    << vault_identity_request.process_index();
        successful_response = false;
      } else {
        std::lock_guard<std::mutex> lock(config_file_mutex_);
        if (!WriteFile(config_file_path_, config.SerializeAsString())) {
          LOG(kError) << "Failed to write config file after obtaining bootstrap info.";
        } else {
          successful_response = true;
        }
      }
    } else {
      successful_response = true;
    }
  }
  if (successful_response) {
    itr = FindFromProcessIndex(vault_identity_request.process_index());
    vault_identity_response.set_pmid(serialised_pmid.string());
    vault_identity_response.set_chunkstore_path((*itr)->chunkstore_path);
    (*itr)->vault_port = static_cast<Port>(vault_identity_request.listening_port());
    (*itr)->vault_version = vault_identity_request.version();
    std::for_each(endpoints_.begin(), endpoints_.end(),
                  [&vault_identity_response](const EndPoint & element) {
      vault_identity_response.add_bootstrap_endpoint_ip(element.first);
      vault_identity_response.add_bootstrap_endpoint_port(element.second);
    });
  } else {
    vault_identity_response.clear_pmid();
    vault_identity_response.clear_chunkstore_path();
    // TODO(Team): further investigation on whether this return is suitable is required
    return;
  }
  response = detail::WrapMessage(MessageType::kVaultIdentityResponse,
                                 vault_identity_response.SerializeAsString());
}

void VaultManager::HandleVaultJoinedNetworkRequest(const std::string& request,
                                                       std::string& response) {
  protobuf::VaultJoinedNetwork vault_joined_network;
  if (!vault_joined_network.ParseFromString(request)) {
    // Silently drop
    LOG(kError) << "Failed to parse VaultJoinedNetwork.";
    return;
  }

  protobuf::VaultJoinedNetworkAck vault_joined_network_ack;
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(FindFromProcessIndex(vault_joined_network.process_index()));
  bool join_result(false);
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with process_index " << vault_joined_network.process_index()
                << " hasn't been added.";
    join_result = false;
  } else {
    join_result = true;
    (*itr)->joined_network = vault_joined_network.joined();
  }
  vault_joined_network_ack.set_ack(join_result);
  if ((*itr)->client_port != 0)
    SendVaultJoinConfirmation((*itr)->pmid->name(), join_result);
  response = detail::WrapMessage(MessageType::kVaultIdentityResponse,
                                 vault_joined_network_ack.SerializeAsString());
}

void VaultManager::HandleStopVaultRequest(const std::string& request, std::string& response) {
  protobuf::StopVaultRequest stop_vault_request;
  if (!stop_vault_request.ParseFromString(request)) {
    // Silently drop
    LOG(kError) << "Failed to parse StopVaultRequest.";
    return;
  }

  protobuf::StopVaultResponse stop_vault_response;
  passport::Pmid::Name pmid_name(Identity(stop_vault_request.identity()));
  asymm::PlainText data(stop_vault_request.data());
  asymm::Signature signature(stop_vault_request.signature());
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(FindFromPmidName(pmid_name));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << Base64Substr(pmid_name.value) << " hasn't been added.";
    stop_vault_response.set_result(false);
  } else if (!asymm::CheckSignature(data, signature, (*itr)->pmid->public_key())) {
    LOG(kError) << "Failure to validate request to stop vault ID " << Base64Substr(pmid_name.value);
    stop_vault_response.set_result(false);
  } else {
    LOG(kInfo) << "Shutting down vault with identity " << Base64Substr(pmid_name.value);
    stop_vault_response.set_result(StopVault(pmid_name, data, signature, true));
    if (!AmendVaultDetailsInConfigFile(*itr, true)) {
      LOG(kError) << "Failed to amend details in config file for vault ID: "
                  << Base64Substr((*itr)->pmid->name().value);
      stop_vault_response.set_result(false);
    }
  }
  response =
      detail::WrapMessage(MessageType::kStopVaultResponse, stop_vault_response.SerializeAsString());
}

void VaultManager::HandleUpdateIntervalRequest(const std::string& request,
                                                   std::string& response) {
  protobuf::UpdateIntervalRequest update_interval_request;
  if (!update_interval_request.ParseFromString(request)) {  // Silently drop
    LOG(kError) << "Failed to parse UpdateIntervalRequest.";
    return;
  }

  protobuf::UpdateIntervalResponse update_interval_response;
  if (update_interval_request.has_new_update_interval()) {
    if (SetUpdateInterval(bptime::seconds(update_interval_request.new_update_interval())))
      update_interval_response.set_update_interval(GetUpdateInterval().total_seconds());
    else
      update_interval_response.set_update_interval(0);
  } else {
    update_interval_response.set_update_interval(GetUpdateInterval().total_seconds());
  }

  response = detail::WrapMessage(MessageType::kUpdateIntervalResponse,
                                 update_interval_response.SerializeAsString());
}

void VaultManager::HandleSendEndpointToVaultManagerRequest(const std::string& request,
                                                                   std::string& response) {
  protobuf::SendEndpointToVaultManagerRequest send_endpoint_request;
  protobuf::SendEndpointToVaultManagerResponse send_endpoint_response;
  if (!send_endpoint_request.ParseFromString(request)) {
    LOG(kError) << "Failed to parse SendEndpointToVaultManager.";
    return;
  }
  if (AddBootstrapEndPoint(
          send_endpoint_request.bootstrap_endpoint_ip(),
          static_cast<Port>(send_endpoint_request.bootstrap_endpoint_port()))) {
    send_endpoint_response.set_result(true);
  } else {
    send_endpoint_response.set_result(false);
  }
  response = detail::WrapMessage(MessageType::kSendEndpointToVaultManagerResponse,
                                 send_endpoint_response.SerializeAsString());
}

void VaultManager::HandleBootstrapRequest(const std::string& request, std::string& response) {
  protobuf::BootstrapRequest bootstrap_request;
  protobuf::BootstrapResponse bootstrap_response;
  if (!bootstrap_request.ParseFromString(request)) {
    LOG(kError) << "Failed to parse BootstrapRequest.";
    return;
  }
  if (endpoints_.empty()) {
    protobuf::VaultManagerConfig config;
    if (!ReadFileToVaultManagerConfig(config_file_path_, config)) {
      // TODO(Team): Should have counter for failures to trigger recreation?
      LOG(kError) << "Failed to read & parse config file " << config_file_path_;
    }
    if (!ObtainBootstrapInformation(config)) {
      LOG(kError) << "Failed to get endpoints from bootstrap server";
    } else {
      if (!WriteFile(config_file_path_, config.SerializeAsString())) {
        LOG(kError) << "Failed to write config file after obtaining bootstrap info.";
      }
    }
  } else {
    std::for_each(endpoints_.begin(), endpoints_.end(),
                  [&bootstrap_response](const EndPoint & element) {
      bootstrap_response.add_bootstrap_endpoint_ip(element.first);
      bootstrap_response.add_bootstrap_endpoint_port(element.second);
    });
  }
  response =
      detail::WrapMessage(MessageType::kBootstrapResponse, bootstrap_response.SerializeAsString());
}

// NOTE: vault_info_mutex_ must be locked when calling this function.
void VaultManager::SendVaultJoinConfirmation(const passport::Pmid::Name& pmid_name,
                                                 bool join_result) {
  protobuf::VaultJoinConfirmation vault_join_confirmation;
  auto itr(FindFromPmidName(pmid_name));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << Base64Substr(pmid_name.value) << " hasn't been added.";
    return;
  }
  Port client_port((*itr)->client_port);
  std::mutex local_mutex;
  std::condition_variable local_cond_var;
  bool done(false), local_result(false);
  std::function<void(bool)> callback = [&](bool result) {
      {
        std::lock_guard<std::mutex> lock(local_mutex);
        local_result = result;
        done = true;
      }
      local_cond_var.notify_one();
  };
  TransportPtr request_transport(new LocalTcpTransport(asio_service_.service()));
  int result(0);
  request_transport->Connect((*itr)->client_port, result);
  if (result != kSuccess) {
    LOG(kError) << "Failed to connect request transport to client.";
    callback(false);
  }

  request_transport->on_message_received().connect([this, callback](
      const std::string & message,
      Port /*vault_manager_port*//*) { HandleVaultJoinConfirmationAck(message, callback); });
  request_transport->on_error().connect([this, callback](const int & error) {
    LOG(kError) << "Transport reported error code " << error;
    callback(false);
  });
  vault_join_confirmation.set_identity(pmid_name->string());
  vault_join_confirmation.set_joined(join_result);
  LOG(kVerbose) << "Sending vault join confirmation to client on port " << (*itr)->client_port;
  request_transport->Send(detail::WrapMessage(MessageType::kVaultJoinConfirmation,
                                              vault_join_confirmation.SerializeAsString()),
                          client_port);

  std::unique_lock<std::mutex> lock(local_mutex);
  if (!local_cond_var.wait_for(lock, std::chrono::seconds(10), [&] { return done; }))
    LOG(kError) << "Timed out waiting for reply.";
  if (!local_result)
    LOG(kError) << "Failed to confirm joining of vault to client.";
}

void VaultManager::HandleVaultJoinConfirmationAck(
    const std::string& message, std::function<void(bool)> callback) {
  MessageType type;
  std::string payload;
  if (!detail::UnwrapMessage(message, type, payload)) {
    LOG(kError) << "Failed to handle incoming message.";
    return;
  }
  if (type != MessageType::kVaultJoinConfirmationAck) {
    LOG(kError) << "Incoming message is of incorrect type.";
    return;
  }
  protobuf::VaultJoinConfirmationAck ack;
  ack.ParseFromString(payload);
  callback(ack.ack());
}

bool VaultManager::InTestMode() const {
  return config_file_path_ == fs::path(".") / detail::kGlobalConfigFilename;
}

std::vector<VaultManager::VaultInfoPtr>::iterator VaultManager::FindFromPmidName(
    const passport::Pmid::Name& pmid_name) {
  return std::find_if(vault_infos_.begin(), vault_infos_.end(),
                                                [pmid_name](const VaultInfoPtr & vault_info)->bool {
    return vault_info->pmid->name() == pmid_name;
  });
}

std::vector<VaultManager::VaultInfoPtr>::iterator VaultManager::FindFromProcessIndex(
    ProcessIndex process_index) {
  return std::find_if(vault_infos_.begin(),
                      vault_infos_.end(), [process_index](const VaultInfoPtr & vault_info)->bool {
    return vault_info->process_index == process_index;
  });
}

void VaultManager::RestartVault(const passport::Pmid::Name& pmid_name) {
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(FindFromPmidName(pmid_name));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << Base64Substr(pmid_name.value) << " hasn't been added.";
    return;
  }
  process_manager_.StartProcess((*itr)->process_index);
}

// NOTE: vault_infos_mutex_ must be locked before calling this function.
// TODO(Fraser#5#): 2012-08-17 - This is pretty heavy-handed - locking for duration of function.
//                               Try to reduce lock scope eventually.
bool VaultManager::StopVault(const passport::Pmid::Name& pmid_name,
                                 const asymm::PlainText& data, const asymm::Signature& signature,
                                 bool permanent) {
  auto itr(FindFromPmidName(pmid_name));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << Base64Substr(pmid_name.value) << " hasn't been added.";
    return false;
  }
  (*itr)->requested_to_run = !permanent;
  process_manager_.LetProcessDie((*itr)->process_index);
  protobuf::VaultShutdownRequest vault_shutdown_request;
  vault_shutdown_request.set_process_index((*itr)->process_index);
  vault_shutdown_request.set_data(data.string());
  vault_shutdown_request.set_signature(signature.string());
  std::shared_ptr<LocalTcpTransport> sending_transport(
      std::make_shared<LocalTcpTransport>(asio_service_.service()));
  int result(0);
  sending_transport->Connect((*itr)->vault_port, result);
  if (result != kSuccess) {
    LOG(kError) << "Failed to connect sending transport to vault.";
    return false;
  }

  sending_transport->Send(detail::WrapMessage(MessageType::kVaultShutdownRequest,
                                              vault_shutdown_request.SerializeAsString()),
                          (*itr)->vault_port);
  LOG(kInfo) << "Sent shutdown request to vault on port " << (*itr)->vault_port;
  return process_manager_.WaitForProcessToStop((*itr)->process_index);
}

void VaultManager::StopAllVaults() {
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  std::for_each(vault_infos_.begin(), vault_infos_.end(), [this](const VaultInfoPtr & info) {
    if (process_manager_.GetProcessStatus(info->process_index) != ProcessStatus::kRunning) {
      return;
    }
    asymm::PlainText random_data(RandomString(64));
    asymm::Signature signature(asymm::Sign(random_data, info->pmid->private_key()));
    if (!StopVault(info->pmid->name(), random_data, signature, false)) {
      LOG(kError) << "StopAllVaults: failed to stop - " << Base64Substr(info->pmid->name().value);
    }
  });
}

/*
//  void VaultManager::EraseVault(const std::string& account_name) {
//    if (index < static_cast<int32_t>(processes_.size())) {
//      auto itr(processes_.begin() + (index - 1));
//      process_manager_.KillProcess((*itr).second);
//      processes_.erase(itr);
//      LOG(kInfo) << "Erasing vault...";
//      if (WriteConfig()) {
//        LOG(kInfo) << "Done!";
//      }
//    } else {
//      LOG(kError) << "Invalid index of " << index << " for processes container with size "
//                  << processes_.size();
//    }
//  }

//  int32_t VaultManager::ListVaults(bool select) const {
//    fs::path path((GetSystemAppDir() / "config.txt"));
//
//    std::string content;
//    ReadFile(path, &content);
//
//    typedef boost::tokenizer<boost::char_separator<char>> vault_tokenizer;
//    boost::char_separator<char> delimiter("\n", "", boost::keep_empty_tokens);
//    vault_tokenizer tok(content, delimiter);
//
//    int32_t i = 1;
//    LOG(kInfo) << "************************************************************";
//    for (vault_tokenizer::iterator iterator = tok.begin(); iterator != tok.end(); ++iterator) {
//      LOG(kInfo) << i << ". " << *iterator;
//      i++;
//    }
//    LOG(kInfo) << "************************************************************";
//
//    if (select) {
//      int32_t option;
//      LOG(kInfo) << "Select an item: ";
//      std::cin >> option;
//      return option;
//    }
//
//    return 0;
//  }
*//*

bool VaultManager::ObtainBootstrapInformation(protobuf::VaultManagerConfig& config) {
  protobuf::Bootstrap* bootstrap_list(config.mutable_bootstrap_endpoints());

  protobuf::Bootstrap end_points;
#ifdef TESTING
  if (detail::UsingDefaultEnvironment()) {
#endif
    std::string serialised_endpoints(download_manager_.GetBootstrapInfo());
    if (serialised_endpoints.empty()) {
      LOG(kError) << "Retrieved endpoints are empty.";
    }
    if (!end_points.ParseFromString(serialised_endpoints)) {
      LOG(kError) << "Retrieved endpoints do not parse.";
      return false;
    }
#ifdef TESTING
  } else {
    if (end_points.bootstrap_contacts_size() == 0) {
      if (detail::GetBootstrapIps().empty()) {
        protobuf::Endpoint* local_endpoint(end_points.add_bootstrap_contacts());
        local_endpoint->set_ip(GetLocalIp().to_string());
        local_endpoint->set_port(kLivePort);
      } else {
        for (auto& ep : detail::GetBootstrapIps()) {
          protobuf::Endpoint* local_endpoint(end_points.add_bootstrap_contacts());
          local_endpoint->set_ip(ep.address().to_string());
          local_endpoint->set_port(ep.port());
        }
      }
    }
  }
#endif
  LoadBootstrapEndpoints(end_points);

  bootstrap_list->CopyFrom(end_points);
  return true;
}

void VaultManager::LoadBootstrapEndpoints(const protobuf::Bootstrap& end_points) {
  int max_index(end_points.bootstrap_contacts_size());
  std::lock_guard<std::mutex> lock(config_file_mutex_);
  endpoints_.clear();
  for (int n(0); n < max_index; ++n) {
    std::string ip(end_points.bootstrap_contacts(n).ip());
    Port port(static_cast<Port>(end_points.bootstrap_contacts(n).port()));
    endpoints_.push_back(std::make_pair(ip, port));
  }
}

bool VaultManager::StartVaultProcess(VaultInfoPtr& vault_info) {
  Process process;
#ifdef TESTING
  fs::path executable_path(detail::GetPathToVault());
  if (executable_path.empty())
    executable_path = fs::path(".");
#ifdef MAIDSAFE_WIN32
  TCHAR file_name[MAX_PATH];
  if (GetModuleFileName(NULL, file_name, MAX_PATH))
    executable_path = fs::path(file_name).parent_path();
#endif
#else
  fs::path executable_path(GetAppInstallDir());
#endif
  if (!process.SetExecutablePath(executable_path / detail::kVaultName)) {
    LOG(kError) << "Failed to set executable path for: "
                << Base64Substr(vault_info->pmid->name().value);
    return false;
  }
  // --vmid argument is added automatically by process_manager_.AddProcess(...)

  process.AddArgument("--start");
  process.AddArgument("--chunk_path " + vault_info->chunkstore_path);
#if defined TESTING
  process.AddArgument("--identity_index " + std::to_string(vault_info->identity_index));
// process.AddArgument("--log_folder ./dummy_vault_logfiles");
//   process.AddArgument("--log_routing I");
#endif

  LOG(kInfo) << "Process Name: " << process.name();
  vault_info->process_index = process_manager_.AddProcess(process, listener_.ListeningPort());
  if (vault_info->process_index == ProcessManager::kInvalidIndex()) {
    LOG(kError) << "Error starting vault with ID: " << Base64Substr(vault_info->pmid->name().value);
    return false;
  }

  vault_infos_.push_back(vault_info);
  process_manager_.StartProcess(vault_info->process_index);
  return true;
}

bool VaultManager::ReadFileToVaultManagerConfig(const fs::path& file_path,
                                                        protobuf::VaultManagerConfig& config) {
  std::string config_content;
  {
    std::lock_guard<std::mutex> lock(config_file_mutex_);
    if (!ReadFile(file_path, &config_content) || config_content.empty()) {
      // TODO(Team): Should have counter for failures to trigger recreation?
      LOG(kError) << "Failed to read config file " << file_path;
      return false;
    }
  }

  if (!config.ParseFromString(config_content)) {
    // TODO(Team): Should have counter for failures to trigger recreation?
    LOG(kError) << "Failed to read config file " << file_path;
    return false;
  }

  return true;
}

bool VaultManager::AddBootstrapEndPoint(const std::string& ip, Port port) {
  std::unique_lock<std::mutex> lock(config_file_mutex_);
  auto it(std::find_if(endpoints_.begin(), endpoints_.end(),
                                               [&ip, &port](const EndPoint & element)->bool {
    return element.first == ip && element.second == port;
  }));

  if (it == endpoints_.end()) {
    endpoints_.push_back(std::make_pair(ip, port));
    while (endpoints_.size() > 1000U) {  // TODO(Philip) add constant for max bootstrap file size
      auto itr(endpoints_.begin());
      endpoints_.erase(itr);
    }
    lock.unlock();
    protobuf::VaultManagerConfig config;
    if (!ReadFileToVaultManagerConfig(config_file_path_, config)) {
      // TODO(Team): Should have counter for failures to trigger recreation?
      LOG(kError) << "Failed to read & parse config file " << config_file_path_;
      return false;
    }
    protobuf::Bootstrap* eps = config.mutable_bootstrap_endpoints();
    eps->Clear();
    protobuf::Endpoint* node;
    lock.lock();
    for (const auto& elem : endpoints_) {
      node = eps->add_bootstrap_contacts();
      node->set_ip((elem).first);
      node->set_port((elem).second);
    }
    if (!WriteFile(config_file_path_, config.SerializeAsString())) {
      LOG(kError) << "Failed to write config file after adding endpoint.";
      return false;
    }
    lock.unlock();
  } else {
    LOG(kInfo) << "Endpoint " << ip << ":" << port << " already in config file.";
  }

  return true;
}

bool VaultManager::AmendVaultDetailsInConfigFile(const VaultInfoPtr& vault_info,
                                                     bool existing_vault) {
  protobuf::VaultManagerConfig config;
  if (!ReadFileToVaultManagerConfig(config_file_path_, config)) {
    LOG(kError) << "Failed to read config file to amend details of vault ID "
                << Base64Substr(vault_info->pmid->name().value);
    return false;
  }

  if (existing_vault) {
    for (int n(0); n < config.vault_info_size(); ++n) {
      passport::Pmid pmid(passport::ParsePmid(NonEmptyString(config.vault_info(n).pmid())));
      if (vault_info->pmid->name() == pmid.name()) {
        protobuf::VaultInfo* p_info = config.mutable_vault_info(n);
        p_info->set_pmid(passport::SerialisePmid(*vault_info->pmid).string());
        p_info->set_chunkstore_path(vault_info->chunkstore_path);
        p_info->set_requested_to_run(vault_info->requested_to_run);
        p_info->set_version(vault_info->vault_version);
        n = config.vault_info_size();
      }
    }
  } else {
    protobuf::VaultInfo* p_info = config.add_vault_info();
    p_info->set_pmid(passport::SerialisePmid(*vault_info->pmid).string());
    p_info->set_chunkstore_path(vault_info->chunkstore_path);
    p_info->set_requested_to_run(true);
    p_info->set_version(kInvalidVersion);
    {
      std::lock_guard<std::mutex> lock(config_file_mutex_);
      if (!WriteFile(config_file_path_, config.SerializeAsString())) {
        LOG(kError) << "Failed to write config file to amend details of vault ID "
                    << Base64Substr(vault_info->pmid->name().value);
        return false;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(config_file_mutex_);
    if (!WriteFile(config_file_path_, config.SerializeAsString())) {
      LOG(kError) << "Failed to write config file after adding endpoint.";
      return false;
    }
  }

  return true;
}*/

}  // namespace vault_manager

}  // namespace maidsafe