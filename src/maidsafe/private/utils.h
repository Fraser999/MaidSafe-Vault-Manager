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

#ifndef MAIDSAFE_PRIVATE_UTILS_H_
#define MAIDSAFE_PRIVATE_UTILS_H_

#include <string>
#include <vector>
#include "maidsafe/private/transport.h"


namespace maidsafe {

namespace priv {

// Convert an IP in ASCII format to IPv4 or IPv6 bytes
std::string IpAsciiToBytes(const std::string &decimal_ip);

// Convert an IPv4 or IPv6 in bytes format to ASCII format
std::string IpBytesToAscii(const std::string &bytes_ip);

// Convert an internet network address into dotted string format.
void IpNetToAscii(uint32_t address, char *ip_buffer);

// Convert a dotted string format internet address into Ipv4 format.
uint32_t IpAsciiToNet(const char *buffer);

// Return all local addresses
std::vector<IP> GetLocalAddresses();

}  // namespace priv

}  // namespace maidsafe

#endif  // MAIDSAFE_PRIVATE_UTILS_H_
