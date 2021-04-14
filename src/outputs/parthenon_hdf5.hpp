// (C) (or copyright) 2020-2021. Triad National Security, LLC. All rights reserved.
//
// This program was produced under U.S. Government contract 89233218CNA000001 for Los
// Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
// for the U.S. Department of Energy/National Nuclear Security Administration. All rights
// in the program are reserved by Triad National Security, LLC, and the U.S. Department
// of Energy/National Nuclear Security Administration. The Government is granted for
// itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
// license in this material to reproduce, prepare derivative works, distribute copies to
// the public, perform publicly and display publicly, and to permit others to do so.
//========================================================================================
#include "H5Tpublic.h"
#include "H5public.h"
#ifndef HDF5OUTPUT
#error "parthenon_hdf5.hpp requires HDF5 output to be enabled"
#endif // ifndef HDF5OUTPUT

#ifndef OUTPUTS_PARTHENON_HDF5_HPP_
#define OUTPUTS_PARTHENON_HDF5_HPP_
// Definitions common to parthenon restart and parthenon output for HDF5

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <hdf5.h>

#include "basic_types.hpp"
#include "coordinates/coordinates.hpp"
#include "defs.hpp"
#include "globals.hpp"
#include "interface/meshblock_data_iterator.hpp"
#include "mesh/mesh.hpp"
#include "outputs/outputs.hpp"
#include "parameter_input.hpp"
#include "parthenon_arrays.hpp"
#include "utils/error_checking.hpp"

#include "parthenon_mpi.hpp"

using parthenon::Real;

namespace parthenon {
namespace HDF5 {

/**
 * @brief RAII handles for HDF5. Use the typedefs directly (e.g. `H5A`, `H5D`, etc.)
 *
 * @tparam CloseFn - function pointer to destructor for HDF5 object
 */
template <herr_t (*CloseFn)(hid_t)>
class H5Handle {
 public:
  H5Handle() = default;

  H5Handle(H5Handle const &) = delete;
  H5Handle &operator=(H5Handle const &) = delete;

  H5Handle(H5Handle &&other) : hid_(other.Release()) {}
  H5Handle &operator=(H5Handle &&other) {
    Reset();
    hid_ = other.Release();
    return *this;
  }

  static H5Handle FromHIDCheck(hid_t const hid) {
    PARTHENON_REQUIRE_THROWS(hid >= 0, "H5 FromHIDCheck failed");

    H5Handle handle;
    handle.hid_ = hid;
    return handle;
  }

  void Reset() {
    if (*this) {
      PARTHENON_HDF5_CHECK(CloseFn(hid_));
      hid_ = -1;
    }
  }

  hid_t Release() {
    auto hid = hid_;
    hid_ = -1;
    return hid;
  }

  ~H5Handle() { Reset(); }

  // Implicit conversion to hid_t for convenience
  operator hid_t() const { return hid_; }
  explicit operator bool() const { return hid_ >= 0; }

 private:
  hid_t hid_ = -1;
};

using H5A = H5Handle<&H5Aclose>;
using H5D = H5Handle<&H5Dclose>;
using H5F = H5Handle<&H5Fclose>;
using H5G = H5Handle<&H5Gclose>;
using H5P = H5Handle<&H5Pclose>;
using H5T = H5Handle<&H5Tclose>;
using H5S = H5Handle<&H5Sclose>;

// Static functions to return HDF type
static hid_t getHDF5Type(const hbool_t *) { return H5T_NATIVE_HBOOL; }
static hid_t getHDF5Type(const int32_t *) { return H5T_NATIVE_INT32; }
static hid_t getHDF5Type(const int64_t *) { return H5T_NATIVE_INT64; }
static hid_t getHDF5Type(const float *) { return H5T_NATIVE_FLOAT; }
static hid_t getHDF5Type(const double *) { return H5T_NATIVE_DOUBLE; }
static H5T getHDF5Type(const char *const *) {
  H5T var_string_type = H5T::FromHIDCheck(H5Tcopy(H5T_C_S1));
  PARTHENON_HDF5_CHECK(H5Tset_size(var_string_type, H5T_VARIABLE));
  return var_string_type;
}

inline H5G MakeGroup(hid_t file, const std::string &name) {
  return H5G::FromHIDCheck(
      H5Gcreate(file, name.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
}

template <typename T>
void HDF5WriteND(hid_t location, const std::string &name, const T *data, int rank,
                 const hsize_t *local_offset, const hsize_t *local_count,
                 const hsize_t *global_count, hid_t plist_xfer, hid_t plist_dcreate) {
  const H5S local_space = H5S::FromHIDCheck(H5Screate_simple(rank, local_count, NULL));
  const H5S global_space = H5S::FromHIDCheck(H5Screate_simple(rank, global_count, NULL));

  auto type = getHDF5Type(data);
  const H5D gDSet =
      H5D::FromHIDCheck(H5Dcreate(location, name.c_str(), type, global_space, H5P_DEFAULT,
                                  plist_dcreate, H5P_DEFAULT));
  PARTHENON_HDF5_CHECK(H5Sselect_hyperslab(global_space, H5S_SELECT_SET, local_offset,
                                           NULL, local_count, NULL));
  PARTHENON_HDF5_CHECK(
      H5Dwrite(gDSet, type, local_space, global_space, plist_xfer, data));
}

template <typename T>
void HDF5Write2D(hid_t location, const std::string &name, const T *data,
                 const hsize_t *local_offset, const hsize_t *local_count,
                 const hsize_t *global_count, const H5P &plist_xfer) {

  HDF5WriteND(location, name, data, 2, local_offset, local_count, global_count,
              plist_xfer, H5P_DEFAULT);
}

template <typename T>
void WriteHDF5Attribute(const std::string &name, const std::vector<T> &values,
                        hid_t location) {
  if (values.size() <= 0) return;

  const hsize_t dim[1] = {values.size()};
  const H5S data_space = H5S::FromHIDCheck(*dim == 1 ? H5Screate(H5S_SCALAR)
                                                     : H5Screate_simple(1, dim, dim));

  const T *data = values.data();
  auto type = getHDF5Type(data);

  H5A const attribute = H5A::FromHIDCheck(
      H5Acreate(location, name.c_str(), type, data_space, H5P_DEFAULT, H5P_DEFAULT));
  PARTHENON_HDF5_CHECK(H5Awrite(attribute, type, data));
}

template <typename T>
void WriteHDF5Attribute(const std::string &name, T value, hid_t location) {
  std::vector<T> vec(1);
  vec[0] = value;
  WriteHDF5Attribute(name, vec, location);
}

} // namespace HDF5
} // namespace parthenon

#endif // OUTPUTS_PARTHENON_HDF5_HPP_
