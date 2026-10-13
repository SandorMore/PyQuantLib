#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "SPSCQueue/SPSCQueue.hpp"

namespace py = pybind11;

PYBIND11_MODULE(QuantLib, module)
{
    using ISPSCQueue = SPSCQueue<int>;

    py::class_<ISPSCQueue>(module, "SPSCQueue")
        .def(py::init<std::size_t>(), py::arg("capacity"))
        .def("try_push", [](ISPSCQueue& queue, int value) {
            return queue.try_emplace(value);
        }, py::arg("value"), py::call_guard<py::gil_scoped_release>())
        .def("try_pop", &ISPSCQueue::try_pop,
             py::call_guard<py::gil_scoped_release>());
}
