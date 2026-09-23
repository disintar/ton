// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once
#include <string>
#include "td/utils/Slice.h"
namespace ton::validator::toncenter {
enum class Source { LiteServer, PrivateOverlay };
struct Response {
  long http_status = 0;
  int transport_error = 0;
  bool accepted = false;
  std::string message_hash;
};
// Blocking helper for the dedicated worker and standalone diagnostic client only.
Response post_message(const std::string &endpoint, const std::string &api_key, td::Slice boc);
// Nonblocking, bounded submission after local admission or from an authorized private peer.
#if defined(TON_WITH_TONCENTER_RELAY)
void submit(td::Slice boc, Source source = Source::LiteServer);
#else
inline void submit(td::Slice, Source = Source::LiteServer) {}
#endif
}
