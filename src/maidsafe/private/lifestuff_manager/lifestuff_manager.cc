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

#include "maidsafe/private/lifestuff_manager/lifestuff_manager.h"

#include <chrono>
#include <iostream>

#include "boost/filesystem/path.hpp"
#include "boost/lexical_cast.hpp"

#include "maidsafe/common/config.h"
#include "maidsafe/common/log.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/private/lifestuff_manager/controller_messages_pb.h"
#include "maidsafe/private/lifestuff_manager/local_tcp_transport.h"
#include "maidsafe/private/lifestuff_manager/return_codes.h"
#include "maidsafe/private/lifestuff_manager/utils.h"
#include "maidsafe/private/lifestuff_manager/vault_info_pb.h"


namespace bptime = boost::posix_time;
namespace fs = boost::filesystem;

namespace maidsafe {

namespace priv {

namespace lifestuff_manager {

#ifndef MAIDSAFE_WIN32
namespace {

std::string GetUserId() {
#ifdef USE_DUMMY
  return "maidsafe";
#else
  char user_name[64] = {0};
  int result(getlogin_r(user_name, sizeof(user_name) - 1));
  if (0 != result)
    return "";
  return std::string(user_name);
#endif
}

}  // unnamed namespace
#endif

LifeStuffManager::VaultInfo::VaultInfo()
    : process_index(),
      account_name(),
      fob(),
      chunkstore_path(),
      vault_port(0),
      client_port(0),
      requested_to_run(false),
      joined_network(false),
      vault_version(kInvalidVersion) {}

void LifeStuffManager::VaultInfo::ToProtobuf(protobuf::VaultInfo* pb_vault_info) const {
  pb_vault_info->set_account_name(account_name);
  pb_vault_info->set_fob(utils::SerialiseFob(fob).string());
  pb_vault_info->set_chunkstore_path(chunkstore_path);
  pb_vault_info->set_requested_to_run(requested_to_run);
  pb_vault_info->set_version(vault_version);
}

void LifeStuffManager::VaultInfo::FromProtobuf(const protobuf::VaultInfo& pb_vault_info) {
  account_name = pb_vault_info.account_name();
  fob = utils::ParseFob(NonEmptyString(pb_vault_info.fob()));
  chunkstore_path = pb_vault_info.chunkstore_path();
  requested_to_run = pb_vault_info.requested_to_run();
  vault_version = pb_vault_info.version();
}


LifeStuffManager::LifeStuffManager()
    : process_manager_(),
      // TODO(Fraser#5#): 2012-08-12 - Provide proper path to server as constants
      download_manager_("http", "dash.maidsafe.net", "~phil"),
      asio_service_(3),
      update_interval_(/*bptime::hours(24)*/ kMinUpdateInterval()),
//      update_timer_(asio_service_.service()),
      update_mutex_(),
      transport_(std::make_shared<LocalTcpTransport>(asio_service_.service())),
      local_port_(kMinPort()),
      vault_infos_(),
      vault_infos_mutex_(),
      client_ports_and_versions_(),
      client_ports_mutex_(),
#ifdef USE_TEST_KEYS
      config_file_path_(GetUserAppDir() / detail::kGlobalConfigFilename),
#else
      config_file_path_(GetSystemAppSupportDir() / detail::kGlobalConfigFilename),
#endif
      latest_local_installer_path_(),
      endpoints_(),
      config_file_mutex_(),
      need_to_stop_(false) {
#ifdef USE_TEST_KEYS
  WriteFile(GetUserAppDir() / "ServiceVersion.txt", kApplicationVersion);
#else
  WriteFile(GetSystemAppSupportDir() / "ServiceVersion.txt", kApplicationVersion);
#endif
  asio_service_.Start();
#ifdef USE_DUMMY
  Initialise();
#else
  asio_service_.service().post([&] () { Initialise(); });  // NOLINT (Dan)
#endif
}

void LifeStuffManager::Initialise() {
  transport_->on_message_received().connect(
      [this] (const std::string& message, Port peer_port) {
        HandleReceivedMessage(message, peer_port);
      });
  transport_->on_error().connect([] (const int& error) {
                                   LOG(kError) << "Transport reported error code: " << error;
                                 });

  boost::system::error_code error_code;
  if (!fs::exists(config_file_path_, error_code) ||
      error_code.value() == boost::system::errc::no_such_file_or_directory) {
    LOG(kInfo) << "LifeStuffManager failed to find existing config file in " << config_file_path_;
    while (!CreateConfigFile()) {
      if (need_to_stop_)
        return;
      LOG(kError) << "Will retry to to create new config file at " << config_file_path_;
      Sleep(boost::posix_time::seconds(1));
    }
  }

  while (!ListenForMessages()) {
    if (need_to_stop_)
      return;
    LOG(kError) << "LifeStuffManager failed to create a listening port. Shutting down.";
    Sleep(boost::posix_time::seconds(1));
  }

  download_manager_.SetLatestLocalVersion(kApplicationVersion);
  UpdateExecutor();


  ReadConfigFileAndStartVaults();
  error_code.clear();
  CheckForUpdates(error_code);
  LOG(kInfo) << "LifeStuffManager started";
}

LifeStuffManager::~LifeStuffManager() {
  need_to_stop_ = true;
  process_manager_.LetAllProcessesDie();
  StopAllVaults();
//  {
//    std::lock_guard<std::mutex> lock(update_mutex_);
//    update_timer_.cancel();
//  }
  transport_->StopListeningAndCloseConnections();
  asio_service_.Stop();
}

boost::posix_time::time_duration LifeStuffManager::kMinUpdateInterval() {
  return bptime::minutes(5);
}

boost::posix_time::time_duration LifeStuffManager::kMaxUpdateInterval() {
  return bptime::hours(24 * 7);
}

void LifeStuffManager::RestartLifeStuffManager(const std::string& /*latest_file*/,
                                               const std::string& /*executable_name*/) const {
  // system("/etc/init.d/mvm restart");
//  int result(system(command.c_str()));
//  if (result != 0)
  LOG(kWarning) << "Not implemented";
}

bool LifeStuffManager::CreateConfigFile() {
  protobuf::LifeStuffManagerConfig config;
  config.set_update_interval(update_interval_.total_seconds());

  int count(0);
  while (!ObtainBootstrapInformation(config) && count++ < 10) {
    LOG(kError) << "Failed to obtain bootstrap information from server.";
//    return false;
  }
  std::lock_guard<std::mutex> lock(config_file_mutex_);
  if (!WriteFile(config_file_path_, config.SerializeAsString())) {
    LOG(kError) << "Failed to create config file " << config_file_path_;
    return false;
  }
  LOG(kInfo) << "Created config file " << config_file_path_;

  return true;
}

bool LifeStuffManager::ReadConfigFileAndStartVaults() {
  std::string content;
  {
    std::lock_guard<std::mutex> lock(config_file_mutex_);
    if (!ReadFile(config_file_path_, &content)) {
      LOG(kError) << "Failed to read config file " << config_file_path_;
      return false;
    }
  }
  protobuf::LifeStuffManagerConfig config;
  if (!config.ParseFromString(content)) {
    LOG(kError) << "Failed to parse config file " << config_file_path_;
    return false;
  }

  update_interval_ = bptime::seconds(config.update_interval());

  protobuf::Bootstrap end_points(config.bootstrap_endpoints());

  LoadBootstrapEndpoints(end_points);

  for (int i(0); i != config.vault_info_size(); ++i) {
    VaultInfoPtr vault_info(new VaultInfo);
    vault_info->FromProtobuf(config.vault_info(i));
    if (vault_info->requested_to_run) {
      if (!StartVaultProcess(vault_info))
        LOG(kError) << "Failed to start vault ID" << Base64Substr(vault_info->fob.identity);
    }
  }

  return true;
}

bool LifeStuffManager::WriteConfigFile() {
  protobuf::LifeStuffManagerConfig config;
  {
    std::lock_guard<std::mutex> lock(update_mutex_);
    config.set_update_interval(update_interval_.total_seconds());
  }
  {
    std::lock_guard<std::mutex> lock(vault_infos_mutex_);
    for (auto& vault_info : vault_infos_) {
      protobuf::VaultInfo* pb_vault_info = config.add_vault_info();
      vault_info->ToProtobuf(pb_vault_info);
    }
  }
  {
    std::lock_guard<std::mutex> lock(config_file_mutex_);
    if (!WriteFile(config_file_path_, config.SerializeAsString())) {
      LOG(kError) << "Failed to write config file " << config_file_path_;
      return false;
    }
  }
  return true;
}

bool LifeStuffManager::ListenForMessages() {
  int result(0);
  transport_->StartListening(local_port_, result);
  while (result != kSuccess) {
    ++local_port_;
    if (local_port_ > kMaxPort()) {
      LOG(kError) << "Listening failed on all ports in range " << kMinPort() << " - " << kMaxPort();
      return false;
    }
    transport_->StartListening(local_port_, result);
  }
  LOG(kError) << "Listening on " << local_port_;
  return true;
}

void LifeStuffManager::HandleReceivedMessage(const std::string& message, Port peer_port) {
  MessageType type;
  std::string payload;
  if (!detail::UnwrapMessage(message, type, payload)) {
    LOG(kError) << "Failed to handle incoming message.";
    return;
  }

  LOG(kVerbose) << "HandleReceivedMessage: message type " << static_cast<int>(type) << " received.";
  std::string response;
  switch (type) {
    case MessageType::kClientRegistrationRequest:
      HandleClientRegistrationRequest(payload, response);
      break;
    case MessageType::kStartVaultRequest:
      HandleStartVaultRequest(payload, response);
      break;
    case MessageType::kVaultIdentityRequest:
      HandleVaultIdentityRequest(payload, response);
      break;
    case MessageType::kVaultJoinedNetwork:
      HandleVaultJoinedNetworkRequest(payload, response);
      break;
    case MessageType::kStopVaultRequest:
      HandleStopVaultRequest(payload, response);
      break;
    case MessageType::kUpdateIntervalRequest:
      HandleUpdateIntervalRequest(payload, response);
      break;
    case MessageType::kSendEndpointToLifeStuffManagerRequest:
      HandleSendEndpointToLifeStuffManagerRequest(payload, response);
      break;
    case MessageType::kBootstrapRequest:
      HandleBootstrapRequest(payload, response);
      break;
    default:
      return;
  }
  transport_->Send(response, peer_port);
}

void LifeStuffManager::HandleClientRegistrationRequest(const std::string& request,
                                                  std::string& response) {
  protobuf::ClientRegistrationRequest client_request;
  if (!client_request.ParseFromString(request)) {  // Silently drop
    LOG(kError) << "Failed to parse client registration request.";
    return;
  }

  uint16_t request_port(static_cast<uint16_t>(client_request.listening_port()));
  {
    std::lock_guard<std::mutex> lock(client_ports_mutex_);
    client_ports_and_versions_[request_port] = client_request.version();
  }

  protobuf::ClientRegistrationResponse client_response;
  if (endpoints_.empty()) {
    protobuf::LifeStuffManagerConfig config;
    if (!ReadFileToLifeStuffManagerConfig(config_file_path_, config)) {
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
    std::for_each(endpoints_.begin(),
                  endpoints_.end(),
                  [&client_response] (const EndPoint& element) {
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

void LifeStuffManager::HandleStartVaultRequest(const std::string& request, std::string& response) {
  protobuf::StartVaultRequest start_vault_request;
  if (!start_vault_request.ParseFromString(request)) {
    // Silently drop
    LOG(kError) << "Failed to parse StartVaultRequest.";
    return;
  }

  auto set_response([&response] (bool result) {
    protobuf::StartVaultResponse start_vault_response;
    start_vault_response.set_result(result);
    response = detail::WrapMessage(MessageType::kStartVaultResponse,
                                   start_vault_response.SerializeAsString());
  });

  uint16_t client_port(static_cast<uint16_t>(start_vault_request.client_port()));
  {
    std::lock_guard<std::mutex> lock(client_ports_mutex_);
    auto client_itr(client_ports_and_versions_.find(client_port));
    if (client_itr == client_ports_and_versions_.end()) {
      LOG(kError) << "Client is not registered with LifeStuffManager.";
      return set_response(false);
    }
  }

  VaultInfoPtr vault_info(new VaultInfo);
  {
    std::lock_guard<std::mutex> lock(vault_infos_mutex_);
    auto itr(FindFromIdentity(vault_info->fob.identity));
    bool existing_vault(false);
    if (itr != vault_infos_.end()) {
      existing_vault = true;
      if (!asymm::CheckSignature(asymm::PlainText(start_vault_request.token()),
                                 asymm::Signature(start_vault_request.token_signature()),
                                 vault_info->fob.keys.public_key)) {
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
        Fob temp_keys(utils::ParseFob(NonEmptyString(start_vault_request.fob())));
        if ((*itr)->joined_network) {
          // TODO(Team): Stop and restart with new credentials
        } else {
          // TODO(Team): Start with new credentials
          (*itr)->account_name = start_vault_request.account_name();
          (*itr)->fob = temp_keys;
          (*itr)->client_port = client_port;
          (*itr)->requested_to_run = true;
        }
      }
    } else {
      // The vault is not already registered.
      vault_info->fob = utils::ParseFob(NonEmptyString(start_vault_request.fob()));
      vault_info->account_name = start_vault_request.account_name();
      if (start_vault_request.has_chunkstore_path()) {
        vault_info->chunkstore_path = start_vault_request.chunkstore_path();
      } else {
        std::string short_vault_id(EncodeToBase64(crypto::Hash<crypto::SHA1>(
                                                      vault_info->fob.identity)));
        vault_info->chunkstore_path = (config_file_path_.parent_path() / short_vault_id).string();
      }
      vault_info->client_port = client_port;
      if (!StartVaultProcess(vault_info)) {
        LOG(kError) << "Failed to start a process for vault ID: "
                    << Base64Substr(vault_info->fob.identity);
        return set_response(false);
      }
    }
    if (!AmendVaultDetailsInConfigFile(vault_info, existing_vault)) {
      LOG(kError) << "Failed to amend details in config file for vault ID: "
                  << Base64Substr(vault_info->fob.identity);
      return set_response(false);
    }
  }

  set_response(true);
}

void LifeStuffManager::HandleVaultIdentityRequest(const std::string& request,
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
  NonEmptyString serialised_fob;
  auto itr(FindFromProcessIndex(vault_identity_request.process_index()));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with process_index " << vault_identity_request.process_index()
                << " hasn't been added.";
    successful_response = false;
    // TODO(Team): Should this be dropped silently?
  } else {
    serialised_fob = utils::SerialiseFob((*itr)->fob);
    if (endpoints_.empty()) {
      protobuf::LifeStuffManagerConfig config;
      if (!ReadFileToLifeStuffManagerConfig(config_file_path_, config)) {
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
    vault_identity_response.set_account_name((*itr)->account_name);
    vault_identity_response.set_fob(serialised_fob.string());
    vault_identity_response.set_chunkstore_path((*itr)->chunkstore_path);
    (*itr)->vault_port = static_cast<uint16_t>(vault_identity_request.listening_port());
    (*itr)->vault_version = vault_identity_request.version();
    std::for_each(endpoints_.begin(),
                  endpoints_.end(),
                  [&vault_identity_response] (const EndPoint& element) {
                    vault_identity_response.add_bootstrap_endpoint_ip(element.first);
                    vault_identity_response.add_bootstrap_endpoint_port(element.second);
                  });
  } else {
    vault_identity_response.clear_account_name();
    vault_identity_response.clear_fob();
    vault_identity_response.clear_chunkstore_path();
    // TODO(Team): further investigation on whether this return is suitable is required
    return;
  }
  response = detail::WrapMessage(MessageType::kVaultIdentityResponse,
                                 vault_identity_response.SerializeAsString());
}

void LifeStuffManager::HandleVaultJoinedNetworkRequest(const std::string& request,
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
  SendVaultJoinConfirmation((*itr)->fob.identity, join_result);
  response = detail::WrapMessage(MessageType::kVaultIdentityResponse,
                                 vault_joined_network_ack.SerializeAsString());
}

void LifeStuffManager::HandleStopVaultRequest(const std::string& request, std::string& response) {
  protobuf::StopVaultRequest stop_vault_request;
  if (!stop_vault_request.ParseFromString(request)) {
    // Silently drop
    LOG(kError) << "Failed to parse StopVaultRequest.";
    return;
  }

  protobuf::StopVaultResponse stop_vault_response;
  Identity identity(stop_vault_request.identity());
  asymm::PlainText data(stop_vault_request.data());
  asymm::Signature signature(stop_vault_request.signature());
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(FindFromIdentity(identity));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << Base64Substr(identity) << " hasn't been added.";
    stop_vault_response.set_result(false);
  } else if (!asymm::CheckSignature(data, signature, (*itr)->fob.keys.public_key)) {
    LOG(kError) << "Failure to validate request to stop vault ID " << Base64Substr(identity);
    stop_vault_response.set_result(false);
  } else {
    LOG(kInfo) << "Shutting down vault with identity " << Base64Substr(identity);
    stop_vault_response.set_result(StopVault(identity, data, signature, true));
    if (!AmendVaultDetailsInConfigFile(*itr, true)) {
      LOG(kError) << "Failed to amend details in config file for vault ID: "
                  << Base64Substr((*itr)->fob.identity);
      stop_vault_response.set_result(false);
    }
  }
  response = detail::WrapMessage(MessageType::kStopVaultResponse,
                                 stop_vault_response.SerializeAsString());
}

void LifeStuffManager::HandleUpdateIntervalRequest(const std::string& request,
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

void LifeStuffManager::HandleSendEndpointToLifeStuffManagerRequest(const std::string& request,
                                                                   std::string& response) {
  protobuf::SendEndpointToLifeStuffManagerRequest send_endpoint_request;
  protobuf::SendEndpointToLifeStuffManagerResponse send_endpoint_response;
  if (!send_endpoint_request.ParseFromString(request)) {
    LOG(kError) << "Failed to parse SendEndpointToLifeStuffManager.";
    return;
  }
  if (AddBootstrapEndPoint(
          send_endpoint_request.bootstrap_endpoint_ip(),
          static_cast<uint16_t>(send_endpoint_request.bootstrap_endpoint_port()))) {
    send_endpoint_response.set_result(true);
  } else {
    send_endpoint_response.set_result(false);
  }
  response = detail::WrapMessage(MessageType::kSendEndpointToLifeStuffManagerResponse,
                                 send_endpoint_response.SerializeAsString());
}

void LifeStuffManager::HandleBootstrapRequest(const std::string& request, std::string& response) {
  protobuf::BootstrapRequest bootstrap_request;
  protobuf::BootstrapResponse bootstrap_response;
  if (!bootstrap_request.ParseFromString(request)) {
    LOG(kError) << "Failed to parse BootstrapRequest.";
    return;
  }
  if (endpoints_.empty()) {
    protobuf::LifeStuffManagerConfig config;
    if (!ReadFileToLifeStuffManagerConfig(config_file_path_, config)) {
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
    std::for_each(endpoints_.begin(),
                  endpoints_.end(),
                  [&bootstrap_response] (const EndPoint& element) {
                    bootstrap_response.add_bootstrap_endpoint_ip(element.first);
                    bootstrap_response.add_bootstrap_endpoint_port(element.second);
                  });
  }
  response = detail::WrapMessage(MessageType::kBootstrapResponse,
                                 bootstrap_response.SerializeAsString());
}

bool LifeStuffManager::SetUpdateInterval(const bptime::time_duration& update_interval) {
  if (update_interval < kMinUpdateInterval() || update_interval > kMaxUpdateInterval()) {
    LOG(kError) << "Invalid update interval of " << update_interval;
    return false;
  }
  std::lock_guard<std::mutex> lock(update_mutex_);
  update_interval_ = update_interval;
//  update_timer_.expires_from_now(update_interval_);
//  update_timer_.async_wait([this] (const boost::system::error_code& ec) { CheckForUpdates(ec); });  // NOLINT (Fraser)
  return true;
}

bptime::time_duration LifeStuffManager::GetUpdateInterval() const {
  std::lock_guard<std::mutex> lock(update_mutex_);
  return update_interval_;
}

void LifeStuffManager::CheckForUpdates(const boost::system::error_code& ec) {
  if (ec) {
    if (ec != boost::asio::error::operation_aborted) {
      LOG(kError) << ec.message();
      return;
    }
  }

  UpdateExecutor();

//  update_timer_.expires_from_now(update_interval_);
//  update_timer_.async_wait([this] (const boost::system::error_code& ec) { CheckForUpdates(ec); });  // NOLINT (Fraser)
}

// NOTE: vault_info_mutex_ must be locked when calling this function.
void LifeStuffManager::SendVaultJoinConfirmation(const Identity& identity, bool join_result) {
  protobuf::VaultJoinConfirmation vault_join_confirmation;
  auto itr(FindFromIdentity(identity));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << Base64Substr(identity)
                << " hasn't been added.";
    return;
  }
  uint16_t client_port((*itr)->client_port);
  std::mutex local_mutex;
  std::condition_variable local_cond_var;
  bool done(false), local_result(false);
  std::function<void(bool)> callback = [&] (bool result) {  // NOLINT (Dan)
                                         std::lock_guard<std::mutex> lock(local_mutex);
                                         local_result = result;
                                         done = true;
                                         local_cond_var.notify_one();
                                       };
  TransportPtr request_transport(new LocalTcpTransport(asio_service_.service()));
  int result(0);
  request_transport->Connect((*itr)->client_port, result);
  if (result != kSuccess) {
    LOG(kError) << "Failed to connect request transport to client.";
    callback(false);
  }

  request_transport->on_message_received().connect(
      [this, callback] (const std::string& message, Port /*lifestuff_manager_port*/) {
        HandleVaultJoinConfirmationAck(message, callback);
      });
  request_transport->on_error().connect([this, callback] (const int& error) {
                                          LOG(kError) << "Transport reported error code " << error;
                                          callback(false);
                                        });
  vault_join_confirmation.set_identity(identity.string());
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

void LifeStuffManager::HandleVaultJoinConfirmationAck(const std::string& message,
                                                      std::function<void(bool)> callback) {  // NOLINT (Philip)
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

void LifeStuffManager::SendNewVersionAvailable(uint16_t client_port) {
  protobuf::NewVersionAvailable new_version_available;
  std::mutex local_mutex;
  std::condition_variable local_cond_var;
  bool done(false), local_result(false);
  std::function<void(bool)> callback = [&] (bool result) {  // NOLINT (Dan)
                                         std::lock_guard<std::mutex> lock(local_mutex);
                                         local_result = result;
                                         done = true;
                                         local_cond_var.notify_one();
                                       };
  TransportPtr request_transport(std::make_shared<LocalTcpTransport>(asio_service_.service()));
  int result(0);
  request_transport->Connect(client_port, result);
  if (result != kSuccess) {
    LOG(kError) << "Failed to connect request transport to client.";
    callback(false);
  }
  request_transport->on_message_received().connect(
      [this, callback](const std::string& message, Port /*lifestuff_manager_port*/) {
        HandleNewVersionAvailableAck(message, callback);
      });
  request_transport->on_error().connect([this, callback] (const int& error) {
                                          LOG(kError) << "Transport reported error code " << error;
                                          callback(false);
                                        });
  new_version_available.set_new_version_filepath(latest_local_installer_path_.string());
  LOG(kVerbose) << "Sending new version available to client on port " << client_port;
  request_transport->Send(detail::WrapMessage(MessageType::kNewVersionAvailable,
                                              new_version_available.SerializeAsString()),
                          client_port);

  std::unique_lock<std::mutex> lock(local_mutex);
  if (!local_cond_var.wait_for(lock, std::chrono::seconds(10), [&] { return done; })) {
    LOG(kError) << "Timed out waiting for reply.";
    return;
  }
  if (!local_result) {
    LOG(kError) << "Failed to confirm joining of vault to client.";
    return;
  }

  {
    std::lock_guard<std::mutex> lock(client_ports_mutex_);
    auto client_itr(client_ports_and_versions_.find(client_port));
    if (client_itr == client_ports_and_versions_.end()) {
      LOG(kError) << "Client is not registered with LifeStuffManager.";
      return;
    }
    (*client_itr).second = VersionToInt(download_manager_.latest_local_version());
  }
}

void LifeStuffManager::HandleNewVersionAvailableAck(const std::string& message,
                                                    std::function<void(bool)> callback) {  // NOLINT (Philip)
  MessageType type;
  std::string payload;
  if (!detail::UnwrapMessage(message, type, payload)) {
    LOG(kError) << "Failed to handle incoming message.";
    return;
  }
  if (type != MessageType::kNewVersionAvailableAck) {
    LOG(kError) << "Incoming message is of incorrect type.";
    return;
  }
  protobuf::NewVersionAvailableAck ack;
  ack.ParseFromString(payload);
  callback(true);
}

#if defined MAIDSAFE_LINUX
bool LifeStuffManager::IsInstaller(const fs::path& path) {
  return path.extension() == ".deb" &&
         path.stem().string().length() > 8 &&
         path.stem().string().substr(0, 9) == "LifeStuff";
}
#else
bool LifeStuffManager::IsInstaller(const fs::path& path) {
  return path.extension() == ".exe" && path.stem().string().substr(0, 9) == "LifeStuff";
}
#endif

void LifeStuffManager::UpdateExecutor() {
  std::vector<fs::path> updated_files;
  if (download_manager_.Update(updated_files) != kSuccess) {
    LOG(kInfo) << "No update identified in the server.";
    return;  // failed or no updates.
  }

  auto it(std::find_if(updated_files.begin(),
                       updated_files.end(),
                       [&] (const fs::path& path)->bool { return IsInstaller(path); }));  // NOLINT
  if (it != updated_files.end()) {
    latest_local_installer_path_ = *it;
    LOG(kInfo) << "Found new installer at " << latest_local_installer_path_;
  } else {
    LOG(kInfo) << "No new installer";
  }

  it = (std::find_if(updated_files.begin(),
                     updated_files.end(),
                     [&] (const fs::path& path)->bool { return path.stem() == detail::kVaultName; }));  // NOLINT
  fs::path new_local_vault_path;
  if (it != updated_files.end()) {
    new_local_vault_path = *it;
    LOG(kInfo) << "Found new vault exe at " << new_local_vault_path;
  } else {
    LOG(kInfo) << "No new vault exe.";
  }

//    WriteConfigFile();
// #if defined MAIDSAFE_LINUX
//  std::string command("dpkg -i " + latest_local_installer_path_.string());
//  int result(system(command.c_str()));
//  if (result != 0)
//    LOG(kError) << "Update failed: failed to run installer.  Result: " << result;
// #elif defined MAIDSAFE_APPLE
//  // TODO(Phil#5#): 2012-09-04 - FIND INSTALLER IN UPDATED FILES
//  //  RUN INSTALLER SOMEHOW
// #endif

  // Notify out-of-date clients
  std::map<uint16_t, int> client_ports_and_versions_copy;
  {
    std::lock_guard<std::mutex> lock(client_ports_mutex_);
    client_ports_and_versions_copy = client_ports_and_versions_;
  }

  for (auto entry : client_ports_and_versions_copy) {
    if (entry.second < VersionToInt(download_manager_.latest_remote_version()))
      SendNewVersionAvailable(entry.first);
  }

  if (!new_local_vault_path.empty()) {
    StopAllVaults();
    boost::system::error_code error_code;
    fs::rename(new_local_vault_path,
               GetAppInstallDir() / (detail::kVaultName +
                                     detail::kThisPlatform().executable_extension()),
               error_code);
    if (error_code)
      LOG(kError) << "Failed to move new vault executable.";

    {
      std::lock_guard<std::mutex> lock(vault_infos_mutex_);
      vault_infos_.clear();
    }
    if (!ReadConfigFileAndStartVaults())
      LOG(kError) << "Failed to restart vaults.";
  }
}

bool LifeStuffManager::InTestMode() const {
  return config_file_path_ == fs::path(".") / detail::kGlobalConfigFilename;
}

std::vector<LifeStuffManager::VaultInfoPtr>::iterator LifeStuffManager::FindFromIdentity(
    const Identity& identity) {
  return std::find_if(vault_infos_.begin(),
                      vault_infos_.end(),
                      [identity] (const VaultInfoPtr& vault_info)->bool {
                        return vault_info->fob.identity == identity;
                      });
}

std::vector<LifeStuffManager::VaultInfoPtr>::iterator LifeStuffManager::FindFromProcessIndex(
    ProcessIndex process_index) {
  return std::find_if(vault_infos_.begin(),
                      vault_infos_.end(),
                      [process_index] (const VaultInfoPtr& vault_info)->bool {
                        return vault_info->process_index == process_index;
                      });
}

void LifeStuffManager::RestartVault(const Identity& identity) {
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  auto itr(FindFromIdentity(identity));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << Base64Substr(identity) << " hasn't been added.";
    return;
  }
  process_manager_.StartProcess((*itr)->process_index);
}

// NOTE: vault_infos_mutex_ must be locked before calling this function.
// TODO(Fraser#5#): 2012-08-17 - This is pretty heavy-handed - locking for duration of function.
//                               Try to reduce lock scope eventually.
bool LifeStuffManager::StopVault(const Identity& identity,
                                 const asymm::PlainText& data,
                                 const asymm::Signature& signature,
                                 bool permanent) {
  auto itr(FindFromIdentity(identity));
  if (itr == vault_infos_.end()) {
    LOG(kError) << "Vault with identity " << Base64Substr(identity) << " hasn't been added.";
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

void LifeStuffManager::StopAllVaults() {
  std::lock_guard<std::mutex> lock(vault_infos_mutex_);
  std::for_each(vault_infos_.begin(),
                vault_infos_.end(),
                [this] (const VaultInfoPtr& info) {
                  if (process_manager_.GetProcessStatus(info->process_index) !=
                      ProcessStatus::kRunning) {
                    return;
                  }
                  asymm::PlainText random_data(RandomString(64));
                  asymm::Signature signature(asymm::Sign(random_data, info->fob.keys.private_key));
                  if (!StopVault(info->fob.identity, random_data, signature, false)) {
                    LOG(kError) << "StopAllVaults: failed to stop - "
                                << Base64Substr(info->fob.identity);
                  }
                });
}

/*
//  void LifeStuffManager::EraseVault(const std::string& account_name) {
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

//  int32_t LifeStuffManager::ListVaults(bool select) const {
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
*/

bool LifeStuffManager::ObtainBootstrapInformation(protobuf::LifeStuffManagerConfig& config) {
  std::string serialised_endpoints(download_manager_.RetrieveBootstrapInfo());
  if (serialised_endpoints.empty()) {
    LOG(kError) << "Retrieved endpoints are empty.";
//    return false;
  }
  protobuf::Bootstrap end_points;
  if (!end_points.ParseFromString(serialised_endpoints)) {
    LOG(kError) << "Retrieved endpoints do not parse.";
    return false;
  }
  LoadBootstrapEndpoints(end_points);

  config.mutable_bootstrap_endpoints()->CopyFrom(end_points);
  return true;
}

void LifeStuffManager::LoadBootstrapEndpoints(protobuf::Bootstrap& end_points) {
  int max_index(end_points.bootstrap_contacts_size());
  std::lock_guard<std::mutex> lock(config_file_mutex_);
  endpoints_.clear();
  for (int n(0); n < max_index; ++n) {
    std::string ip(end_points.bootstrap_contacts(n).ip());
    uint16_t port(static_cast<uint16_t>(end_points.bootstrap_contacts(n).port()));
    endpoints_.push_back(std::make_pair(ip, port));
  }
}

bool LifeStuffManager::StartVaultProcess(VaultInfoPtr& vault_info) {
  Process process;
#ifdef USE_TEST_KEYS
#ifdef USE_DUMMY
  std::string process_name(detail::kDummyName);
#else
  std::string process_name(detail::kVaultName);
#endif
  fs::path executable_path(".");
#ifdef MAIDSAFE_WIN32
    TCHAR file_name[MAX_PATH];
    if (GetModuleFileName(NULL, file_name, MAX_PATH))
      executable_path = fs::path(file_name).parent_path();
#endif
#else
  std::string process_name(detail::kVaultName);
  fs::path executable_path(GetAppInstallDir());
#endif
  if (!process.SetExecutablePath(executable_path /
                                 (process_name + detail::kThisPlatform().executable_extension()))) {
    LOG(kError) << "Failed to set executable path for: " << Base64Substr(vault_info->fob.identity);
    return false;
  }
  // --vmid argument is added automatically by process_manager_.AddProcess(...)

  process.AddArgument("--log_config ./maidsafe_log.ini");
  process.AddArgument("--start");
  process.AddArgument("--chunk_path " + vault_info->chunkstore_path);
#ifdef USE_TEST_KEYS
#ifndef MAIDSAFE_WIN32
  std::string user_id(GetUserId());
  if (!user_id.empty())
    process.AddArgument("--usr_id " + user_id);
#endif
#endif

  LOG(kInfo) << "Process Name: " << process.name();
  vault_info->process_index = process_manager_.AddProcess(process, local_port_);
  if (vault_info->process_index == ProcessManager::kInvalidIndex()) {
    LOG(kError) << "Error starting vault with ID: " << Base64Substr(vault_info->fob.identity);
    return false;
  }

  vault_infos_.push_back(vault_info);
  process_manager_.StartProcess(vault_info->process_index);
  return true;
}

bool LifeStuffManager::ReadFileToLifeStuffManagerConfig(const fs::path& file_path,
                                                        protobuf::LifeStuffManagerConfig& config) {
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

bool LifeStuffManager::AddBootstrapEndPoint(const std::string& ip, const uint16_t& port) {
  std::unique_lock<std::mutex> lock(config_file_mutex_);
  auto it(std::find_if(endpoints_.begin(),
                       endpoints_.end(),
                       [&ip, &port] (const EndPoint& element)->bool {
                         return element.first == ip && element.second == port;
                       }));

  if (it == endpoints_.end()) {
    endpoints_.push_back(std::make_pair(ip, port));
    while (endpoints_.size() > 1000U) {  // TODO(Philip) add constant for max bootstrap file size
      auto itr(endpoints_.begin());
      endpoints_.erase(itr);
    }
    lock.unlock();
    protobuf::LifeStuffManagerConfig config;
    if (!ReadFileToLifeStuffManagerConfig(config_file_path_, config)) {
      // TODO(Team): Should have counter for failures to trigger recreation?
      LOG(kError) << "Failed to read & parse config file " << config_file_path_;
      return false;
    }
    protobuf::Bootstrap *eps = config.mutable_bootstrap_endpoints();
    eps->Clear();
    protobuf::Endpoint* node;
    lock.lock();
    for (auto itr(endpoints_.begin()); itr != endpoints_.end(); ++itr) {
      node = eps->add_bootstrap_contacts();
      node->set_ip((*itr).first);
      node->set_port((*itr).second);
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

bool LifeStuffManager::AmendVaultDetailsInConfigFile(const VaultInfoPtr& vault_info,
                                                     bool existing_vault) {
  protobuf::LifeStuffManagerConfig config;
  if (!ReadFileToLifeStuffManagerConfig(config_file_path_, config)) {
    LOG(kError) << "Failed to read config file to amend details of vault ID "
                << Base64Substr(vault_info->fob.identity);
    return false;
  }

  if (existing_vault) {
    for (int n(0); n < config.vault_info_size(); ++n) {
      Fob fob(utils::ParseFob(NonEmptyString(config.vault_info(n).fob())));
      if (vault_info->fob.identity == fob.identity) {
        protobuf::VaultInfo* p_info = config.mutable_vault_info(n);
        p_info->set_account_name(vault_info->account_name);
        p_info->set_fob(utils::SerialiseFob(vault_info->fob).string());
        p_info->set_chunkstore_path(vault_info->chunkstore_path);
        p_info->set_requested_to_run(vault_info->requested_to_run);
        p_info->set_version(vault_info->vault_version);
        n = config.vault_info_size();
      }
    }
  } else {
    protobuf::VaultInfo* p_info = config.add_vault_info();
    p_info->set_account_name(vault_info->account_name);
    p_info->set_fob(utils::SerialiseFob(vault_info->fob).string());
    p_info->set_chunkstore_path(vault_info->chunkstore_path);
    p_info->set_requested_to_run(true);
    p_info->set_version(kInvalidVersion);
    {
      std::lock_guard<std::mutex> lock(config_file_mutex_);
      if (!WriteFile(config_file_path_, config.SerializeAsString())) {
        LOG(kError) << "Failed to write config file to amend details of vault ID "
                    << Base64Substr(vault_info->fob.identity);
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
}

}  // namespace lifestuff_manager

}  // namespace priv

}  // namespace maidsafe