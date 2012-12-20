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

#include "maidsafe/lifestuff_manager/utils.h"

#include <string>

#include "maidsafe/common/test.h"


namespace maidsafe {

namespace lifestuff_manager {

namespace detail {

namespace test {

TEST(UtilsTest, DISABLED_BEH_WrapAndUnwrapMessage) {
  FAIL() << "Needs test";
}

TEST(UtilsTest, BEH_GenerateVmidParameter) {
  EXPECT_EQ("0_0", GenerateVmidParameter(0, 0));
  EXPECT_EQ("0_65535", GenerateVmidParameter(0, 65535));
  EXPECT_EQ("1_65535", GenerateVmidParameter(1, 65535));
  EXPECT_EQ("1000_65535", GenerateVmidParameter(1000, 65535));
  EXPECT_EQ("1000_0", GenerateVmidParameter(1000, 0));
}

TEST(UtilsTest, DISABLED_BEH_ParseVmidParameter) {
  FAIL() << "Needs test";
}

}  // namespace test

}  // namespace detail

}  // namespace lifestuff_manager

}  // namespace maidsafe
