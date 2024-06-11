#ifndef TON_IBLOCKREQUESTRECEIVER_HPP
#define TON_IBLOCKREQUESTRECEIVER_HPP

#include <string>
#include "ton/ton-types.h"

namespace ton {

class IBlockRequestReceiver {
 public:
  virtual ~IBlockRequestReceiver() = default;

  virtual td::Result<BlockId> getRequest() = 0;
};

}

#endif //TON_IBLOCKREQUESTRECEIVER_HPP
