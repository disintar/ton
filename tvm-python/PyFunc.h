#include <string>
#include <stdexcept>
#include "td/utils/Status.h"
#include <utility>
#include "crypto/func/func.h"
#include "crypto/fift/Fift.h"
#include "crypto/fift/utils.h"
#include "PyCell.h"


#ifndef TON_FUNC_H
#define TON_FUNC_H

std::string compile_from_sources(const std::vector<std::string>& sources, std::ostringstream& errors);
std::string func_to_asm(const std::vector<std::string>& sources, 
                        bool preamble = true,
                        int indent = 0,
                        bool verbosity = false,
                        int optimization = 0,
                        bool envelope = true,
                        bool stack_comments = true,
                        bool op_comments = false
                        );
std::string func_string_to_asm(const std::string& source,
                        bool preamble = true,
                        int indent = 0,
                        bool verbosity = false,
                        int optimization = 0,
                        bool envelope = true,
                        bool stack_comments = true,
                        bool op_comments = false
                        );

#endif  //TON_FIFT_H