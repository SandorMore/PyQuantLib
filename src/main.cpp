#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "SPSCQueue/SPSCQueue.hpp"
#include "TSQueue/TSQueue.hpp"
#include "Parser/Parser.hpp"
#include "Parser/JsonTape.hpp"
#include "OrderBook/OrderBook.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace py = pybind11;

namespace {

py::object checked(PyObject* obj) {
    if (!obj) throw py::error_already_set();
    return py::reinterpret_steal<py::object>(obj);
}

// Accepts only immutable buffers (str, bytes), so the view stays valid while the
// GIL is released.
std::string_view json_view(const py::handle& data) {
    if (PyUnicode_Check(data.ptr())) {
        Py_ssize_t size = 0;
        const char* chars = PyUnicode_AsUTF8AndSize(data.ptr(), &size);
        if (!chars) throw py::error_already_set();
        return {chars, static_cast<size_t>(size)};
    }
    if (PyBytes_Check(data.ptr())) {
        return {PyBytes_AS_STRING(data.ptr()), static_cast<size_t>(PyBytes_GET_SIZE(data.ptr()))};
    }
    throw py::type_error("JSON input must be str or bytes");
}

// Builds Python objects directly from the structural indexes, skipping the
// JsonValue tree. Integers become int (arbitrary precision), like json.loads.
struct PySink {
    using Value = py::object;
    using Array = py::list;
    using Object = py::dict;
    using Key = py::object;

    // Object keys repeat across records, so reuse their str objects.
    std::unordered_map<std::string, py::object> key_cache;
    static constexpr size_t MAX_CACHED_KEYS = 4096;

    Value make_null() { return py::none(); }
    Value make_bool(bool b) { return py::bool_(b); }
    Value make_double(double d) { return checked(PyFloat_FromDouble(d)); }

    Value make_integer(std::string_view text, uint64_t magnitude, bool negative, bool exact) {
        if (exact) {
            if (!negative) return checked(PyLong_FromUnsignedLongLong(magnitude));
            if (magnitude <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return checked(PyLong_FromLongLong(-static_cast<long long>(magnitude)));
            }
        }
        const std::string digits(text);
        return checked(PyLong_FromString(digits.c_str(), nullptr, 10));
    }

    Value make_string(std::string& s) { return py::str(s.data(), s.size()); }

    Key make_key(std::string& s) {
        if (auto it = key_cache.find(s); it != key_cache.end()) return it->second;
        py::object key = py::str(s.data(), s.size());
        if (key_cache.size() < MAX_CACHED_KEYS) key_cache.emplace(s, key);
        return key;
    }

    Array new_array() { return py::list(); }
    void push(Array& a, Value&& v) {
        if (PyList_Append(a.ptr(), v.ptr()) != 0) throw py::error_already_set();
    }
    Value finish(Array&& a) { return std::move(a); }

    Object new_object() { return py::dict(); }
    void insert(Object& o, Key&& k, Value&& v) {
        if (PyDict_SetItem(o.ptr(), k.ptr(), v.ptr()) != 0) throw py::error_already_set();
    }
    Value finish(Object&& o) { return std::move(o); }
};

py::object json_loads(const py::object& data) {
    struct Release {
        ~Release() { json_detail::release_index_buffer(); }
    } release;

    const std::string_view json = json_view(data);
    json_detail::IndexView idx;
    {
        py::gil_scoped_release nogil;
        idx = json_detail::index_document(json);
    }
    PySink sink;
    return json_detail::TapeWalker<PySink>(json, idx, sink).parse_document();
}

py::object to_python(const JsonValue& v) {
    switch (v.type()) {
        case JsonType::Null: return py::none();
        case JsonType::Bool: return py::bool_(v.as_bool());
        case JsonType::Number: return checked(PyFloat_FromDouble(v.as_number()));
        case JsonType::String: return py::str(v.as_string());
        case JsonType::Array: {
            const auto& arr = v.as_array();
            py::list out(arr.size());
            for (size_t i = 0; i < arr.size(); ++i) {
                out[i] = to_python(arr[i]);
            }
            return out;
        }
        case JsonType::Object: {
            py::dict out;
            for (const auto& [key, value] : v.as_object()) {
                out[py::str(key)] = to_python(value);
            }
            return out;
        }
    }
    return py::none();
}

} // namespace

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
        .def("__getitem__", py::overload_cast<const std::string&>(&JsonValue::operator[]), py::return_value_policy::reference_internal)
        .def("to_python", &to_python, "Convert to native Python objects (numbers become float)");

    py::class_<JsonDocument>(module, "JsonDocument")
        .def(py::init<>())
        .def_static("parse", [](const py::object& json) {
            const std::string_view view = json_view(json);
            py::gil_scoped_release nogil;
            return JsonDocument::parse(view);
        }, py::arg("json"), "Parse str or bytes into a JsonDocument (releases the GIL)")
        .def("root", py::overload_cast<>(&JsonDocument::root), py::return_value_policy::reference_internal)
        .def("is_valid", &JsonDocument::is_valid);

    py::register_exception<JsonParseError>(module, "JsonParseError", PyExc_ValueError);

    module.def("loads", &json_loads, py::arg("data"),
               "Parse str or bytes straight into Python objects, like json.loads");

    // Order Book
    py::enum_<Side>(module, "Side")
        .value("Buy", Side::Buy)
        .value("Sell", Side::Sell);

    py::enum_<OrderType>(module, "OrderType")
        .value("Limit", OrderType::Limit)
        .value("Market", OrderType::Market);

    py::enum_<TimeInForce>(module, "TimeInForce")
        .value("GTC", TimeInForce::GoodTillCancel)
        .value("IOC", TimeInForce::ImmediateOrCancel)
        .value("FOK", TimeInForce::FillOrKill);

    py::enum_<OrderStatus>(module, "OrderStatus")
        .value("New", OrderStatus::New)
        .value("PartiallyFilled", OrderStatus::PartiallyFilled)
        .value("Filled", OrderStatus::Filled)
        .value("Cancelled", OrderStatus::Cancelled)
        .value("Rejected", OrderStatus::Rejected);

    py::class_<OrderBook::ExecReport>(module, "ExecReport")
        .def_readonly("id", &OrderBook::ExecReport::id)
        .def_readonly("status", &OrderBook::ExecReport::status)
        .def_readonly("filled", &OrderBook::ExecReport::filled)
        .def_readonly("remaining", &OrderBook::ExecReport::remaining);

    py::class_<OrderBook::Trade>(module, "Trade")
        .def_readonly("maker", &OrderBook::Trade::maker)
        .def_readonly("taker", &OrderBook::Trade::taker)
        .def_readonly("price", &OrderBook::Trade::price)
        .def_readonly("qty", &OrderBook::Trade::qty)
        .def_readonly("taker_side", &OrderBook::Trade::takerSide);

    py::class_<OrderBook>(module, "OrderBook")
        .def(py::init<std::uint32_t, std::size_t>(), py::arg("max_orders"), py::arg("expected_levels") = 4096)
        .def("submit", &OrderBook::submit, py::arg("side"), py::arg("type"), py::arg("tif"),
             py::arg("price"), py::arg("qty"), py::call_guard<py::gil_scoped_release>())
        .def("cancel", &OrderBook::cancel, py::arg("id"))
        .def("reduce", &OrderBook::reduce, py::arg("id"), py::arg("new_qty"))
        .def("replace", &OrderBook::replace, py::arg("id"), py::arg("price"), py::arg("qty"),
             py::call_guard<py::gil_scoped_release>())
        .def("trades", [](const OrderBook& ob) {
            const auto t = ob.trades();
            return std::vector<OrderBook::Trade>(t.begin(), t.end());
        }, "Trades from the last submit/replace")
        .def("best_bid", &OrderBook::bestBid)
        .def("best_ask", &OrderBook::bestAsk)
        .def("volume_at", &OrderBook::volumeAt, py::arg("side"), py::arg("price"))
        .def("depth", [](const OrderBook& ob, Side side, std::size_t levels) {
            py::list out;
            for (const auto& l : ob.depth(side, levels)) out.append(py::make_tuple(l.price, l.qty));
            return out;
        }, py::arg("side"), py::arg("levels"), "List of (price, qty), best first")
        .def("find", [](const OrderBook& ob, OrderId id) -> py::object {
            const auto o = ob.find(id);
            if (!o) return py::none();
            return py::make_tuple(o->id, o->side, o->price, o->qty);
        }, py::arg("id"), "(id, side, price, qty) or None")
        .def("order_count", &OrderBook::orderCount)
        .def("level_count", &OrderBook::levelCount, py::arg("side"));

    // Initialize SIMD architecture detection
    module.def("init_parser", &init, "Initialize the JSON parser");
    module.def("get_simd_arch", &active_arch, "Get the active SIMD architecture");
    module.def("simd_arch_to_string", [](DIA arch) {
        return std::string(to_string(arch));
    }, "Convert SIMD architecture to string");
}
