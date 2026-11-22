#include "cata_catch.h"

#if defined(MP_SERVER)

#include <string>
#include <vector>

#include "color.h"
#include "output.h"

// Phase 4b-1: the wire and the glyph path must resolve inline colour tags with
// one implementation. split_by_color() cuts at each tag, so a tag segment also
// carries the text that follows it; these tests pin the extracted runs,
// including the trailing-tag empty run that carries the final colour.

TEST_CASE( "mp_log_split_colored_text", "[mp]" )
{
    SECTION( "plain text is one run in the initial colour" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "hello", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 1 );
        CHECK( runs[0].text == "hello" );
        CHECK( runs[0].color == c_green );
    }

    SECTION( "an empty string produces no runs" ) {
        CHECK( split_colored_text( "", c_green, c_white, report_color_error::no ).empty() );
    }

    SECTION( "an inline colour colours the text after its tag" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "a<color_red>b</color>c", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 3 );
        CHECK( runs[0].text == "a" );
        CHECK( runs[0].color == c_green );
        CHECK( runs[1].text == "b" );
        CHECK( runs[1].color == c_red );
        CHECK( runs[2].text == "c" );
        CHECK( runs[2].color == c_green );
    }

    SECTION( "an unmatched close tag falls back to the base colour" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "a</color>b", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 2 );
        CHECK( runs[0].text == "a" );
        CHECK( runs[0].color == c_green );
        CHECK( runs[1].text == "b" );
        CHECK( runs[1].color == c_white );
    }

    SECTION( "a trailing tag keeps an empty run with the final colour" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "a<color_red>", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 2 );
        CHECK( runs[0].text == "a" );
        CHECK( runs[0].color == c_green );
        CHECK( runs[1].text.empty() );
        CHECK( runs[1].color == c_red );
    }

    SECTION( "a non-tag angle bracket stays literal text" ) {
        // split_by_color only cuts at <color_*> and </color>; any other tag
        // syntax is plain text and must survive verbatim.
        const std::vector<colored_text_run> runs = split_colored_text(
                    "a<no_such_tag>b", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 1 );
        CHECK( runs[0].text == "a<no_such_tag>b" );
        CHECK( runs[0].color == c_green );
    }

    SECTION( "an invalid colour name stays literal text" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "<color_not_a_colour>x", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 1 );
        CHECK( runs[0].text == "<color_not_a_colour>x" );
        CHECK( runs[0].color == c_green );
    }
}

#endif // MP_SERVER
