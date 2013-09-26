/*  Copyright 2012 MaidSafe.net limited

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

#ifndef MAIDSAFE_LIFESTUFF_MANAGER_TESTS_TEST_UTILS_H_
#define MAIDSAFE_LIFESTUFF_MANAGER_TESTS_TEST_UTILS_H_

#include <string>

namespace maidsafe {

namespace lifestuff_manager {

namespace test {

int GetNumRunningProcesses(const std::string& process_name);

}  // namespace test

}  // namespace lifestuff_manager

}  // namespace maidsafe

#endif  // MAIDSAFE_LIFESTUFF_MANAGER_TESTS_TEST_UTILS_H_
