/* Copyright (c) 2010 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
* Neither the name of the maidsafe.net limited nor the names of its
contributors may be used to endorse or promote products derived from this
software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "maidsafe/private/message_handler.h"

#include <string>
#include "boost/lexical_cast.hpp"
#include "maidsafe/common/log.h"
#include "maidsafe/private/transport_pb.h"



namespace maidsafe {

namespace priv {

void MessageHandler::OnMessageReceived(const std::string &request,
                                       const Info /*&info*/,
                                       std::string */*response*/,
                                       Timeout */*timeout*/) {
  protobuf::WrapperMessage wrapper;
  if (wrapper.ParseFromString(request) && wrapper.IsInitialized()) {
    callback_(wrapper.msg_type(), wrapper.payload());
  }
}

void MessageHandler::OnError(const TransportCondition &transport_condition,
                             const Endpoint &remote_endpoint) {
  LOG(kError) << "OnError (" << transport_condition << ")";
  (*on_error_)(transport_condition, remote_endpoint);
}

void MessageHandler::ProcessSerialisedMessage(
    const int &/*message_type*/,
    const std::string &/*payload*/,
    const SecurityType &/*security_type*/,
    const std::string &/*message_signature*/,
    const Info & /*info*/,
    std::string* /*message_response*/,
    Timeout* /*timeout*/) {}

std::string MessageHandler::MakeSerialisedWrapperMessage(
    const int &message_type,
    const std::string &payload,
    SecurityType security_type,
    const PublicKey &/*recipient_public_key*/) {
  protobuf::WrapperMessage wrapper_message;
  wrapper_message.set_msg_type(message_type);
  wrapper_message.set_payload(payload);
  std::string final_message(1, security_type);

  // No security.
//  if (security_type == kNone) {
    final_message += wrapper_message.SerializeAsString();
//  } else {
    // If we asked for security but provided no securifier, fail.
//     if (security_type && !private_key_) {
//       LOG(kError) << "MakeSerialisedWrapperMessage - type " << message_type
//                   << " - PrivateKey Validation Failed.";
//       return "";
//     }

    // Handle signing
//     if (security_type & kSign) {
//       std::string signature;
//       if (asymm::Sign(boost::lexical_cast<std::string>(message_type) + payload,
//                      *private_key_,
//                      &signature) != kSuccess) {
//        LOG(kError) << "MakeSerialisedWrapperMessage - type " << message_type
//                    << " - Sign Failed.";
//        return "";
//       }
//       wrapper_message.set_message_signature(signature);
//     }

    // Handle encryption
    /*if (security_type & kAsymmetricEncrypt) {
      if (!asymm::ValidateKey(recipient_public_key)) {
        LOG(kError) << "MakeSerialisedWrapperMessage - type " << message_type
                    << " - PublicKey Validation Failed.";
        return "";
      }

      std::string encrypted_message;
      if (asymm::Encrypt(wrapper_message.SerializeAsString(),
                         recipient_public_key,
                         &encrypted_message) != kSuccess) {
        LOG(kError) << "MakeSerialisedWrapperMessage - type " << message_type
                    << " - Encryption Failed.";
        return "";
      }
      final_message += encrypted_message;
    } else {
      final_message += wrapper_message.SerializeAsString();
    }*/
//   }
  return final_message;
}

void MessageHandler::SetCallback(boost::function<void(int, std::string)> callback) {
  callback_ = callback;
}

}  //  namespace priv

}  //  namespace maidsafe
