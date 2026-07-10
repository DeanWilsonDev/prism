#pragma once

// Test fixture for metric accuracy. Deliberately free of system includes so it
// parses cleanly without an SDK.
namespace Demo {

enum ShapeKind { Round, Square };

template <typename T>
struct Box {
 public:
  T value;
};

struct Point {
 public:
  int x = 0;
  int y = 0;
};

struct Shape {
 public:
  // "ShapeKind" contains "Shape" as a substring. Resolving a field's type by
  // searching the type spelling for known class names reports this as a
  // reference to Shape, giving Shape a self-loop and a false circular
  // dependency. It is an enum, and composes nothing.
  ShapeKind kind = Round;

  Point origin;           // directly
  Point corners[4];       // through an array
  Box<Point> boxed;       // through a template argument
  Shape* next = nullptr;  // a self reference is not composition
};

struct Circle : Shape {
 public:
  int Classify(int steps) const;
};

}  // namespace Demo
