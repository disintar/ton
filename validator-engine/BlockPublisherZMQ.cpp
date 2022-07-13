#include "BlockPublisherZMQ.hpp"

namespace ton::validator {

BlockPublisherZMQ::BlockPublisherZMQ(const std::string& endpoint) : socket(ctx, zmq::socket_type::pub) {
  socket.bind(endpoint);
}

void BlockPublisherZMQ::publishBlockData(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  socket.send(zmq::str_buffer("block/data"), zmq::send_flags::sndmore);
  socket.send(zmq::message_t(json.c_str(), json.size()));
}

void BlockPublisherZMQ::publishBlockState(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  socket.send(zmq::str_buffer("block/state"), zmq::send_flags::sndmore);
  socket.send(zmq::message_t(json.c_str(), json.size()));
}

}
