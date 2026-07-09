#pragma once

// Test fixture for header-aware extraction: a class declared in a header with a
// method defined out-of-line in orbit.cpp. Deliberately free of system includes
// so it parses cleanly without an SDK.
namespace Demo {

class Orbit {
 public:
  int Steps() const;
  double radius = 1.0;
};

}  // namespace Demo
