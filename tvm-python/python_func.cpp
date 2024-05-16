// Copyright 2023 Disintar LLP / andrey@head-labs.com

#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <optional>
#include "third-party/pybind11/include/pybind11/stl.h"
#include "tvm-python/PyFunc.h"

namespace py = pybind11;
using namespace pybind11::literals;  // to bring in the `_a` literal

PYBIND11_MODULE(python_func, m) {
  PSLICE() << "";
  SET_VERBOSITY_LEVEL(verbosity_ERROR);
  
  m.def("func_to_asm", func_to_asm, py::arg("source_files: list[str]"),
                                    py::arg("preamble") = false,
                                    py::arg("indent") = 0,
                                    py::arg("verbosity") = false,
                                    py::arg("optimization") = 2,
                                    py::arg("envelope") = true,
                                    py::arg("stack_comments") = false,
                                    py::arg("op_comments") = false);
  m.def("func_string_to_asm", func_string_to_asm, py::arg("source_string: str"),
                                                  py::arg("preamble") = false,
                                                  py::arg("indent") = 0,
                                                  py::arg("verbosity") = false,
                                                  py::arg("optimization") = 2,
                                                  py::arg("envelope") = true,
                                                  py::arg("stack_comments") = false,
                                                  py::arg("op_comments") = false);


}
