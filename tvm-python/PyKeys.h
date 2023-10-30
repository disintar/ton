//
// Created by Andrey Tvorozhkov on 10/26/23.
//
#include <vector>
#include <string.h>
#include "tonlib/tonlib/keys/Mnemonic.h"

#ifndef TON_PYKEYS_H
#define TON_PYKEYS_H

// This is needed to convert python strings
class PySecureString {
  td::SecureString my_secure;

  PySecureString(std::string data) {
    my_secure = td::SecureString(std::move(data));
  }

  std::string reveal() {
    return my_secure.data();
  }
};

class PyMnemonic {
  tonlib::Mnemonic my_mnemo;

  PyMnemonic(std::vector<PySecureString> mnemo, PySecureString mnemonic_password) {
    my_mnemo = tonlib::Mnemonic(mnemo)
  }
};

#endif  //TON_PYKEYS_H
