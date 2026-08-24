#include <ours/panic.hpp>
#include <boost/assert/source_location.hpp>

auto ox_panic(char const *fn_name) -> void { ours::panic("Boost.Container: {}", fn_name); }

namespace boost {
namespace container { 
   void throw_bad_alloc() { ox_panic(__func__); }

   void throw_out_of_range(const char* str) { ox_panic(__func__); }

   void throw_length_error(const char* str) { ox_panic(__func__); }

   void throw_logic_error(const char* str) { ox_panic(__func__); }

   void throw_runtime_error(const char* str) { ox_panic(__func__); }

   void throw_runtime_error(std::exception const & e) { ox_panic(__func__); }
} // namespace container
   void throw_exception( std::exception const & e ) { ox_panic(__func__); }

   void throw_exception( std::exception const & e, boost::source_location const & loc ) // user defined
   { ox_panic(__func__); } 

   void assertion_failed(char const * expr, char const * function, char const * file, long line) // user defined
   { ox_panic(__func__); } 

   void assertion_failed_msg(char const * expr, char const * msg, char const * function, char const * file, long line) // user defined
   { ox_panic(__func__); } 
} // namespace boost