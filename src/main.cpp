#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "SPSCQueue/SPSCQueue.hpp"

namespace py = pybind11;

PYBIND11_MODULE(QuantLib, module)
{
    using IntQueue = SPSCQueue<int>;

    py::class_<IntQueue>(module, "SPSCQueue")
        .def(py::init<std::size_t>(), py::arg("capacity"))
        .def("try_push", [](IntQueue& queue, int value) {
            return queue.try_emplace(value);
        }, py::arg("value"), py::call_guard<py::gil_scoped_release>())
        .def("try_pop", &IntQueue::try_pop,
             py::call_guard<py::gil_scoped_release>());
}
