// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "PyKeys.h"
#include "tonlib/tonlib/keys/bip39.h"
#include "td/utils/misc.h"
#include "crypto/Ed25519.h"
#include "crypto/common/bigint.hpp"

PyPrivateKey::PyPrivateKey() : key(td::Ed25519::generate_private_key().move_as_ok()) {
}

std::string hex_to_bytes(std::string str) {
  std::string t;

  if (str.size() & 1) {
    throw std::invalid_argument("not a hex string");
  }
  t.reserve(str.size() >> 1);

  std::size_t i;
  unsigned f = 0;
  for (i = 0; i < str.size(); i++) {
    int c = str[i];
    if (c >= '0' && c <= '9') {
      c -= '0';
    } else {
      c |= 0x20;
      if (c >= 'a' && c <= 'f') {
        c -= 'a' - 10;
      } else {
        break;
      }
    }
    f = (f << 4) + c;
    if (i & 1) {
      t += (char)(f & 0xff);
    }
  }

  return t;
}

std::string to_hex(td::Slice buffer) {
  const char *hex = "0123456789ABCDEF";
  std::string res(2 * buffer.size(), '\0');
  for (std::size_t i = 0; i < buffer.size(); i++) {
    auto c = buffer.ubegin()[i];  // Iterate in the original order
    res[2 * (buffer.size() - 1 - i)] = hex[c >> 4];
    res[2 * (buffer.size() - 1 - i) + 1] = hex[c & 15];
  }
  return res;
}

std::string public_buffer_to_hex(td::Slice buffer) {
  const char *hex = "0123456789ABCDEF";
  std::string res(2 * buffer.size(), '\0');
  for (std::size_t i = 0; i < buffer.size(); i++) {
    auto c = buffer.ubegin()[i];
    res[2 * i] = hex[(c >> 4) & 0xF];
    res[2 * i + 1] = hex[c & 0xF];
  }
  return res;
}

PyPublicKey::PyPublicKey(std::string key_int) : key(td::Ed25519::PublicKey{td::SecureString{hex_to_bytes(key_int)}}) {
}

std::tuple<bool, std::string> PyPublicKey::verify_signature(py::bytes data, py::bytes signature) {
  auto R = key.verify_signature(td::Slice(std::move(std::string(data))), td::Slice(std::move(std::string(signature))));
  std::string err_msg;
  bool valid = true;

  if (R.is_error()) {
    valid = false;
    err_msg = R.move_as_error().to_string();
  }

  return std::tuple<bool, std::string>(valid, err_msg);
};

std::string PyPublicKey::get_public_key_hex() {
  return public_buffer_to_hex(key.as_octet_string());
}

PyPrivateKey::PyPrivateKey(std::string key_int)
    : key(td::Ed25519::PrivateKey{td::SecureString{hex_to_bytes(key_int)}}) {
}

std::string PyPrivateKey::get_private_key_hex() {
  return to_hex(key.as_octet_string());
}

py::bytes PyPrivateKey::sign(py::bytes data) {
  auto R = key.sign(td::Slice(std::move(std::string(data))));
  if (R.is_error()) {
    throw std::invalid_argument(R.move_as_error().to_string());
  }
  return std::move(R.move_as_ok().as_slice().str());
};

PyPublicKey PyPrivateKey::get_public_key() {
  return PyPublicKey(key.get_public_key().move_as_ok());
}

PyMnemonic::PyMnemonic(std::vector<std::string> mnemo, std::string mnemonic_password)
    : my_mnemo(tonlib::Mnemonic::create(std::vector<td::SecureString>(mnemo.begin(), mnemo.end()),
                                        td::SecureString(mnemonic_password))
                   .move_as_ok()) {
}

std::vector<std::string> PyMnemonic::get_words() {
  std::vector<std::string> words;
  auto tmp = my_mnemo.get_words();
  words.reserve(tmp.size());

  for (size_t i = 0; i < tmp.size(); i++) {
    words.emplace_back(tmp[i].as_slice().str());
  }

  return words;
}

std::string PyMnemonic::get_private_key_hex() {
  return to_hex(my_mnemo.to_private_key().as_octet_string());
}

PyPrivateKey PyMnemonic::get_private_key() {
  return PyPrivateKey(my_mnemo.to_private_key());
}

PyMnemonic create_new_mnemo(std::string entropy, std::string password, int words_count) {
  tonlib::Mnemonic::Options opts;

  if (!entropy.empty()) {
    opts.entropy = td::SecureString(std::move(entropy));
  }

  if (!password.empty()) {
    opts.password = td::SecureString(std::move(password));
  }

  opts.words_count = words_count;

  return PyMnemonic(tonlib::Mnemonic::create_new(std::move(opts)).move_as_ok());
}

std::vector<std::string> get_bip39_words() {
  std::vector<std::string> words;
  auto tmp = tonlib::Mnemonic::normalize_and_split(td::SecureString(tonlib::bip39_english()));
  words.reserve(tmp.size());

  for (size_t i = 0; i < tmp.size(); i++) {
    words.emplace_back(tmp[i].as_slice().str());
  }

  return words;
}