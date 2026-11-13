// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the bind_*.cpp files: the pybind11 namespace alias, conversions
// between Python objects and the core's small value types, and the type
// casters that let functions take Vec3f sequences, point buffers, ParamValue
// and graph Values straight from Python.

#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_session.h"
#include "groomlab/math.h"
#include "groomlab/ramp.h"
#include "groomlab/map_ref.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace py = pybind11;

namespace groomlab::py_bind {

// ---- Vec3f and point lists ----------------------------------------------

// A Vec3 instance or a sequence of three numbers.
bool loadVec3(py::handle src, Vec3f& out);
// A sequence of Vec3/3-sequences, or a float32/float64 buffer shaped (N, 3) or (3N,).
bool loadPoints(py::handle src, std::vector<Vec3f>& out);
std::vector<Vec3f> toPoints(py::handle src);  // throws TypeError
// A buffer or sequence of ints (flat, 3 per triangle) or of 3-sequences.
std::vector<ScalpMesh::Triangle> toTriangles(py::handle src);
// N floats per entry: a buffer shaped (M, N) or (M*N,), or a sequence of N-sequences.
template <std::size_t N>
std::vector<std::array<float, N>> toFloatTuples(py::handle src);
// A buffer or sequence of numbers.
std::vector<float> toFloats(py::handle src);

py::tuple toTuple(const Vec3f& v);

// ---- Variants ------------------------------------------------------------

bool loadParamValue(py::handle src, ParamValue& out);
py::object castParamValue(const ParamValue& v);
const char* paramKind(const ParamValue& v);  // bool, int, float, string, vec3, ramp, map

bool loadValue(py::handle src, Value& out);
py::object castValue(const Value& v);

// ---- Buffer views ----------------------------------------------------------

// A view over an array owned by another Python object (a StrandSet, a
// ScalpMesh...): supports the buffer protocol, so memoryview(view) and
// numpy.asarray(view) read (or write) the core's memory without a copy.
struct ArrayView {
    py::object owner;
    void* data = nullptr;
    py::ssize_t itemsize = 0;
    std::string format;
    std::vector<py::ssize_t> shape;
    bool readonly = true;

    py::ssize_t count() const;  // product of the shape
    py::buffer_info info() const;
};

template <class T>
ArrayView makeView(py::object owner, const T* data, std::vector<py::ssize_t> shape, bool readonly,
                   std::string format = py::format_descriptor<T>::format()) {
    ArrayView v;
    v.owner = std::move(owner);
    v.data = const_cast<void*>(static_cast<const void*>(data));
    v.itemsize = static_cast<py::ssize_t>(sizeof(T));
    v.format = std::move(format);
    v.shape = std::move(shape);
    v.readonly = readonly;
    return v;
}

// (N, 3) float view of Vec3f storage.
inline ArrayView pointsView(py::object owner, const std::vector<Vec3f>& v, bool readonly) {
    return makeView(std::move(owner), reinterpret_cast<const float*>(v.data()),
                    {static_cast<py::ssize_t>(v.size()), 3}, readonly);
}
template <class T>
ArrayView flatView(py::object owner, const std::vector<T>& v, bool readonly) {
    return makeView(std::move(owner), v.data(), {static_cast<py::ssize_t>(v.size())}, readonly);
}

// PEP 3118 struct format of StrandRoot: int32 face, float u, float v.
extern const char* const kRootFormat;

// Python's view of a GroomSession's ViewResult (bind_session.cpp): owns what it shows (shared
// pointers), so it outlives the next evaluation. DisplayBuilder.update takes one (bind_display.cpp).
struct PyView {
    ViewResult result;
};

// ---- JSON helpers -----------------------------------------------------------

py::object jsonLoads(const std::string& text);
std::string jsonDumps(py::handle obj);
// A str is taken as JSON text; anything else is json.dumps'ed.
std::string jsonText(py::handle obj);

// ---- Module sections ---------------------------------------------------------

void bindBasic(py::module_& m);
void bindStrands(py::module_& m);
void bindHierarchy(py::module_& m);
void bindGraph(py::module_& m);
void bindGroom(py::module_& m);
void bindSession(py::module_& m);  // after bindGroom (GroomData)
void bindDisplay(py::module_& m);  // after bindSession (ViewResult)
void bindStroke(py::module_& m);   // after bindSession (GroomSession)
void bindAlembic(py::module_& m);  // a no-op stub without groomlab_alembic
void bindReference(py::module_& m);
void bindCardBlocks(py::module_& m);

}  // namespace groomlab::py_bind

// ---- Type casters -------------------------------------------------------------

namespace pybind11::detail {

// Graph holds a map of unique_ptr nodes: not copyable, though MSVC's std::map
// declares a copy constructor unconditionally, which would make pybind11
// instantiate one. Say so explicitly.
template <> struct is_copy_constructible<groomlab::Graph> : std::false_type {};
template <> struct is_copy_assignable<groomlab::Graph> : std::false_type {};
template <> struct is_copy_constructible<groomlab::GraphDocument> : std::false_type {};
template <> struct is_copy_assignable<groomlab::GraphDocument> : std::false_type {};

// std::vector<Vec3f>: loads from sequences of Vec3/tuples or float buffers;
// casts to a list of Vec3 copies (never references into the vector).
template <>
struct type_caster<std::vector<groomlab::Vec3f>> {
    PYBIND11_TYPE_CASTER(std::vector<groomlab::Vec3f>, const_name("Sequence[Vec3]"));

    bool load(handle src, bool) { return groomlab::py_bind::loadPoints(src, value); }

    static handle cast(const std::vector<groomlab::Vec3f>& src, return_value_policy, handle) {
        list out(src.size());
        for (std::size_t i = 0; i < src.size(); ++i) {
            out[i] = pybind11::cast(src[i], return_value_policy::copy);
        }
        return out.release();
    }
};

template <>
struct type_caster<groomlab::ParamValue> {
    PYBIND11_TYPE_CASTER(groomlab::ParamValue, const_name("ParamValue"));

    bool load(handle src, bool) { return groomlab::py_bind::loadParamValue(src, value); }

    static handle cast(const groomlab::ParamValue& src, return_value_policy, handle) {
        return groomlab::py_bind::castParamValue(src).release();
    }
};

template <>
struct type_caster<groomlab::Value> {
    PYBIND11_TYPE_CASTER(groomlab::Value, const_name("Value"));

    bool load(handle src, bool) { return groomlab::py_bind::loadValue(src, value); }

    static handle cast(const groomlab::Value& src, return_value_policy, handle) {
        return groomlab::py_bind::castValue(src).release();
    }
};

}  // namespace pybind11::detail
