#define BOOST_TEST_NO_MAIN
#define BOOST_TEST_ALTERNATIVE_INIT_API
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

bool init_function() { return true; }

int main( int argc, char* argv[] )
{
    return ::boost::unit_test::unit_test_main( &init_function, argc, argv );
}