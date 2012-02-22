/*
* ============================================================================
*
* Copyright [2011] maidsafe.net limited
*
* The following source code is property of maidsafe.net limited and is not
* meant for external use.  The use of this code is governed by the license
* file licence.txt found in the root of this directory and also on
* www.maidsafe.net.
*
* You are not free to copy, amend or otherwise use this source code without
* the explicit written permission of the board of directors of maidsafe.net.
*
* ============================================================================
*/

#include "maidsafe/private/chunk_actions/utils.h"

#include "maidsafe/common/crypto.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/private/return_codes.h"
#include "maidsafe/private/log.h"
#include "maidsafe/private/chunk_actions/chunk_pb.h"
#include "maidsafe/private/chunk_actions/default_rules.h"

#include "maidsafe/private/chunk_store/chunk_store.h"

namespace maidsafe {

namespace priv {

namespace chunk_actions {

void PrintToLog(const std::string &message) {
  DLOG(ERROR) << message;
}

int GetContentAndTigerHash(const std::string &name,
                           std::shared_ptr<chunk_store::ChunkStore> chunk_store,
                           std::string *chunk_content,
                           std::string *hash) {
  *chunk_content = chunk_store->Get(name);
  if (chunk_content->empty()) {
    DLOG(WARNING) << "Failed to get Tiger hash " << Base32Substr(name)
                  << " (failed to retrieve chunk from ChunkStore)";
    hash->clear();
    return kFailedToFindChunk;
  }
  *hash = crypto::Hash<crypto::Tiger>(*chunk_content);
  if (hash->empty()) {
    DLOG(ERROR) << "Failed to create Tiger hash for " << Base32Substr(name);
    return kHashFailure;
  }
  return kSuccess;
}

}  // namespace chunk_actions

}  // namespace priv

}  // namespace maidsafe

