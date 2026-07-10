#include "metrics.hpp"

namespace Demo {

// Decision points: `if`, `&&`, `for`, and two `case` labels. `switch` itself is
// not one, nor is `default`. Cyclomatic complexity is therefore 1 + 5 = 6.
int Circle::Classify(int steps) const
{
  if (steps > 0 && steps < 10) {
    return 1;
  }
  for (int index = 0; index < steps; ++index) {
    switch (index) {
      case 0:
        return 0;
      case 1:
        return 1;
      default:
        break;
    }
  }
  return steps;
}

}  // namespace Demo
