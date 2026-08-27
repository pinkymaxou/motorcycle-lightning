/* The configuration's round trip: defaults -> validate -> protobuf -> back.
 * This is what stands between a firmware update and the rider having to
 * re-enter everything, so it gets tested off the board. */
#include <cstdio>
#include <cstring>

#include "config_store.h"

namespace
{

int g_fail;

#define CHECK(cond) do { \
    if (!(cond)) \
    { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

constexpr size_t BUF_BYTES = 4096;

void testDefaultsAreValid()
{
    SysConfig cfg;
    ConfigStore::defaults(&cfg);
    CHECK(ConfigStore::validate(&cfg));
    CHECK(CFG_DEFAULT_SECTION_COUNT == cfg.strips[0].n_sections);
    CHECK(0 == cfg.strips[1].n_sections);          /* second strip not installed */
    CHECK(stripTotalLeds(cfg.strips[0]) > 0);
}

void testRoundTripKeepsEverything()
{
    SysConfig in;
    ConfigStore::defaults(&in);
    /* something in every corner of the message */
    in.strips[1].n_sections = 1;
    in.strips[1].sections[0].led_count = 42;
    in.strips[1].sections[0].reversed = true;
    in.strips[1].sections[0].turn = TurnSource::Right;
    std::strcpy(in.strips[1].sections[0].fx_idle, "f_white");
    in.strips[1].led_model = LedModel::SK6812;
    in.strips[1].color_order = ColorOrder::RGBW;
    in.strips[1].reversed = true;
    std::strcpy(in.fx_hazard_on, "f_turn_on");
    in.blink_exit_x10 = 15;
    in.brake_holdoff_s = 30;
    std::strcpy(in.ap_ssid, "MyMoto");
    std::strcpy(in.ap_pass, "longenoughpass");
    std::strcpy(in.sta_ssid, "some-network");
    std::strcpy(in.sta_pass, "hunter2");
    in.sta_active = true;
    in.palette.colors[0] = Fx::RgbaColor{ 1, 2, 3, 4 };

    uint8_t buf[BUF_BYTES];
    const size_t len = ConfigStore::encode(in, buf, sizeof(buf),
                                           ConfigStore::Secrets::Include);
    CHECK(0 != len);

    SysConfig out;
    CHECK(ConfigStore::decode(buf, len, &out));
    CHECK(ConfigStore::validate(&out));

    CHECK(in.strips[0].n_sections == out.strips[0].n_sections);
    CHECK(42 == out.strips[1].sections[0].led_count);
    CHECK(out.strips[1].sections[0].reversed);
    CHECK(TurnSource::Right == out.strips[1].sections[0].turn);
    CHECK(0 == std::strcmp("f_white", out.strips[1].sections[0].fx_idle));
    CHECK(LedModel::SK6812 == out.strips[1].led_model);
    CHECK(ColorOrder::RGBW == out.strips[1].color_order);
    CHECK(out.strips[1].reversed);
    CHECK(0 == std::strcmp("f_turn_on", out.fx_hazard_on));
    CHECK(0 == out.fx_hazard_off[0]);
    CHECK(15 == out.blink_exit_x10);
    CHECK(30 == out.brake_holdoff_s);
    CHECK(0 == std::strcmp("some-network", out.sta_ssid));
    CHECK(0 == std::strcmp("MyMoto", out.ap_ssid));
    CHECK(0 == std::strcmp("longenoughpass", out.ap_pass));
    CHECK(out.sta_active);
    CHECK(1 == out.palette.colors[0].r && 4 == out.palette.colors[0].a);

    /* the password travels to flash but never onto the wire */
    CHECK(0 == std::strcmp("hunter2", out.sta_pass));
    const size_t wire = ConfigStore::encode(in, buf, sizeof(buf),
                                            ConfigStore::Secrets::Omit);
    SysConfig seen;
    CHECK(ConfigStore::decode(buf, wire, &seen));
    CHECK('\0' == seen.sta_pass[0]);
    CHECK('\0' == seen.ap_pass[0]);
    CHECK(0 == std::strcmp("MyMoto", seen.ap_ssid));
}

/* A config written by a build that knew more fields than this one must still
 * load — that is the whole reason the blob is protobuf and not a struct. */
void testUnknownFieldsAreIgnored()
{
    SysConfig in;
    ConfigStore::defaults(&in);
    uint8_t buf[BUF_BYTES];
    size_t len = ConfigStore::encode(in, buf, sizeof(buf),
                                     ConfigStore::Secrets::Include);
    CHECK(0 != len);

    /* field 15, varint, value 7 — nothing in this build's schema (a tag
       above 15 would need two bytes, which is not what is being tested) */
    buf[len++] = (15 << 3) | 0;
    buf[len++] = 7;

    SysConfig out;
    CHECK(ConfigStore::decode(buf, len, &out));
    CHECK(ConfigStore::validate(&out));
    CHECK(in.strips[0].n_sections == out.strips[0].n_sections);
}

void testGarbageIsRejected()
{
    SysConfig in;
    ConfigStore::defaults(&in);
    uint8_t buf[BUF_BYTES];
    const size_t len = ConfigStore::encode(in, buf, sizeof(buf),
                                           ConfigStore::Secrets::Include);
    SysConfig out;
    /* truncated in the middle of a length-delimited field */
    CHECK(!ConfigStore::decode(buf, len / 2, &out));

    /* and validate() still guards the ranges the schema cannot */
    SysConfig bad;
    ConfigStore::defaults(&bad);
    bad.strips[0].n_sections = CFG_MAX_SECTIONS + 1;
    CHECK(!ConfigStore::validate(&bad));
    ConfigStore::defaults(&bad);
    bad.blink_exit_x10 = 99;
    CHECK(!ConfigStore::validate(&bad));
    /* a named access point with a key WPA2 would refuse: the AP would never
       start and the page would be unreachable */
    ConfigStore::defaults(&bad);
    std::strcpy(bad.ap_ssid, "MyMoto");
    std::strcpy(bad.ap_pass, "short");
    CHECK(!ConfigStore::validate(&bad));
    std::strcpy(bad.ap_pass, "longenough");
    CHECK(ConfigStore::validate(&bad));

    ConfigStore::defaults(&bad);
    std::memset(bad.strips[0].sections[0].fx_idle, 'x',
                sizeof(bad.strips[0].sections[0].fx_idle));
    CHECK(!ConfigStore::validate(&bad));
}

} // namespace

int main()
{
    testDefaultsAreValid();
    testRoundTripKeepsEverything();
    testUnknownFieldsAreIgnored();
    testGarbageIsRejected();

    if (0 != g_fail)
    {
        std::printf("config tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("config tests: all passed\n");
    return 0;
}
