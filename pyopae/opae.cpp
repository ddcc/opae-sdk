#include <Python.h>

#include <algorithm>
#include <sstream>
#include <vector>
#include <fstream>
#include <chrono>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include <opae/cxx/core/token.h>
#include <opae/cxx/core/properties.h>
#include <opae/cxx/core/handle.h>
#include <opae/cxx/core/dma_buffer.h>
#include <opae/cxx/core/except.h>
#include <opae/manage.h>


namespace py = pybind11;

using std::chrono::high_resolution_clock;
using std::chrono::microseconds;

using opae::fpga::types::token;
using opae::fpga::types::properties;
using opae::fpga::types::handle;
using opae::fpga::types::dma_buffer;
using opae::fpga::types::except;

enum class fpga_status : uint8_t {
  closed = 0,
  open
};

static void reconfigure(handle::ptr_t h, int slot, const char *filename, int flags){
  std::ifstream gbsfile(filename, std::ifstream::binary);
  if (gbsfile.is_open()){
    gbsfile.seekg(0, gbsfile.end);
    auto size = gbsfile.tellg();
    std::vector<char> buff(size);
    gbsfile.seekg(0, gbsfile.beg);
    gbsfile.read(buff.data(), size);
    auto result = fpgaReconfigureSlot(*h, slot, reinterpret_cast<uint8_t*>(buff.data()), size, flags);
    if (result != FPGA_OK){
      gbsfile.close();
      throw except(result, OPAECXX_HERE);
    }
    gbsfile.close();
  } else{
    throw except(FPGA_EXCEPTION, OPAECXX_HERE);
  }
}

static bool buffer_poll(dma_buffer::ptr_t self, uint64_t offset, uint64_t value,
                        uint64_t mask, uint64_t poll_usec, uint64_t timeout_usec){
  auto timeout = high_resolution_clock::now() + microseconds(timeout_usec);
  while((self->read<uint64_t>(offset) & mask) != value){
    std::this_thread::sleep_for(microseconds(poll_usec));
    if (high_resolution_clock::now() > timeout){
      return false;
    }
  }
  return true;
}

PYBIND11_MODULE(opae, m) {
  m.doc() = "Open Programmable Acceleration Engine - Python bindings"; // optional module docstring

  m.def("reconfigure", reconfigure);
  py::class_<properties, properties::ptr_t> pyproperties(m, "properties");
  pyproperties
    .def(py::init())
    .def_static("read",
        [](token::ptr_t t){
          return properties::read(t);
        })
    .def_property("parent",
      [](const properties &p) -> token::ptr_t {
        auto token_struct = p.parent;
        auto parent_props = properties::read(token_struct);
        auto tokens = token::enumerate({*parent_props});
        return tokens[0];
      },
      [](properties & p, token::ptr_t t){
        p.parent = *t;
      }
    )
    .def_property("guid",
      [](const properties &p) -> std::string {
        std::cout << "guid: " << p.guid << "\n";
        std::stringstream ss;
        ss << p.guid;
        return ss.str();
      },
      [](properties & p, const std::string& guid_str){
        p.guid.parse(guid_str.c_str());
      }
    )
    .def_property("type",
       [](properties &p) -> fpga_objtype {
         return p.type;
       },
       [](properties & p, fpga_objtype t){
         p.type = t;
       }
    )
    .def_property("bus",
       [](properties &p) -> uint8_t {
         return p.bus;
       },
       [](properties & p, uint8_t b){
         p.bus = b;
       }
      );

  py::class_<token, token::ptr_t> pytoken(m, "token");
  pytoken
      .def_static("enumerate", &token::enumerate);


  py::class_<handle, handle::ptr_t> pyhandle(m, "handle");
  pyhandle
      .def_static("open",
          [](token::ptr_t t, int flags) -> handle::ptr_t{
            return handle::open(t, flags);
          })
      .def("reset", &handle::reset)
      .def("read_csr32", &handle::read_csr32,   py::arg("offset"), py::arg("csr_space") = 0)
      .def("read_csr64", &handle::read_csr64,   py::arg("offset"), py::arg("csr_space") = 0)
      .def("write_csr32", &handle::write_csr32, py::arg("offset"), py::arg("value"), py::arg("csr_space") = 0)
      .def("write_csr64", &handle::write_csr64, py::arg("offset"), py::arg("value"), py::arg("csr_space") = 0)
      .def("close", &handle::close)
      ;

  py::class_<dma_buffer, dma_buffer::ptr_t> pybuffer(m, "dma_buffer");
  pybuffer
    .def_static("allocate", &dma_buffer::allocate)
    .def("size", &dma_buffer::size)
    .def("wsid", &dma_buffer::wsid)
    .def("iova", &dma_buffer::iova)
    .def("fill", &dma_buffer::fill)
    .def("poll", buffer_poll, py::arg("offset"), py::arg("value"),
                              py::arg("mask") = ~0, py::arg("poll_usec") = 100, py::arg("timeout_usec") = 1000)
    .def("compare", &dma_buffer::compare)
    .def("read32",
        [](dma_buffer::ptr_t buff, size_t offset){
          return buff->read<uint32_t>(offset);
          })
    .def("read64",
        [](dma_buffer::ptr_t buff, size_t offset){
          return buff->read<uint64_t>(offset);
          })
    .def("buffer",
        [](dma_buffer::ptr_t b){
          uint8_t* c_buffer = const_cast<uint8_t*>(b->get());
          return py::memoryview(py::buffer_info(c_buffer, b->size()));
        })
  ;


  py::enum_<fpga_result>(m, "fpga_result", py::arithmetic(), "OPAE return codes")
    .value("OK", FPGA_OK)
    .value("INVALID_PARAM", FPGA_INVALID_PARAM)
    .value("BUSY", FPGA_BUSY)
    .value("EXCEPTION", FPGA_EXCEPTION)
    .value("NOT_FOUND", FPGA_NOT_FOUND)
    .value("NO_MEMORY", FPGA_NO_MEMORY)
    .value("NOT_SUPPORTED", FPGA_NOT_SUPPORTED)
    .value("NO_DRIVER", FPGA_NO_DRIVER)
    .value("NO_DAEMON", FPGA_NO_DAEMON)
    .value("NO_ACCESS", FPGA_NO_ACCESS)
    .value("RECONF_ERROR", FPGA_RECONF_ERROR)
    .export_values();

  py::enum_<fpga_objtype>(m, "fpga_objtype", py::arithmetic(), "OPAE resource objects")
    .value("DEVICE", FPGA_DEVICE)
    .value("ACCELERATOR", FPGA_ACCELERATOR)
    .export_values();

  py::enum_<fpga_open_flags>(m, "fpga_open_flags", py::arithmetic(), "OPAE flags for opening resources")
    .value("OPEN_SHARED", FPGA_OPEN_SHARED)
    .export_values();

  py::enum_<fpga_status>(m, "fpga_status", py::arithmetic(), "OPAE resource status")
    .value("CLOSED", fpga_status::closed)
    .value("OPEN", fpga_status::open)
    .export_values();

}