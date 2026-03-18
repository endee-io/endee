#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "../storage/vector_storage.hpp"

namespace py = pybind11;
using Id = ndd::idInt;

PYBIND11_MODULE(enbee, m) {
    m.doc() = "VectorStorage Python bindings";

    // QuantVectorObject (from your engine)
    py::class_<QuantVectorObject>(m, "QuantVectorObject")
        .def(py::init<>())
        .def_readwrite("id", &QuantVectorObject::id)
        .def_readwrite("quant_vector", &QuantVectorObject::quant_vector)
        .def_readwrite("filter", &QuantVectorObject::filter)
        .def_readwrite("meta", &QuantVectorObject::meta)
        .def_readwrite("norm", &QuantVectorObject::norm);

    // VectorStorage
    py::class_<VectorStorage>(m, "VectorStorage")
        .def(py::init<const std::string&, const std::string&, size_t,
                      ndd::quant::QuantizationLevel>())

        .def("store_vectors_batch",
            [](VectorStorage &self, const std::vector<QuantVectorObject> &batch) {
                std::vector<std::pair<Id, QuantVectorObject>> tmp;
                for (const auto &q : batch) {
                    tmp.emplace_back(q.id, q);
                }
                self.store_vectors_batch(tmp);
            })

        .def("get_vector",
            [](VectorStorage &self, Id id) {
                auto raw = self.get_vector(id);
                return py::array_t<uint8_t>(raw.size(), raw.data());
            })

        .def("get_meta",
            [](VectorStorage &self, Id id) {
                return self.get_meta(id).meta;
            })

        .def("get_ids_matching_filters",
            [](VectorStorage &self,
               const std::vector<std::pair<std::string, std::string>> &pairs) {
                return self.getIdsMatchingFilters(pairs);
            });
}