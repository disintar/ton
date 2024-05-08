// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "PyFunc.h"

class CompilationException : public std::runtime_error {
public:
    explicit CompilationException(const std::string& message)
        : std::runtime_error(message) {}
};

std::string compile_from_sources(const std::vector<std::string>& sources) {
    std::ostringstream output;
    funC::read_callback = funC::fs_read_callback;
    int result = funC::func_proceed(sources, output, std::cerr);
    if (result != 0) {
        throw CompilationException("Compilation failed!");
    }
    return output.str();
}

std::string func_to_asm(const std::vector<std::string>& sources,
                        bool preamble,
                        int indent,
                        bool verbosity,
                        int optimization,
                        bool envelope,
                        bool stack_comments,
                        bool op_comments
                        ) {
    funC::asm_preamble = preamble;
    funC::indent = indent;
    if (verbosity) ++funC::verbosity;
    funC::opt_level = optimization;
    funC::program_envelope = envelope;
    funC::stack_layout_comments = stack_comments;
    funC::op_rewrite_comments = op_comments;

    std::ostringstream err;
    std::string res = compile_from_sources(sources);
    return res;
}

std::string func_string_to_asm(const std::string& source,
                               bool preamble,
                               int indent,
                               bool verbosity,
                               int optimization,
                               bool envelope,
                               bool stack_comments,
                               bool op_comments
                               ) {
    funC::asm_preamble = preamble;
    funC::indent = indent;
    if (verbosity) ++funC::verbosity;
    funC::opt_level = optimization;
    funC::program_envelope = envelope;
    funC::stack_layout_comments = stack_comments;
    funC::op_rewrite_comments = op_comments;
    funC::interactive_from_string = true;
    std::string res = compile_from_sources(std::vector<std::string>{source});
    return res;
}