/*
 * ossfs - FUSE-based file system backed by Alibaba Cloud OSS
 *
 * Copyright(C) 2014 Andrew Gaul <andrew@gaul.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <cstdlib>
#include <limits>
#include <stdint.h>
#include <strings.h>
#include <string>
#include <map>

#include "common.h"
#include "s3fs.h"
#include "string_util.h"
#include "test_util.h"

//-------------------------------------------------------------------
// Global variables for test_string_util
//-------------------------------------------------------------------
bool foreground                   = false;
std::string instance_name;

void test_trim()
{
    ASSERT_EQUALS(std::string("1234"), trim("  1234  "));
    ASSERT_EQUALS(std::string("1234"), trim("1234  "));
    ASSERT_EQUALS(std::string("1234"), trim("  1234"));
    ASSERT_EQUALS(std::string("1234"), trim("1234"));

    ASSERT_EQUALS(std::string("1234  "), trim_left("  1234  "));
    ASSERT_EQUALS(std::string("1234  "), trim_left("1234  "));
    ASSERT_EQUALS(std::string("1234"), trim_left("  1234"));
    ASSERT_EQUALS(std::string("1234"), trim_left("1234"));

    ASSERT_EQUALS(std::string("  1234"), trim_right("  1234  "));
    ASSERT_EQUALS(std::string("1234"), trim_right("1234  "));
    ASSERT_EQUALS(std::string("  1234"), trim_right("  1234"));
    ASSERT_EQUALS(std::string("1234"), trim_right("1234"));

    ASSERT_EQUALS(std::string("0"), str(0));
    ASSERT_EQUALS(std::string("1"), str(1));
    ASSERT_EQUALS(std::string("-1"), str(-1));
    ASSERT_EQUALS(std::string("9223372036854775807"), str(std::numeric_limits<int64_t>::max()));
    ASSERT_EQUALS(std::string("-9223372036854775808"), str(std::numeric_limits<int64_t>::min()));
    ASSERT_EQUALS(std::string("0"), str(std::numeric_limits<uint64_t>::min()));
    ASSERT_EQUALS(std::string("18446744073709551615"), str(std::numeric_limits<uint64_t>::max()));
}

void test_base64()
{
    unsigned char *buf;
    size_t len;

    ASSERT_STREQUALS(s3fs_base64(NULL, 0), NULL);
    buf = s3fs_decode64(NULL, 0, &len);
    ASSERT_BUFEQUALS(reinterpret_cast<const char *>(buf), len, NULL, 0);

    ASSERT_STREQUALS(s3fs_base64(reinterpret_cast<const unsigned char *>(""), 0), NULL);
    buf = s3fs_decode64("", 0, &len);
    ASSERT_BUFEQUALS(reinterpret_cast<const char *>(buf), len, NULL, 0);

    ASSERT_STREQUALS(s3fs_base64(reinterpret_cast<const unsigned char *>("1"), 1), "MQ==");
    buf = s3fs_decode64("MQ==", 4, &len);
    ASSERT_BUFEQUALS(reinterpret_cast<const char *>(buf), len, "1", 1);
    ASSERT_EQUALS(len, static_cast<size_t>(1));

    ASSERT_STREQUALS(s3fs_base64(reinterpret_cast<const unsigned char *>("12"), 2), "MTI=");
    buf = s3fs_decode64("MTI=", 4, &len);
    ASSERT_BUFEQUALS(reinterpret_cast<const char *>(buf), len, "12", 2);
    ASSERT_EQUALS(len, static_cast<size_t>(2));

    ASSERT_STREQUALS(s3fs_base64(reinterpret_cast<const unsigned char *>("123"), 3), "MTIz");
    buf = s3fs_decode64("MTIz", 4, &len);
    ASSERT_BUFEQUALS(reinterpret_cast<const char *>(buf), len, "123", 3);
    ASSERT_EQUALS(len, static_cast<size_t>(3));

    ASSERT_STREQUALS(s3fs_base64(reinterpret_cast<const unsigned char *>("1234"), 4), "MTIzNA==");
    buf = s3fs_decode64("MTIzNA==", 8, &len);
    ASSERT_BUFEQUALS(reinterpret_cast<const char *>(buf), len, "1234", 4);
    ASSERT_EQUALS(len, static_cast<size_t>(4));

    // TODO: invalid input
}

void test_strtoofft()
{
    off_t value;

    ASSERT_TRUE(s3fs_strtoofft(&value, "0"));
    ASSERT_EQUALS(value, static_cast<off_t>(0L));

    ASSERT_TRUE(s3fs_strtoofft(&value, "9"));
    ASSERT_EQUALS(value, static_cast<off_t>(9L));

    ASSERT_FALSE(s3fs_strtoofft(&value, "A"));

    ASSERT_TRUE(s3fs_strtoofft(&value, "A", /*base=*/ 16));
    ASSERT_EQUALS(value, static_cast<off_t>(10L));

    ASSERT_TRUE(s3fs_strtoofft(&value, "F", /*base=*/ 16));
    ASSERT_EQUALS(value, static_cast<off_t>(15L));

    ASSERT_TRUE(s3fs_strtoofft(&value, "a", /*base=*/ 16));
    ASSERT_EQUALS(value, static_cast<off_t>(10L));

    ASSERT_TRUE(s3fs_strtoofft(&value, "f", /*base=*/ 16));
    ASSERT_EQUALS(value, static_cast<off_t>(15L));

    ASSERT_TRUE(s3fs_strtoofft(&value, "deadbeef", /*base=*/ 16));
    ASSERT_EQUALS(value, static_cast<off_t>(3735928559L));
}

void test_wtf8_encoding()
{
    std::string ascii("normal std::string");
    std::string utf8("Hyld\xc3\xbdpi \xc3\xbej\xc3\xb3\xc3\xb0""f\xc3\xa9lagsins vex \xc3\xbar k\xc3\xa6rkomnu b\xc3\xb6li \xc3\xad \xc3\xa1st");
    std::string cp1252("Hyld\xfdpi \xfej\xf3\xf0""f\xe9lagsins vex \xfar k\xe6rkomnu b\xf6li \xed \xe1st");
    std::string broken = utf8;
    broken[14] = 0x97;
    std::string mixed = ascii + utf8 + cp1252;

    ASSERT_EQUALS(s3fs_wtf8_encode(ascii), ascii);
    ASSERT_EQUALS(s3fs_wtf8_decode(ascii), ascii);
    ASSERT_EQUALS(s3fs_wtf8_encode(utf8), utf8);
    ASSERT_EQUALS(s3fs_wtf8_decode(utf8), utf8);

    ASSERT_NEQUALS(s3fs_wtf8_encode(cp1252), cp1252);
    ASSERT_EQUALS(s3fs_wtf8_decode(s3fs_wtf8_encode(cp1252)), cp1252);

    ASSERT_NEQUALS(s3fs_wtf8_encode(broken), broken);
    ASSERT_EQUALS(s3fs_wtf8_decode(s3fs_wtf8_encode(broken)), broken);

    ASSERT_NEQUALS(s3fs_wtf8_encode(mixed), mixed);
    ASSERT_EQUALS(s3fs_wtf8_decode(s3fs_wtf8_encode(mixed)), mixed);
}

void test_get_keyword_value() {
    std::string value;

    // Normal case: Keyword exists and has a value
    ASSERT_TRUE(get_keyword_value("http://example.com?keyword=value&other=param", "keyword", value));
    ASSERT_STREQUALS(value.c_str(), "value");

    // Multiple parameters: Keyword exists among other parameters
    ASSERT_TRUE(get_keyword_value("http://example.com?keyword=value&other=param&another=param2", "other", value));
    ASSERT_STREQUALS(value.c_str(), "param");

    // Keyword at the end: Keyword is the last parameter in the URL
    ASSERT_TRUE(get_keyword_value("http://example.com?other=param&another=param2&keyword=value", "keyword", value));
    ASSERT_STREQUALS(value.c_str(), "value");

    // Single parameter: URL contains only one parameter
    ASSERT_TRUE(get_keyword_value("http://example.com?keyword=value", "keyword", value));
    ASSERT_STREQUALS(value.c_str(), "value");

    // Keyword does not exist: URL does not contain the specified keyword
    ASSERT_FALSE(get_keyword_value("http://example.com?other=param", "keyword", value));

    // No equal sign after keyword: Keyword is not followed by an equal sign
    ASSERT_FALSE(get_keyword_value("http://example.com?keywordvalue&other=param", "keyword", value));

    // Empty keyword: Keyword is an empty string
    ASSERT_FALSE(get_keyword_value("http://example.com?=value&other=param", "", value));

    // Null keyword: Keyword is NULL
    ASSERT_FALSE(get_keyword_value("http://example.com?keyword=value&other=param", nullptr, value));

    // Empty URL: URL is an empty string
    ASSERT_FALSE(get_keyword_value("", "keyword", value));

    // Null URL: URL is NULL (empty string in this case)
    ASSERT_FALSE(get_keyword_value("", nullptr, value));

    // No parameters: URL does not contain any parameters
    ASSERT_FALSE(get_keyword_value("http://example.com", "keyword", value));

    // No equal sign: Keyword is not followed by an equal sign
    ASSERT_FALSE(get_keyword_value("http://example.com?keyword", "keyword", value));

    // No value: Keyword is followed by an equal sign but no value
    ASSERT_TRUE(get_keyword_value("http://example.com?keyword=&other=param", "keyword", value));
    ASSERT_STREQUALS(value.c_str(), "");

    // Keyword at the beginning: Keyword is the first parameter in the URL
    ASSERT_TRUE(get_keyword_value("keyword=value&other=param", "keyword", value));
    ASSERT_STREQUALS(value.c_str(), "value");

    // Keyword in the middle: Keyword is in the middle of the URL
    ASSERT_TRUE(get_keyword_value("other=param&keyword=value&another=param2", "keyword", value));
    ASSERT_STREQUALS(value.c_str(), "value");

    // Keyword at the end: Keyword is the last parameter in the URL
    ASSERT_TRUE(get_keyword_value("other=param&another=param2&keyword=value", "keyword", value));
    ASSERT_STREQUALS(value.c_str(), "value");

    // Multiple same keywords: URL contains multiple occurrences of the same keyword
    ASSERT_TRUE(get_keyword_value("http://example.com?keyword=value1&keyword=value2&other=param", "keyword", value));
    ASSERT_STREQUALS(value.c_str(), "value1");
}

void test_get_unixtime_from_iso8601() {
    time_t unixtime;

    // Normal case: Valid ISO8601 date string
    ASSERT_TRUE(get_unixtime_from_iso8601("2023-10-11T12:34:56", unixtime));
    ASSERT_EQUALS(unixtime, 1697027696); // Expected Unix timestamp for 2023-10-11T12:34:56

    // Different valid ISO8601 date string
    ASSERT_TRUE(get_unixtime_from_iso8601("2024-11-12T01:02:03", unixtime));
    ASSERT_EQUALS(unixtime, 1731373323); // Expected Unix timestamp for 2024-11-12T01:02:03

    // Date string with zero time
    ASSERT_TRUE(get_unixtime_from_iso8601("2023-10-11T00:00:00", unixtime));
    ASSERT_EQUALS(unixtime, 1696982400); // Expected Unix timestamp for 2023-10-11T00:00:00
}

void test_convert_unixtime_from_option_arg() {
    time_t unixtime;

    // Normal case: Valid input with all units
    ASSERT_TRUE(convert_unixtime_from_option_arg("1Y2M3D4h5m6s", unixtime));
    ASSERT_EQUALS(unixtime, 36993906);

    // Only years
    ASSERT_TRUE(convert_unixtime_from_option_arg("1Y", unixtime));
    ASSERT_EQUALS(unixtime, 31536000);

    // Mixed units
    ASSERT_TRUE(convert_unixtime_from_option_arg("1Y2D4h", unixtime));
    ASSERT_EQUALS(unixtime, 31723200);
}

int main(int argc, char *argv[])
{
    S3fsLog singletonLog;

    test_trim();
    test_base64();
    test_strtoofft();
    test_wtf8_encoding();
    test_get_keyword_value();
    test_get_unixtime_from_iso8601();
    test_convert_unixtime_from_option_arg();
    return 0;
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
