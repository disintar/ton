// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once
#include <string>
#include "td/utils/Slice.h"
namespace ton::validator::toncenter {
struct Response {
  long http_status = 0;
  int transport_error = 0;
  bool accepted = false;
  std::string message_hash;
};
// Blocking helper for the dedicated worker and standalone diagnostic client only.
Response post_message(const std::string &endpoint, const std::string &api_key, td::Slice boc);
// Nonblocking, bounded best-effort submission. Only call after node admission succeeds.
#if defined(TON_WITH_TONCENTER_RELAY)
void submit(td::Slice boc);
#else
inline void submit(td::Slice) {}
#endif
}
