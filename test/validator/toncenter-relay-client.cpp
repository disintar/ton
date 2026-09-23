// SPDX-License-Identifier: LGPL-2.0-or-later
#include "validator/toncenter-relay.h"
#include "validator/toncenter-relay-metrics.h"
#include "td/utils/base64.h"
#include <cstdlib>
#include <iostream>
#include <string>
int main(int argc, char **argv) {
  std::string input; std::getline(std::cin, input);
  auto boc = td::base64_decode(input);
  if (boc.is_error() || boc.ok().empty() || boc.ok().size() > 65536) return 2;
  const char *key = std::getenv("DTON_TONCENTER_API_KEY");
  auto response = ton::validator::toncenter::post_message(
      argc == 2 ? argv[1] : "https://toncenter.com/api/v3/message", key ? key : "", boc.ok());
  std::cout << "http_status=" << response.http_status << " transport_error=" << response.transport_error
            << " accepted=" << response.accepted << " message_hash=" << response.message_hash << "\n";
  return response.accepted ? 0 : 1;
}
