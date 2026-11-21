// SPDX-License-Identifier: Apache-2.0
// Small value types (Vec3, StrandRoot, Ramp, MapRef), the buffer view class,
// the exception mapping and the conversion helpers declared in bind_common.h.

#include "bind_common.h"

#include <cmath>
#include <limits>
#include <sstream>

#include "groomlab/expression.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/random.h"
#include "groomlab/semver.h"
#include "groomlab/texture.h"
#include "groomlab/usd_writer.h"
#include "groomlab/vrscene_writer.h"

namespace groomlab::py_bind {

const char* const kRootFormat = "T{i:face:f:u:f:v:}";

namespace {

bool isSequenceNotText(py::handle h) {
    return PySequence_Check(h.ptr()) && !PyUnicode_Check(h.ptr()) && !PyBytes_Check(h.ptr());
}

bool toFloat(py::handle h, float& out) {
    const double d = PyFloat_AsDouble(h.ptr());
    if (d == -1.0 && PyErr_Occurred()) {
        PyErr_Clear();
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

// Reads every element of a numeric buffer as double, whatever its format and
// strides. Returns false for a format that is not a plain number.
bool readNumbers(const py::buffer_info& info, std::vector<double>& out) {
    const std::string f = info.format;
    char code = f.empty() ? '\0' : f.back();
    if (f.size() > 2 || (f.size() == 2 && f[0] != '<' && f[0] != '=' && f[0] != '@')) return false;
    if (f.size() == 2 && f[0] == '>') return false;
    py::ssize_t total = 1;
    for (py::ssize_t s : info.shape) total *= s;
    out.clear();
    out.reserve(static_cast<std::size_t>(total));
    std::vector<py::ssize_t> index(static_cast<std::size_t>(info.ndim), 0);
    for (py::ssize_t n = 0; n < total; ++n) {
        const char* p = static_cast<const char*>(info.ptr);
        for (py::ssize_t d = 0; d < info.ndim; ++d) p += index[static_cast<std::size_t>(d)] * info.strides[static_cast<std::size_t>(d)];
        double v = 0.0;
        switch (code) {
            case 'f': v = *reinterpret_cast<const float*>(p); break;
            case 'd': v = *reinterpret_cast<const double*>(p); break;
            case 'b': v = *reinterpret_cast<const std::int8_t*>(p); break;
            case 'B': v = *reinterpret_cast<const std::uint8_t*>(p); break;
            case 'h': v = *reinterpret_cast<const std::int16_t*>(p); break;
            case 'H': v = *reinterpret_cast<const std::uint16_t*>(p); break;
            case 'i': v = *reinterpret_cast<const std::int32_t*>(p); break;
            case 'I': v = *reinterpret_cast<const std::uint32_t*>(p); break;
            case 'l': v = static_cast<double>(*reinterpret_cast<const long*>(p)); break;
            case 'L': v = static_cast<double>(*reinterpret_cast<const unsigned long*>(p)); break;
            case 'q': v = static_cast<double>(*reinterpret_cast<const std::int64_t*>(p)); break;
            case 'Q': v = static_cast<double>(*reinterpret_cast<const std::uint64_t*>(p)); break;
            default: return false;
        }
        out.push_back(v);
        for (py::ssize_t d = info.ndim - 1; d >= 0; --d) {
            auto& ix = index[static_cast<std::size_t>(d)];
            if (++ix < info.shape[static_cast<std::size_t>(d)]) break;
            ix = 0;
        }
    }
    return true;
}

// A buffer's numbers when `src` is one with a numeric format; false otherwise.
bool bufferNumbers(py::handle src, std::vector<double>& out, std::vector<py::ssize_t>* shape = nullptr) {
    if (!PyObject_CheckBuffer(src.ptr()) || PyBytes_Check(src.ptr()) || PyByteArray_Check(src.ptr())) return false;
    py::buffer_info info;
    try {
        info = py::reinterpret_borrow<py::buffer>(src).request();
    } catch (const py::error_already_set&) {
        return false;
    }
    if (!readNumbers(info, out)) return false;
    if (shape) *shape = info.shape;
    return true;
}

template <std::size_t N>
bool loadTuples(py::handle src, std::vector<std::array<float, N>>& out) {
    std::vector<double> nums;
    std::vector<py::ssize_t> shape;
    if (bufferNumbers(src, nums, &shape)) {
        if (nums.size() % N != 0) throw py::value_error("buffer length is not a multiple of " + std::to_string(N));
        if (shape.size() == 2 && shape[1] != static_cast<py::ssize_t>(N)) {
            throw py::value_error("buffer must be shaped (n, " + std::to_string(N) + ")");
        }
        out.resize(nums.size() / N);
        for (std::size_t i = 0; i < out.size(); ++i) {
            for (std::size_t k = 0; k < N; ++k) out[i][k] = static_cast<float>(nums[i * N + k]);
        }
        return true;
    }
    if (!isSequenceNotText(src)) return false;
    out.clear();
    for (py::handle item : py::reinterpret_borrow<py::sequence>(src)) {
        if (!isSequenceNotText(item) || py::len(item) != N) return false;
        std::array<float, N> a{};
        std::size_t k = 0;
        for (py::handle x : py::reinterpret_borrow<py::sequence>(item)) {
            if (!toFloat(x, a[k++])) return false;
        }
        out.push_back(a);
    }
    return true;
}

}  // namespace

bool loadVec3(py::handle src, Vec3f& out) {
    if (py::isinstance<Vec3f>(src)) {
        out = src.cast<Vec3f>();
        return true;
    }
    if (!isSequenceNotText(src) || py::len(src) != 3) return false;
    float c[3];
    std::size_t k = 0;
    for (py::handle x : py::reinterpret_borrow<py::sequence>(src)) {
        if (!toFloat(x, c[k++])) return false;
    }
    out = {c[0], c[1], c[2]};
    return true;
}

bool loadPoints(py::handle src, std::vector<Vec3f>& out) {
    if (src.is_none()) return false;
    std::vector<double> nums;
    std::vector<py::ssize_t> shape;
    if (bufferNumbers(src, nums, &shape)) {
        if (nums.size() % 3 != 0) throw py::value_error("point buffer length is not a multiple of 3");
        if (shape.size() == 2 && shape[1] != 3) throw py::value_error("point buffer must be shaped (n, 3)");
        out.resize(nums.size() / 3);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = {static_cast<float>(nums[3 * i]), static_cast<float>(nums[3 * i + 1]),
                      static_cast<float>(nums[3 * i + 2])};
        }
        return true;
    }
    if (!isSequenceNotText(src)) return false;
    out.clear();
    out.reserve(static_cast<std::size_t>(py::len(src)));
    for (py::handle item : py::reinterpret_borrow<py::sequence>(src)) {
        Vec3f v;
        if (!loadVec3(item, v)) return false;
        out.push_back(v);
    }
    return true;
}

std::vector<Vec3f> toPoints(py::handle src) {
    std::vector<Vec3f> out;
    if (!loadPoints(src, out)) throw py::type_error("expected a sequence of points (Vec3 or 3 numbers) or a float buffer shaped (n, 3)");
    return out;
}

std::vector<ScalpMesh::Triangle> toTriangles(py::handle src) {
    std::vector<ScalpMesh::Triangle> out;
    std::vector<double> nums;
    std::vector<py::ssize_t> shape;
    bool flat = false;
    if (bufferNumbers(src, nums, &shape)) {
        flat = true;
    } else if (isSequenceNotText(src)) {
        py::sequence seq = py::reinterpret_borrow<py::sequence>(src);
        if (py::len(seq) == 0) return out;
        if (!isSequenceNotText(seq[0])) {
            flat = true;
            for (py::handle x : seq) nums.push_back(x.cast<double>());
        } else {
            for (py::handle item : seq) {
                if (py::len(item) != 3) throw py::value_error("each triangle needs 3 indices");
                py::sequence tri = py::reinterpret_borrow<py::sequence>(item);
                out.push_back({tri[0].cast<std::int32_t>(), tri[1].cast<std::int32_t>(), tri[2].cast<std::int32_t>()});
            }
            return out;
        }
    } else {
        throw py::type_error("expected triangle indices: a flat sequence or buffer of ints, or a sequence of 3-tuples");
    }
    if (flat) {
        if (nums.size() % 3 != 0) throw py::value_error("triangle index count is not a multiple of 3");
        out.resize(nums.size() / 3);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = {static_cast<std::int32_t>(nums[3 * i]), static_cast<std::int32_t>(nums[3 * i + 1]),
                      static_cast<std::int32_t>(nums[3 * i + 2])};
        }
    }
    return out;
}

template <std::size_t N>
std::vector<std::array<float, N>> toFloatTuples(py::handle src) {
    std::vector<std::array<float, N>> out;
    if (!loadTuples<N>(src, out)) {
        throw py::type_error("expected a sequence of " + std::to_string(N) + "-tuples or a float buffer");
    }
    return out;
}
template std::vector<std::array<float, 2>> toFloatTuples<2>(py::handle);
template std::vector<std::array<float, 4>> toFloatTuples<4>(py::handle);

std::vector<float> toFloats(py::handle src) {
    std::vector<double> nums;
    if (!bufferNumbers(src, nums)) {
        if (!isSequenceNotText(src)) throw py::type_error("expected a sequence of numbers or a numeric buffer");
        for (py::handle x : py::reinterpret_borrow<py::sequence>(src)) nums.push_back(x.cast<double>());
    }
    return std::vector<float>(nums.begin(), nums.end());
}

py::tuple toTuple(const Vec3f& v) { return py::make_tuple(v.x, v.y, v.z); }

// ---- ParamValue ---------------------------------------------------------------

bool loadParamValue(py::handle src, ParamValue& out) {
    if (PyBool_Check(src.ptr())) {
        out = src.ptr() == Py_True;
        return true;
    }
    if (PyLong_Check(src.ptr())) {
        out = src.cast<std::int64_t>();
        return true;
    }
    if (PyFloat_Check(src.ptr())) {
        out = src.cast<double>();
        return true;
    }
    if (PyUnicode_Check(src.ptr())) {
        out = src.cast<std::string>();
        return true;
    }
    if (py::isinstance<Ramp>(src)) {
        out = src.cast<Ramp>();
        return true;
    }
    if (py::isinstance<MapRef>(src)) {
        out = src.cast<MapRef>();
        return true;
    }
    Vec3f v;
    if (loadVec3(src, v)) {
        out = v;
        return true;
    }
    return false;
}

py::object castParamValue(const ParamValue& v) {
    return std::visit(
        [](const auto& x) -> py::object {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Vec3f> || std::is_same_v<T, Ramp> || std::is_same_v<T, MapRef>) {
                return py::cast(x, py::return_value_policy::copy);
            } else {
                return py::cast(x);
            }
        },
        v);
}

const char* paramKind(const ParamValue& v) {
    switch (v.index()) {
        case 0: return "bool";
        case 1: return "int";
        case 2: return "float";
        case 3: return "string";
        case 4: return "vec3";
        case 5: return "ramp";
        case 6: return "map";
    }
    return "unknown";
}

// ---- Value ---------------------------------------------------------------------

bool loadValue(py::handle src, Value& out) {
    if (src.is_none()) {
        out = std::monostate{};
        return true;
    }
    if (py::isinstance<StrandSet>(src)) {
        // Copy-on-write: the copy shares the arrays until either side writes.
        out = StrandsPtr(std::make_shared<const StrandSet>(src.cast<const StrandSet&>()));
        return true;
    }
    if (py::isinstance<GuideSet>(src)) {
        out = GuidesPtr(src.cast<std::shared_ptr<GuideSet>>());
        return true;
    }
    if (py::isinstance<ScalpMesh>(src)) {
        out = MeshPtr(src.cast<std::shared_ptr<ScalpMesh>>());
        return true;
    }
    if (py::isinstance<GuideHierarchy>(src)) {
        out = HierarchyPtr(src.cast<std::shared_ptr<GuideHierarchy>>());
        return true;
    }
    if (py::isinstance<PartLines>(src)) {
        out = PartsPtr(src.cast<std::shared_ptr<PartLines>>());
        return true;
    }
    if (py::isinstance<FlowField>(src)) {
        out = FlowPtr(src.cast<std::shared_ptr<FlowField>>());
        return true;
    }
    if (py::isinstance<FieldValue>(src)) {
        out = FieldPtr(src.cast<std::shared_ptr<FieldValue>>());
        return true;
    }
    if (py::isinstance<Ramp>(src)) {
        out = CurvePtr(std::make_shared<const Ramp>(src.cast<Ramp>()));
        return true;
    }
    return false;
}

py::object castValue(const Value& v) {
    return std::visit(
        [](const auto& x) -> py::object {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return py::none();
            } else if constexpr (std::is_same_v<T, StrandsPtr>) {
                if (!x) return py::none();
                return py::cast(std::make_shared<StrandSet>(*x));  // cheap: copy-on-write
            } else if constexpr (std::is_same_v<T, CurvePtr>) {
                if (!x) return py::none();
                return py::cast(*x, py::return_value_policy::copy);
            } else if constexpr (std::is_same_v<T, UniformPtr>) {
                if (!x) return py::none();
                return castParamValue(x->value);  // a Value port's: the number, int, bool, text or vec3
            } else {
                if (!x) return py::none();
                using E = std::remove_const_t<typename T::element_type>;
                return py::cast(std::const_pointer_cast<E>(x));
            }
        },
        v);
}

// ---- ArrayView -------------------------------------------------------------------

py::ssize_t ArrayView::count() const {
    py::ssize_t n = 1;
    for (py::ssize_t s : shape) n *= s;
    return n;
}

py::buffer_info ArrayView::info() const {
    std::vector<py::ssize_t> strides(shape.size());
    py::ssize_t stride = itemsize;
    for (std::size_t i = shape.size(); i-- > 0;) {
        strides[i] = stride;
        stride *= shape[i];
    }
    return py::buffer_info(data, itemsize, format, static_cast<py::ssize_t>(shape.size()), shape, strides, readonly);
}

namespace {

py::object viewScalar(const ArrayView& v, py::ssize_t flatIndex) {
    const char* p = static_cast<const char*>(v.data) + flatIndex * v.itemsize;
    const char code = v.format.empty() ? '\0' : v.format.back();
    switch (code) {
        case 'f': return py::float_(*reinterpret_cast<const float*>(p));
        case 'd': return py::float_(*reinterpret_cast<const double*>(p));
        case 'i': return py::int_(*reinterpret_cast<const std::int32_t*>(p));
        case 'I': return py::int_(*reinterpret_cast<const std::uint32_t*>(p));
        case 'q': return py::int_(*reinterpret_cast<const std::int64_t*>(p));
        case 'Q': return py::int_(*reinterpret_cast<const std::uint64_t*>(p));
        case 'l': return py::int_(*reinterpret_cast<const long*>(p));
        case 'L': return py::int_(*reinterpret_cast<const unsigned long*>(p));
        case 'B': return py::int_(*reinterpret_cast<const std::uint8_t*>(p));
        case 'b': return py::int_(*reinterpret_cast<const std::int8_t*>(p));
        default: break;
    }
    if (v.format == kRootFormat) return py::cast(*reinterpret_cast<const StrandRoot*>(p));
    throw py::type_error("cannot index a view with format " + v.format);
}

py::object viewItem(const ArrayView& v, py::ssize_t i) {
    if (v.shape.empty()) throw py::index_error("empty view");
    if (i < 0) i += v.shape[0];
    if (i < 0 || i >= v.shape[0]) throw py::index_error("view index out of range");
    if (v.shape.size() == 1) return viewScalar(v, i);
    // Two dimensions: a row as a tuple.
    const py::ssize_t width = v.shape[1];
    py::tuple row(width);
    for (py::ssize_t k = 0; k < width; ++k) row[k] = viewScalar(v, i * width + k);
    return row;
}

void viewSetItem(ArrayView& v, py::ssize_t i, py::handle value) {
    if (v.readonly) throw py::type_error("the view is read-only; use the mutable_* accessor");
    if (v.shape.empty()) throw py::index_error("empty view");
    if (i < 0) i += v.shape[0];
    if (i < 0 || i >= v.shape[0]) throw py::index_error("view index out of range");
    const char code = v.format.empty() ? '\0' : v.format.back();
    auto store = [&](py::ssize_t flat, py::handle x) {
        char* p = static_cast<char*>(v.data) + flat * v.itemsize;
        switch (code) {
            case 'f': *reinterpret_cast<float*>(p) = x.cast<float>(); return;
            case 'd': *reinterpret_cast<double*>(p) = x.cast<double>(); return;
            case 'i': *reinterpret_cast<std::int32_t*>(p) = x.cast<std::int32_t>(); return;
            case 'I': *reinterpret_cast<std::uint32_t*>(p) = x.cast<std::uint32_t>(); return;
            case 'q': *reinterpret_cast<std::int64_t*>(p) = x.cast<std::int64_t>(); return;
            case 'Q': *reinterpret_cast<std::uint64_t*>(p) = x.cast<std::uint64_t>(); return;
            case 'B': *reinterpret_cast<std::uint8_t*>(p) = x.cast<std::uint8_t>(); return;
            default: break;
        }
        if (v.format == kRootFormat) {
            *reinterpret_cast<StrandRoot*>(p) = x.cast<StrandRoot>();
            return;
        }
        throw py::type_error("cannot assign into a view with format " + v.format);
    };
    if (v.shape.size() == 1) {
        store(i, value);
        return;
    }
    const py::ssize_t width = v.shape[1];
    Vec3f vec;
    if (width == 3 && loadVec3(value, vec)) {
        char* p = static_cast<char*>(v.data) + i * width * v.itemsize;
        if (code == 'f') {
            float* f = reinterpret_cast<float*>(p);
            f[0] = vec.x; f[1] = vec.y; f[2] = vec.z;
            return;
        }
    }
    if (!isSequenceNotText(value) || py::len(value) != static_cast<std::size_t>(width)) {
        throw py::value_error("expected a sequence of " + std::to_string(width) + " values");
    }
    py::ssize_t k = 0;
    for (py::handle x : py::reinterpret_borrow<py::sequence>(value)) store(i * width + k++, x);
}

}  // namespace

// ---- JSON -----------------------------------------------------------------------

py::object jsonLoads(const std::string& text) { return py::module_::import("json").attr("loads")(text); }
std::string jsonDumps(py::handle obj) { return py::module_::import("json").attr("dumps")(obj).cast<std::string>(); }
std::string jsonText(py::handle obj) {
    if (obj.is_none()) return {};
    if (PyUnicode_Check(obj.ptr())) return obj.cast<std::string>();
    return jsonDumps(obj);
}

// ---- Bindings -----------------------------------------------------------------------

void bindBasic(py::module_& m) {
    // Semantic versions (semver.h): what the update checks compare.
    m.def(
        "parse_version",
        [](const std::string& text) -> py::object {
            const std::optional<SemVer> v = parseSemVer(text);
            if (!v) return py::none();
            return py::make_tuple(py::tuple(py::cast(v->numbers)), py::tuple(py::cast(v->prerelease)));
        },
        py::arg("text"),
        "A semantic version's parts ((numbers...), (pre-release identifiers...)) as strings, None when text is not one "
        "(\"1.2.0\", \"v1.3.0-beta.2\", \"1.2.0+build.5\"; trailing zero numbers dropped).");
    m.def(
        "compare_versions", [](const std::string& a, const std::string& b) { return compareVersions(a, b); }, py::arg("a"),
        py::arg("b"),
        "-1, 0 or 1 as version a is older than, the same as or newer than b (semantic versions: a pre-release before its "
        "release, beta.2 before beta.10); ValueError when either is not a version.");

    // Exceptions: each core error type has a Python class deriving from RuntimeError
    // (std::invalid_argument maps to ValueError and std::runtime_error to RuntimeError
    // through pybind11's defaults).
    py::register_exception<GraphError>(m, "GraphError", PyExc_RuntimeError);
    py::register_exception<GroomFileError>(m, "GroomFileError", PyExc_RuntimeError);
    py::register_exception<TextureError>(m, "TextureError", PyExc_RuntimeError);
    py::register_exception<UsdError>(m, "UsdError", PyExc_RuntimeError);
    py::register_exception<VrsceneError>(m, "VrsceneError", PyExc_RuntimeError);

    m.def("hash64", &hash64, py::arg("value"), "64-bit hash of an integer (the core's mixing function).");
    m.def("keep_at_density", &keepAtDensity, py::arg("strand_id"), py::arg("density"),
          "True if a strand with this ID survives at the given preview density.");

    py::class_<Vec3f>(m, "Vec3", "A 3-component float vector. Any function taking a Vec3 also accepts a 3-tuple.")
        .def(py::init<>())
        .def(py::init<float, float, float>(), py::arg("x"), py::arg("y"), py::arg("z"))
        .def(py::init([](py::sequence seq) {
                 Vec3f v;
                 if (!loadVec3(seq, v)) throw py::type_error("Vec3 needs 3 numbers");
                 return v;
             }),
             py::arg("xyz"))
        .def_readwrite("x", &Vec3f::x)
        .def_readwrite("y", &Vec3f::y)
        .def_readwrite("z", &Vec3f::z)
        .def("__len__", [](const Vec3f&) { return 3; })
        .def("__getitem__",
             [](const Vec3f& v, py::ssize_t i) {
                 if (i < 0) i += 3;
                 switch (i) {
                     case 0: return v.x;
                     case 1: return v.y;
                     case 2: return v.z;
                     default: throw py::index_error("Vec3 index out of range");
                 }
             })
        .def("__setitem__",
             [](Vec3f& v, py::ssize_t i, float value) {
                 if (i < 0) i += 3;
                 switch (i) {
                     case 0: v.x = value; return;
                     case 1: v.y = value; return;
                     case 2: v.z = value; return;
                     default: throw py::index_error("Vec3 index out of range");
                 }
             })
        .def("__iter__", [](const Vec3f& v) { return py::iter(toTuple(v)); })
        .def("__eq__",
             [](const Vec3f& a, py::handle other) {
                 Vec3f b;
                 return loadVec3(other, b) && a == b;
             })
        .def("__ne__",
             [](const Vec3f& a, py::handle other) {
                 Vec3f b;
                 return !loadVec3(other, b) || a != b;
             })
        .def("__hash__", [](const Vec3f& v) { return py::hash(toTuple(v)); })
        .def("__add__", [](const Vec3f& a, py::handle o) { Vec3f b; if (!loadVec3(o, b)) throw py::type_error("Vec3 expected"); return a + b; })
        .def("__sub__", [](const Vec3f& a, py::handle o) { Vec3f b; if (!loadVec3(o, b)) throw py::type_error("Vec3 expected"); return a - b; })
        .def("__mul__", [](const Vec3f& a, float s) { return a * s; })
        .def("__rmul__", [](const Vec3f& a, float s) { return a * s; })
        .def("__truediv__", [](const Vec3f& a, float s) { return a / s; })
        .def("__neg__", [](const Vec3f& a) { return -a; })
        .def("dot", [](const Vec3f& a, py::handle o) { Vec3f b; if (!loadVec3(o, b)) throw py::type_error("Vec3 expected"); return a.dot(b); }, py::arg("other"))
        .def("cross", [](const Vec3f& a, py::handle o) { Vec3f b; if (!loadVec3(o, b)) throw py::type_error("Vec3 expected"); return a.cross(b); }, py::arg("other"))
        .def("length", &Vec3f::length)
        .def("normalized", &Vec3f::normalized)
        .def("to_tuple", &toTuple, "The vector as an (x, y, z) tuple.")
        .def("__repr__", [](const Vec3f& v) {
            std::ostringstream s;
            s << "Vec3(" << v.x << ", " << v.y << ", " << v.z << ")";
            return s.str();
        })
        .def(py::pickle([](const Vec3f& v) { return toTuple(v); },
                        [](py::tuple t) { return Vec3f{t[0].cast<float>(), t[1].cast<float>(), t[2].cast<float>()}; }));
    py::implicitly_convertible<py::sequence, Vec3f>();

    m.def("rotate_axis_angle", &rotateAxisAngle, py::arg("v"), py::arg("axis"), py::arg("angle"),
          "v rotated by angle radians about the unit vector axis (right-handed).");
    m.def("smoothstep", &smoothstep, py::arg("e0"), py::arg("e1"), py::arg("x"), "GLSL smoothstep.");
    // The expression language's builtins (expression.cpp kFns): what the editors' completion and help list.
    m.def(
        "expression_functions",
        [] {
            py::list out;
            for (const ExpressionFunction& f : Expression::functions()) {
                py::dict d;
                d["name"] = f.name;
                d["arguments"] = f.arguments;
                d["about"] = f.about;
                d["min_args"] = f.minArgs;
                d["max_args"] = f.maxArgs;
                d["vector"] = f.vector;
                d["gpu"] = f.gpu;
                out.append(d);
            }
            return out;
        },
        "Every expression builtin written by name, in the editors' order: {name, arguments, about, min_args, max_args, vector, gpu}.");

    py::class_<StrandRoot>(m, "StrandRoot",
                           "Where a strand is rooted on the scalp: triangle index plus barycentric (u, v); "
                           "face < 0 means not rooted.")
        .def(py::init<>())
        .def(py::init([](std::int32_t face, float u, float v) { return StrandRoot{face, u, v}; }), py::arg("face"),
             py::arg("u") = 0.0f, py::arg("v") = 0.0f)
        .def_readwrite("face", &StrandRoot::face)
        .def_readwrite("u", &StrandRoot::u)
        .def_readwrite("v", &StrandRoot::v)
        .def_property_readonly("rooted", [](const StrandRoot& r) { return r.face >= 0; })
        .def("__eq__", [](const StrandRoot& a, const StrandRoot& b) { return a.face == b.face && a.u == b.u && a.v == b.v; }, py::is_operator())
        .def("__repr__", [](const StrandRoot& r) {
            std::ostringstream s;
            s << "StrandRoot(face=" << r.face << ", u=" << r.u << ", v=" << r.v << ")";
            return s.str();
        })
        .def(py::pickle([](const StrandRoot& r) { return py::make_tuple(r.face, r.u, r.v); },
                        [](py::tuple t) { return StrandRoot{t[0].cast<std::int32_t>(), t[1].cast<float>(), t[2].cast<float>()}; }));

    py::class_<Ramp>(m, "Ramp",
                     "A curve over [0, 1] as (position, value) keys: monotone cubic between keys, or straight "
                     "segments with straight=True; held constant past the ends.")
        .def(py::init<>())
        .def(py::init([](const std::vector<std::pair<float, float>>& keys, bool straight) {
                 Ramp r;
                 r.keys = keys;
                 r.straight = straight;
                 r.sort();
                 return r;
             }),
             py::arg("keys"), py::arg("straight") = false)
        .def_static("constant", &Ramp::constant, py::arg("value"), "A ramp with one key: the same value everywhere.")
        .def_static("linear", &Ramp::linear, py::arg("root"), py::arg("tip"), "A straight line from root (t=0) to tip (t=1).")
        .def_property(
            "keys", [](const Ramp& r) { return r.keys; },
            [](Ramp& r, const std::vector<std::pair<float, float>>& keys) {
                r.keys = keys;
                r.handles.clear();  // (other keys: automatic handles)
                r.sort();
            },
            "(position, value) pairs, sorted by position (set: every key's handles automatic).")
        .def_property_readonly(
            "handles",
            [](const Ramp& r) {
                py::list out;
                for (std::size_t k = 0; k < r.keys.size(); ++k) {
                    const RampHandle h = r.handle(k);
                    if (h.set) {
                        out.append(py::make_tuple(h.in, h.out));
                    } else {
                        out.append(py::none());
                    }
                }
                return out;
            },
            "Each key's handles: (slope in, slope out), or None where automatic (issue #102).")
        .def(
            "set_handle",
            [](Ramp& r, std::size_t k, float in, py::object out) {
                if (k >= r.keys.size()) throw py::index_error("no such key");
                r.setHandle(k, RampHandle{true, in, out.is_none() ? in : out.cast<float>()});
            },
            py::arg("key"), py::arg("slope_in"), py::arg("slope_out") = py::none(),
            "Key `key`'s handles: its slope into it and out of it (the same both sides without slope_out).")
        .def(
            "clear_handle",
            [](Ramp& r, std::size_t k) {
                if (k >= r.keys.size()) throw py::index_error("no such key");
                r.setHandle(k, RampHandle{});
            },
            py::arg("key"), "Key `key`'s handles automatic again.")
        .def("tangent_in", &Ramp::tangentIn, py::arg("key"), "The slope into key `key` (its handle's, else automatic).")
        .def("tangent_out", &Ramp::tangentOut, py::arg("key"), "The slope out of key `key` (its handle's, else automatic).")
        .def_static(
            "from_json", [](const std::string& text) { return rampFromJson(text); }, py::arg("text"),
            "A ramp param's value as a graph's JSON holds it (keys, optional handles, linear).")
        .def("to_json", [](const Ramp& r) { return rampToJson(r); }, "The ramp as a graph's JSON holds it.")
        .def_readwrite("straight", &Ramp::straight)
        .def("sort", &Ramp::sort)
        .def("__call__", &Ramp::operator(), py::arg("t"), "The value at t.")
        .def("__eq__", [](const Ramp& a, const Ramp& b) { return a == b; }, py::is_operator())
        .def("__repr__", [](const Ramp& r) {
            std::ostringstream s;
            s << "Ramp([";
            for (std::size_t i = 0; i < r.keys.size(); ++i) {
                if (i) s << ", ";
                s << "(" << r.keys[i].first << ", " << r.keys[i].second << ")";
            }
            s << "]" << (r.straight ? ", straight=True" : "") << ")";
            return s.str();
        })
        .def(py::pickle([](const Ramp& r) { return py::make_tuple(rampToJson(r)); },
                        [](py::tuple t) { return rampFromJson(t[0].cast<std::string>()); }));

    py::enum_<MapRef::Source>(m, "MapSource", "Where a MapRef reads its value.")
        .value("Texture", MapRef::Source::Texture)
        .value("ColorSet", MapRef::Source::ColorSet)
        .value("Attribute", MapRef::Source::Attribute)
        .value("Expression", MapRef::Source::Expression)
        .value("Guide", MapRef::Source::Guide);
    py::enum_<MapRef::Component>(m, "MapComponent", "The component of a map a MapRef picks.")
        .value("R", MapRef::Component::R)
        .value("G", MapRef::Component::G)
        .value("B", MapRef::Component::B)
        .value("A", MapRef::Component::A)
        .value("Luminance", MapRef::Component::Luminance);

    py::class_<MapRef>(m, "MapRef",
                       "A parameter read from a map instead of a constant: a texture (file path), a scalp color "
                       "set, a strand attribute, an expression or a guide attribute layer, remapped to [min, max].")
        .def(py::init<>())
        .def(py::init([](MapRef::Source source, const std::string& name, MapRef::Component component, float min,
                         float max) {
                 MapRef r;
                 r.source = source;
                 r.name = name;
                 r.component = component;
                 r.min = min;
                 r.max = max;
                 return r;
             }),
             py::arg("source"), py::arg("name"), py::arg("component") = MapRef::Component::R, py::arg("min") = 0.0f,
             py::arg("max") = 1.0f)
        .def_static("texture", [](const std::string& path, MapRef::Component c, float lo, float hi) {
                MapRef r; r.source = MapRef::Source::Texture; r.name = path; r.component = c; r.min = lo; r.max = hi; return r; },
            py::arg("path"), py::arg("component") = MapRef::Component::R, py::arg("min") = 0.0f, py::arg("max") = 1.0f)
        .def_static("color_set", [](const std::string& n, MapRef::Component c, float lo, float hi) {
                MapRef r; r.source = MapRef::Source::ColorSet; r.name = n; r.component = c; r.min = lo; r.max = hi; return r; },
            py::arg("name"), py::arg("component") = MapRef::Component::R, py::arg("min") = 0.0f, py::arg("max") = 1.0f)
        .def_static("attribute", [](const std::string& n, float lo, float hi) {
                MapRef r; r.source = MapRef::Source::Attribute; r.name = n; r.min = lo; r.max = hi; return r; },
            py::arg("name"), py::arg("min") = 0.0f, py::arg("max") = 1.0f)
        .def_static("expression", [](const std::string& text, float lo, float hi) {
                MapRef r; r.source = MapRef::Source::Expression; r.name = text; r.min = lo; r.max = hi; return r; },
            py::arg("text"), py::arg("min") = 0.0f, py::arg("max") = 1.0f)
        .def_static("guide", [](const std::string& n, float lo, float hi) {
                MapRef r; r.source = MapRef::Source::Guide; r.name = n; r.min = lo; r.max = hi; return r; },
            py::arg("name"), py::arg("min") = 0.0f, py::arg("max") = 1.0f)
        .def_readwrite("source", &MapRef::source)
        .def_readwrite("name", &MapRef::name)
        .def_readwrite("component", &MapRef::component)
        .def_readwrite("min", &MapRef::min)
        .def_readwrite("max", &MapRef::max)
        .def_property_readonly("source_key", [](const MapRef& r) { return MapRef::sourceKey(r.source); },
                               "The JSON key of the source: texture, colorSet, attribute, expression or guide.")
        .def_property_readonly("component_name", [](const MapRef& r) { return MapRef::componentName(r.component); })
        .def("__eq__", [](const MapRef& a, const MapRef& b) { return a == b; }, py::is_operator())
        .def("__repr__", [](const MapRef& r) {
            std::ostringstream s;
            s << "MapRef(" << MapRef::sourceKey(r.source) << "='" << r.name << "', component='"
              << MapRef::componentName(r.component) << "', min=" << r.min << ", max=" << r.max << ")";
            return s.str();
        });

    py::class_<Frame>(m, "Frame", "An orthonormal frame at a curve point: tangent, normal, binormal.")
        .def(py::init<>())
        .def_readwrite("tangent", &Frame::tangent)
        .def_readwrite("normal", &Frame::normal)
        .def_readwrite("binormal", &Frame::binormal)
        .def("to_world", &Frame::toWorld, py::arg("local"))
        .def("to_local", &Frame::toLocal, py::arg("world"));
    m.def("rotation_minimizing_frames",
          [](const std::vector<Vec3f>& points, const Vec3f& initialNormal) {
              return rotationMinimizingFrames(points.data(), points.size(), initialNormal);
          },
          py::arg("points"), py::arg("initial_normal") = Vec3f{1, 0, 0},
          "Rotation-minimizing frames along a polyline (double reflection).");
    m.def("any_perpendicular", &anyPerpendicular, py::arg("v"));
    m.def("rotate_between", &rotateBetween, py::arg("v"), py::arg("from_dir"), py::arg("to_dir"));

    py::class_<ArrayView>(m, "ArrayView", py::buffer_protocol(),
                          "A view of an array inside a core object (positions, widths, IDs...). Supports the "
                          "buffer protocol: memoryview(view) or numpy.asarray(view) share the memory. Views are "
                          "invalidated when the owner resizes the array; the owner is kept alive by the view.")
        .def_buffer([](ArrayView& v) { return v.info(); })
        .def_property_readonly("shape", [](const ArrayView& v) { return py::tuple(py::cast(v.shape)); })
        .def_property_readonly("format", [](const ArrayView& v) { return v.format; }, "PEP 3118 format string.")
        .def_property_readonly("itemsize", [](const ArrayView& v) { return v.itemsize; })
        .def_property_readonly("readonly", [](const ArrayView& v) { return v.readonly; })
        .def_property_readonly("owner", [](const ArrayView& v) { return v.owner; })
        .def("__len__", [](const ArrayView& v) { return v.shape.empty() ? 0 : v.shape[0]; })
        .def("__getitem__", &viewItem, py::arg("index"))
        .def("__setitem__", &viewSetItem, py::arg("index"), py::arg("value"))
        .def("tolist",
             [](const ArrayView& v) {
                 const py::ssize_t n = v.shape.empty() ? 0 : v.shape[0];
                 py::list out(n);
                 for (py::ssize_t i = 0; i < n; ++i) out[i] = viewItem(v, i);
                 return out;
             },
             "The contents as a list (rows as tuples for 2-D views).")
        .def("__repr__", [](const ArrayView& v) {
            std::ostringstream s;
            s << "<ArrayView format=" << v.format << " shape=(";
            for (std::size_t i = 0; i < v.shape.size(); ++i) s << (i ? ", " : "") << v.shape[i];
            s << ")" << (v.readonly ? " readonly" : " writable") << ">";
            return s.str();
        });
}

}  // namespace groomlab::py_bind
