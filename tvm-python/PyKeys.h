// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <vector>
#include <string.h>
#include "tonlib/tonlib/keys/Mnemonic.h"

namespace py = pybind11;

#ifndef TON_PYKEYS_H
#define TON_PYKEYS_H

class PyPublicKey {
 public:
  td::Ed25519::PublicKey key;
  PyPublicKey(std::string key_int);
  PyPublicKey(td::Ed25519::PublicKey key_) : key(std::move(key_)){};
  std::string get_public_key_hex();
  std::tuple<bool, std::string> verify_signature(const char *data, const char *signature);

  PyPublicKey(const PyPublicKey& other) : key(td::Ed25519::PublicKey(other.key.as_octet_string())){};
};

class PyPrivateKey {
 public:
  td::Ed25519::PrivateKey key;
  PyPrivateKey();
  PyPrivateKey(std::string key_int);
  PyPrivateKey(td::Ed25519::PrivateKey key_) : key(td::Ed25519::PrivateKey(key_.as_octet_string())){};
  std::string get_private_key_hex();
  PyPublicKey get_public_key();
  py::bytes sign(const char* data);
};

class PyMnemonic {
 public:
  tonlib::Mnemonic my_mnemo;
  PyMnemonic(std::vector<std::string> mnemo, std::string mnemonic_password);
  PyMnemonic(tonlib::Mnemonic mnemo) : my_mnemo(std::move(mnemo)){};
  std::vector<std::string> get_words();
  std::string get_private_key_hex();
  PyPrivateKey get_private_key();
};

PyMnemonic create_new_mnemo(std::string entropy, std::string password, int words_count);
std::vector<std::string> get_bip39_words();
#endif  //TON_PYKEYS_H
