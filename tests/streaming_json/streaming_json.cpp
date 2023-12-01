// Glaze Library
// For the license information refer to glaze.hpp

#include "boost/ut.hpp"
#include "glaze/glaze.hpp"

#include <iostream>
#include <sstream>

struct dual_stream_buf : public std::streambuf {
    dual_stream_buf(std::streambuf* sb1, std::streambuf* sb2)
        : sb1(sb1), sb2(sb2) {}

    int overflow(int c) override {
        if (c != EOF) {
            if (sb1) {
                std::ostream os1(sb1);
                os1.put(static_cast<char>(c));
            }

            if (sb2) {
                std::ostream os2(sb2);
                os2.put(static_cast<char>(c));
            }
        }
        return c;
    }

    std::streambuf* sb1;  // Stream buffer for the first stream (e.g., std::cout)
    std::streambuf* sb2;  // Stream buffer for the second stream (e.g., std::ostringstream)
};

struct dual_ostream : public std::ostream {
    dual_ostream(std::ostream& os1)
        : std::ostream(&dual_stream_buf_), dual_stream_buf_(os1.rdbuf(), oss.rdbuf()) {}

    // Accessor function to retrieve the output string
    std::string str() const {
        return oss.str();
    }

    std::ostringstream oss;  // Stream buffer for storing the output
    dual_stream_buf dual_stream_buf_;
};

int main() {
    // Create a dual_ostream that redirects output to both cout and an ostringstream
    dual_ostream dual_ostream(std::cout);

    // Use the dual_ostream as you would use std::cout
    dual_ostream << "Hello, world!" << std::endl;

    // Retrieve the output as a string from the dual_ostream
    std::string output_string = dual_ostream.str();

    // Display the output
    std::cout << "Output from std::cout:\n" << output_string;

    return 0;
}


int main() {
    // Create an ostringstream to store the output
    std::ostringstream oss;

    // Create a dual_ostream that redirects output to both cout and the ostringstream
    dual_ostream dual_ostream(std::cout, oss);

    // Use the dual_ostream as you would use std::cout
    dual_ostream << "Hello, world!" << std::endl;

    // Retrieve the output as a string from the ostringstream
    std::string output_string = oss.str();

    // Display the output
    std::cout << "Output from std::cout:\n" << output_string;

    return 0;
}


using namespace boost::ut;

struct my_struct
{
   int i = 287;
   double d = 3.14;
   std::string hello = "Hello World";
   std::array<uint64_t, 3> arr = {1, 2, 3};
};

template <>
struct glz::meta<my_struct>
{
   static constexpr std::string_view name = "my_struct";
   using T = my_struct;
   static constexpr auto value = object(&T::i, &T::d, &T::hello, &T::arr);
};

suite starter = [] {
   "example"_test = [] {
      my_struct s{};
      glz::write_json(s, std::cout);
      expect(buffer == R"({"i":287,"d":3.14,"hello":"Hello World","arr":[1,2,3]})");
      expect(glz::prettify(buffer) == R"({
   "i": 287,
   "d": 3.14,
   "hello": "Hello World",
   "arr": [
      1,
      2,
      3
   ]
})");
   };
};

int main()
{
   // Explicitly run registered test suites and report errors
   // This prevents potential issues with thread local variables
   const auto result = boost::ut::cfg<>.run({.report_errors = true});
   return result;
}
