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

#include "maidsafe/common/log.h"
#include "maidsafe/common/test.h"

int main(int argc, char **argv) {
  maidsafe::log::FilterMap filter_map;
  filter_map["*"] = maidsafe::log::kFatal;
  return ExecuteMain(argc, argv, filter_map, false, maidsafe::log::ColourMode::kFullLine);
}
