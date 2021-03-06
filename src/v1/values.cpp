// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>

#include <algorithm>
#include <initializer_list>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>

#include <mesos/v1/resources.hpp>
#include <mesos/v1/values.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/strings.hpp>

using std::max;
using std::min;
using std::ostream;
using std::string;
using std::vector;

namespace mesos {
namespace v1 {

ostream& operator<<(ostream& stream, const Value::Scalar& scalar)
{
  return stream << scalar.value();
}


bool operator==(const Value::Scalar& left, const Value::Scalar& right)
{
  return left.value() == right.value();
}


bool operator<=(const Value::Scalar& left, const Value::Scalar& right)
{
  return left.value() <= right.value();
}


Value::Scalar operator+(const Value::Scalar& left, const Value::Scalar& right)
{
  Value::Scalar result;
  result.set_value(left.value() + right.value());
  return result;
}


Value::Scalar operator-(const Value::Scalar& left, const Value::Scalar& right)
{
  Value::Scalar result;
  result.set_value(left.value() - right.value());
  return result;
}


Value::Scalar& operator+=(Value::Scalar& left, const Value::Scalar& right)
{
  left.set_value(left.value() + right.value());
  return left;
}


Value::Scalar& operator-=(Value::Scalar& left, const Value::Scalar& right)
{
  left.set_value(left.value() - right.value());
  return left;
}


namespace internal {

struct Range
{
  uint64_t start;
  uint64_t end;
};


// Coalesces the vector of ranges provided and modifies `result` to contain the
// solution.
// The algorithm first sorts all the individual intervals so that we can iterate
// over them sequentially.
// The algorithm does a single pass, after the sort, and builds up the solution
// in place. It then modifies the `result` with as few steps as possible. The
// expensive part of this operation is modification of the protobuf, which is
// why we prefer to build up the solution in a temporary vector.
void coalesce(Value::Ranges* result, vector<Range> ranges)
{
  // Exit early if empty.
  if (ranges.empty()) {
    result->clear_range();
    return;
  }

  std::sort(
      ranges.begin(),
      ranges.end(),
      [](const Range& left, const Range& right) {
        return std::tie(left.start, left.end) <
               std::tie(right.start, right.end);
      });

  // We build up initial state of the current range.
  CHECK(!ranges.empty());
  int count = 1;
  Range current = ranges.front();

  // In a single pass, we compute the size of the end result, as well as modify
  // in place the intermediate data structure to build up result as we
  // solve it.
  foreach (const Range& range, ranges) {
    // Skip if this range is equivalent to the current range.
    if (range.start == current.start && range.end == current.end) {
      continue;
    }

    // If the current range just needs to be extended on the right.
    if (range.start == current.start && range.end > current.end) {
      current.end = range.end;
    } else if (range.start > current.start) {
      // If we are starting farther ahead, then there are 2 cases:
      if (range.start <= current.end + 1) {
        // 1. Ranges are overlapping and we can merge them.
        current.end = max(current.end, range.end);
      } else {
        // 2. No overlap and we are adding a new range.
        ranges[count - 1] = current;
        ++count;
        current = range;
      }
    }
  }

  // Record the state of the last range into of ranges vector.
  ranges[count - 1] = current;

  CHECK(count <= static_cast<int>(ranges.size()));

  // Shrink result if it is too large by deleting trailing subrange.
  if (count < result->range_size()) {
    result->mutable_range()->DeleteSubrange(
        count, result->range_size() - count);
  }

  // Resize enough space so we allocate the pointer array just once.
  result->mutable_range()->Reserve(count);

  // Copy the solution from ranges vector into result.
  for (int i = 0; i < count; ++i) {
    // result might be small and needs to be extended.
    if (i >= result->range_size()) {
      result->add_range();
    }

    CHECK(i < result->range_size());
    result->mutable_range(i)->set_begin(ranges[i].start);
    result->mutable_range(i)->set_end(ranges[i].end);
  }

  CHECK_EQ(result->range_size(), count);
}

} // namespace internal {


// Coalesce the given addedRanges  into 'result' ranges.
void coalesce(
    Value::Ranges* result,
    std::initializer_list<Value::Ranges> addedRanges)
{
  size_t rangesSum = result->range_size();
  foreach (const Value::Ranges& range, addedRanges) {
    rangesSum += range.range_size();
  }

  vector<internal::Range> ranges;
  ranges.reserve(rangesSum);

  // Merges ranges into a vector.
  auto fill = [&ranges](const Value::Ranges& inputs) {
    foreach (const Value::Range& range, inputs.range()) {
      ranges.push_back({range.begin(), range.end()});
    }
  };

  // Merge both ranges into the vector;
  fill(*result);
  foreach (const Value::Ranges& range, addedRanges) {
    fill(range);
  }

  internal::coalesce(result, std::move(ranges));
}


// Coalesce the given Value::Ranges 'ranges'.
void coalesce(Value::Ranges* result)
{
  coalesce(result, {Value::Ranges()});
}


// Coalesce the given range 'addedRange' into 'result' ranges.
void coalesce(Value::Ranges* result, const Value::Range& addedRange)
{
  Value::Ranges ranges;
  Value::Range* range = ranges.add_range();
  range->CopyFrom(addedRange);
  coalesce(result, {ranges});
}


// Removes a range from already coalesced ranges.
// The algorithms constructs a new vector of ranges which is then
// coalesced into a Value::Ranges instance.
static void remove(Value::Ranges* _ranges, const Value::Range& removal)
{
  vector<internal::Range> ranges;
  ranges.reserve(_ranges->range_size());

  foreach (const Value::Range& range, _ranges->range()) {
    // Skip if the entire range is subsumed by `removal`.
    if (range.begin() >= removal.begin() && range.end() <= removal.end()) {
      continue;
    }

    // Divide if the range subsumes the `removal`.
    if (range.begin() < removal.begin() && range.end() > removal.end()) {
      // Front.
      ranges.emplace_back(internal::Range{range.begin(), removal.begin() - 1});
      // Back.
      ranges.emplace_back(internal::Range{removal.end() + 1, range.end()});
    }

    // Fully Emplace if the range doesn't intersect.
    if (range.end() < removal.begin() || range.begin() > removal.end()) {
      ranges.emplace_back(internal::Range{range.begin(), range.end()});
    } else {
      // Trim if the range does intersect.
      if (range.end() > removal.end()) {
        // Trim front.
        ranges.emplace_back(internal::Range{removal.end() + 1, range.end()});
      } else {
        // Trim back.
        CHECK(range.begin() < removal.begin());
        ranges.emplace_back(
            internal::Range{range.begin(), removal.begin() - 1});
      }
    }
  }

  internal::coalesce(_ranges, std::move(ranges));
}


ostream& operator<<(ostream& stream, const Value::Ranges& ranges)
{
  stream << "[";
  for (int i = 0; i < ranges.range_size(); i++) {
    stream << ranges.range(i).begin() << "-" << ranges.range(i).end();
    if (i + 1 < ranges.range_size()) {
      stream << ", ";
    }
  }
  stream << "]";
  return stream;
}


bool operator==(const Value::Ranges& _left, const Value::Ranges& _right)
{
  Value::Ranges left;
  coalesce(&left, {_left});

  Value::Ranges right;
  coalesce(&right, {_right});

  if (left.range_size() == right.range_size()) {
    for (int i = 0; i < left.range_size(); i++) {
      // Make sure this range is equal to a range in the right.
      bool found = false;
      for (int j = 0; j < right.range_size(); j++) {
        if (left.range(i).begin() == right.range(j).begin() &&
            left.range(i).end() == right.range(j).end()) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


bool operator<=(const Value::Ranges& _left, const Value::Ranges& _right)
{
  Value::Ranges left;
  coalesce(&left, {_left});

  Value::Ranges right;
  coalesce(&right, {_right});

  for (int i = 0; i < left.range_size(); i++) {
    // Make sure this range is a subset of a range in right.
    bool matched = false;
    for (int j = 0; j < right.range_size(); j++) {
      if (left.range(i).begin() >= right.range(j).begin() &&
          left.range(i).end() <= right.range(j).end()) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }

  return true;
}


Value::Ranges operator+(const Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges result;
  coalesce(&result, {left, right});
  return result;
}


Value::Ranges operator-(const Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges result;
  coalesce(&result, {left});
  return result -= right;
}


Value::Ranges& operator+=(Value::Ranges& left, const Value::Ranges& right)
{
  coalesce(&left, {right});
  return left;
}


Value::Ranges& operator-=(Value::Ranges& left, const Value::Ranges& right)
{
  coalesce(&left);
  for (int i = 0; i < right.range_size(); ++i) {
    remove(&left, right.range(i));
  }
  return left;
}


ostream& operator<<(ostream& stream, const Value::Set& set)
{
  stream << "{";
  for (int i = 0; i < set.item_size(); i++) {
    stream << set.item(i);
    if (i + 1 < set.item_size()) {
      stream << ", ";
    }
  }
  stream << "}";
  return stream;
}


bool operator==(const Value::Set& left, const Value::Set& right)
{
  if (left.item_size() == right.item_size()) {
    for (int i = 0; i < left.item_size(); i++) {
      // Make sure this item is equal to an item in the right.
      bool found = false;
      for (int j = 0; j < right.item_size(); j++) {
        if (left.item(i) == right.item(i)) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


bool operator<=(const Value::Set& left, const Value::Set& right)
{
  if (left.item_size() <= right.item_size()) {
    for (int i = 0; i < left.item_size(); i++) {
      // Make sure this item is equal to an item in the right.
      bool found = false;
      for (int j = 0; j < right.item_size(); j++) {
        if (left.item(i) == right.item(j)) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


Value::Set operator+(const Value::Set& left, const Value::Set& right)
{
  Value::Set result;

  for (int i = 0; i < left.item_size(); i++) {
    result.add_item(left.item(i));
  }

  // A little bit of extra logic to avoid adding duplicates from right.
  for (int i = 0; i < right.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < result.item_size(); j++) {
      if (right.item(i) == result.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      result.add_item(right.item(i));
    }
  }

  return result;
}


Value::Set operator-(const Value::Set& left, const Value::Set& right)
{
  Value::Set result;

  // Look for the same item in right as we add left to result.
  for (int i = 0; i < left.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < right.item_size(); j++) {
      if (left.item(i) == right.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      result.add_item(left.item(i));
    }
  }

  return result;
}


Value::Set& operator+=(Value::Set& left, const Value::Set& right)
{
  // A little bit of extra logic to avoid adding duplicates from right.
  for (int i = 0; i < right.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < left.item_size(); j++) {
      if (right.item(i) == left.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      left.add_item(right.item(i));
    }
  }

  return left;
}


Value::Set& operator-=(Value::Set& left, const Value::Set& right)
{
  // For each item in right, remove it if it's in left.
  for (int i = 0; i < right.item_size(); i++) {
    for (int j = 0; j < left.item_size(); j++) {
      if (right.item(i) == left.item(j)) {
        left.mutable_item()->DeleteSubrange(j, 1);
        break;
      }
    }
  }

  return left;
}


ostream& operator<<(ostream& stream, const Value::Text& value)
{
  return stream << value.value();
}


bool operator==(const Value::Text& left, const Value::Text& right)
{
  return left.value() == right.value();
}


namespace internal {
namespace values {

Try<Value> parse(const string& text)
{
  Value value;

  // Remove any spaces from the text.
  string temp;
  foreach (const char c, text) {
    if (c != ' ') {
      temp += c;
    }
  }

  if (temp.length() == 0) {
    return Error("Expecting non-empty string");
  }

  // TODO(ynie): Find a better way to check brackets.
  if (!strings::checkBracketsMatching(temp, '{', '}') ||
      !strings::checkBracketsMatching(temp, '[', ']') ||
      !strings::checkBracketsMatching(temp, '(', ')')) {
    return Error("Mismatched brackets");
  }

  size_t index = temp.find('[');
  if (index == 0) {
    // This is a Value::Ranges.
    value.set_type(Value::RANGES);
    Value::Ranges* ranges = value.mutable_ranges();
    const vector<string>& tokens = strings::tokenize(temp, "[]-,\n");
    if (tokens.size() % 2 != 0) {
      return Error("Expecting one or more \"ranges\"");
    } else {
      for (size_t i = 0; i < tokens.size(); i += 2) {
        Value::Range* range = ranges->add_range();

        int j = i;
        try {
          range->set_begin(boost::lexical_cast<uint64_t>((tokens[j++])));
          range->set_end(boost::lexical_cast<uint64_t>(tokens[j++]));
        } catch (const boost::bad_lexical_cast&) {
          return Error(
              "Expecting non-negative integers in '" + tokens[j - 1] + "'");
        }
      }

      coalesce(ranges);

      return value;
    }
  } else if (index == string::npos) {
    size_t index = temp.find('{');
    if (index == 0) {
      // This is a set.
      value.set_type(Value::SET);
      Value::Set* set = value.mutable_set();
      const vector<string>& tokens = strings::tokenize(temp, "{},\n");
      for (size_t i = 0; i < tokens.size(); i++) {
        set->add_item(tokens[i]);
      }
      return value;
    } else if (index == string::npos) {
      try {
        // This is a scalar.
        value.set_type(Value::SCALAR);
        Value::Scalar* scalar = value.mutable_scalar();
        scalar->set_value(boost::lexical_cast<double>(temp));
        return value;
      } catch (const boost::bad_lexical_cast&) {
        // This is a text.
        value.set_type(Value::TEXT);
        Value::Text* text = value.mutable_text();
        text->set_value(temp);
        return value;
      }
    } else {
      return Error("Unexpected '{' found");
    }
  }

  return Error("Unexpected '[' found");
}

} // namespace values {
} // namespace internal {

} // namespace v1 {
} // namespace mesos {
