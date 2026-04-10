#include <nanobind/nanobind.h>

int add(int a, int b) {return a + b;}

NB_MODULE(cobra_mba, m) {
  m.def("add", &add);
}
