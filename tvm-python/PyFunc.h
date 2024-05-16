// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include <string>
#include <stdexcept>
#include "crypto/func/func.h"


#ifndef TON_FUNC_H
#define TON_FUNC_H

std::string compile_from_sources(const std::vector<std::string>& sources, std::ostringstream& errors);
std::string func_to_asm(const std::vector<std::string>& sources, 
                        bool preamble,
                        int indent,
                        bool verbosity,
                        int optimization,
                        bool envelope,
                        bool stack_comments,
                        bool op_comments
                        );
std::string func_string_to_asm(const std::string& source,
                        bool preamble,
                        int indent,
                        bool verbosity,
                        int optimization,
                        bool envelope,
                        bool stack_comments,
                        bool op_comments
                        );

#endif  //TON_FUNC_H