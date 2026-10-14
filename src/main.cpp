#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "SPSCQueue/SPSCQueue.hpp"
#include "TSQueue/TSQueue.hpp"
#include "Parser/Parser.hpp"

namespace py = pybind11;

PYBIND11_MODULE(PyQuantLib, module)
{
    // Queues
    using ISPSCQueue = SPSCQueue<int>;
    using DSPSCQueue = SPSCQueue<double>;

    py::class_<ISPSCQueue>(module, "ISPSCQueue")
        .def(py::init<std::size_t>(), py::arg("capacity"))
        .def("try_push", [](ISPSCQueue& queue, int value) {
            return queue.try_emplace(value);
        }, py::arg("value"), py::call_guard<py::gil_scoped_release>())
        .def("try_pop", &ISPSCQueue::try_pop,
             py::call_guard<py::gil_scoped_release>());

    py::class_<DSPSCQueue>(module, "DSPSCQueue")
        .def(py::init<std::size_t>(), py::arg("capacity"))
        .def("try_push", [](DSPSCQueue& queue, double value){
            return queue.try_emplace(value);
        }, py::call_guard<py::gil_scoped_release>())
        .def("try_pop", &DSPSCQueue::try_pop,
            py::call_guard<py::gil_scoped_release>());

    // SIMD Architecture Detection
    py::enum_<DIA>(module, "DIA")
        .value("NONE", DIA::NONE)
        .value("SSE4_2", DIA::SSE4_2)
        .value("AVX2", DIA::AVX2)
        .value("AVX512", DIA::AVX512)
        .value("NEON", DIA::NEON)
        .export_values();

    // JSON Parser
    py::enum_<JsonType>(module, "JsonType")
        .value("Null", JsonType::Null)
        .value("Bool", JsonType::Bool)
        .value("Number", JsonType::Number)
        .value("String", JsonType::String)
        .value("Array", JsonType::Array)
        .value("Object", JsonType::Object)
        .export_values();

    py::class_<JsonValue>(module, "JsonValue")
        .def(py::init<>())
        .def(py::init<bool>())
        .def(py::init<double>())
        .def(py::init<std::string>())
        .def("type", &JsonValue::type)
        .def("is_null", &JsonValue::is_null)
        .def("is_bool", &JsonValue::is_bool)
        .def("is_number", &JsonValue::is_number)
        .def("is_string", &JsonValue::is_string)
        .def("is_array", &JsonValue::is_array)
        .def("is_object", &JsonValue::is_object)
        .def("as_bool", &JsonValue::as_bool)
        .def("as_number", &JsonValue::as_number)
        .def("as_string", &JsonValue::as_string)
        .def("as_array", &JsonValue::as_array)
        .def("as_object", &JsonValue::as_object)
        .def("__getitem__", py::overload_cast<size_t>(&JsonValue::operator[]), py::return_value_policy::reference_internal)
        .def("__getitem__", py::overload_cast<const std::string&>(&JsonValue::operator[]), py::return_value_policy::reference_internal);

    py::class_<JsonDocument>(module, "JsonDocument")
        .def(py::init<>())
        .def_static("parse", &JsonDocument::parse, py::arg("json"))
        .def("root", py::overload_cast<>(&JsonDocument::root), py::return_value_policy::reference_internal)
        .def("is_valid", &JsonDocument::is_valid);

    py::class_<JsonParseError>(module, "JsonParseError")
        .def("position", &JsonParseError::position);

    // Initialize SIMD architecture detection
    module.def("init_parser", &init, "Initialize the JSON parser");
    module.def("get_simd_arch", &active_arch, "Get the active SIMD architecture");
    module.def("simd_arch_to_string", [](DIA arch) {
        return std::string(to_string(arch));
    }, "Convert SIMD architecture to string");
}
