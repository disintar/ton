// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include <vector>
#include <string.h>
#include "tonlib/tonlib/keys/Mnemonic.h"

#ifndef TON_PYKEYS_H
#define TON_PYKEYS_H

class PyMnemonic {
 public:
  tonlib::Mnemonic my_mnemo;
  PyMnemonic(std::vector<std::string> mnemo, std::string mnemonic_password);
  PyMnemonic(tonlib::Mnemonic mnemo) : my_mnemo(std::move(mnemo)){};
  std::vector<std::string> get_words();
  std::string get_private_key_hex();
};

PyMnemonic create_new_mnemo(std::string entropy, std::string password, int words_count);
std::vector<std::string> get_bip39_words();
#endif  //TON_PYKEYS_H
