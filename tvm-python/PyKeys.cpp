// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "PyKeys.h"
#include "tonlib/tonlib/keys/bip39.h"
#include "td/utils/misc.h"

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
  return td::buffer_to_hex(my_mnemo.to_private_key().as_octet_string());
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