#include <boost/locale.hpp>

using namespace std;

extern "C" int utf8_boost(const unsigned char *data, int len)
{
    try {
        boost::locale::conv::utf_to_utf<char>(data, data+len,
                boost::locale::conv::stop);
    } catch (const boost::locale::conv::conversion_error& ex) {
        return 0;
    }

    return 1;
}
