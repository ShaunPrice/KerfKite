// The core's JSON reader and writer, and the number conversions under them, checked without Python.
//
// The expected numbers in the tables were computed by Python (float() and repr()); the larger comparisons
// with Python and with the JSON library this code replaced are described in docs/NATIVE.md. Beyond the
// tables, every short cut in kerfkite/decimal.hpp is checked here against the exact arithmetic it stands
// for, on whatever platform this runs.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "kerfkite/decimal.hpp"
#include "kerfkite/json.hpp"
#include "kerfkite/number_text.hpp"

using json = kerfkite::Json;
using namespace kerfkite;

static int failures = 0;

static void expect(bool condition, const std::string &what) {
    if (!condition) {
        if (failures < 40) std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static uint64_t bits_of(double value) {
    uint64_t pattern;
    std::memcpy(&pattern, &value, sizeof pattern);
    return pattern;
}

static double from_bits(uint64_t pattern) {
    double value;
    std::memcpy(&value, &pattern, sizeof value);
    return value;
}

// The message a text is refused with, or an empty string when it is read.
static std::string refusal(const std::string &text) {
    try {
        json::parse(text);
    } catch (const json::parse_error &error) {
        return error.what();
    } catch (const std::exception &) {
        return "(another exception)";
    }
    return "";
}

// "u", "i" or "f" and the text the value is written as: the kind of number a text is read as.
static std::string kind(const std::string &text) {
    const json value = json::parse(text);
    return std::string(value.is_number_unsigned() ? "u" : value.is_number_integer() ? "i" : value.is_number_float() ? "f" : "?") +
           value.dump();
}

template <typename Thrown, typename Action> static bool throws(Action action) {
    try {
        action();
    } catch (const Thrown &) {
        return true;
    } catch (const std::exception &) {
        return false;
    }
    return false;
}

// A fixed sequence of pseudo-random 64-bit patterns (xorshift64*), the same on every platform.
struct Random {
    uint64_t state;
    uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1DULL;
    }
};

struct Decimal {
    const char *text;
    uint64_t pattern;
};

// Decimal text and the double Python's float() gives for it: ties, numbers either side of a tie by one
// digit hundreds of places along, the smallest and largest doubles, and exponents far past any double.
static const Decimal DECIMALS[] = {
    {"0", 0x0000000000000000ULL},
    {"-0", 0x8000000000000000ULL},
    {"0.0", 0x0000000000000000ULL},
    {"-0.0", 0x8000000000000000ULL},
    {"1", 0x3ff0000000000000ULL},
    {"0.1", 0x3fb999999999999aULL},
    {"0.5", 0x3fe0000000000000ULL},
    {"1e0", 0x3ff0000000000000ULL},
    {"1E+2", 0x4059000000000000ULL},
    {"1e-2", 0x3f847ae147ae147bULL},
    {"123.456", 0x405edd2f1a9fbe77ULL},
    {"-2.5e-3", 0xbf647ae147ae147bULL},
    {"1e22", 0x4480f0cf064dd592ULL},
    {"1e23", 0x44b52d02c7e14af6ULL},
    {"8.5e22", 0x44b1ffdbf6b2b2ebULL},
    {"9007199254740993", 0x4340000000000000ULL},
    {"9007199254740992.5", 0x4340000000000000ULL},
    {"9007199254740993.0000000000000000000000000000000000000001", 0x4340000000000001ULL},
    {"0.30000000000000004", 0x3fd3333333333334ULL},
    {"0.1000000000000000055511151231257827021181583404541015625", 0x3fb999999999999aULL},
    {"2.2250738585072011e-308", 0x000fffffffffffffULL},
    {"2.2250738585072012e-308", 0x0010000000000000ULL},
    {"2.2250738585072014e-308", 0x0010000000000000ULL},
    {"4.9e-324", 0x0000000000000001ULL},
    {"5e-324", 0x0000000000000001ULL},
    {"2.4703282292062327e-324", 0x0000000000000000ULL},
    {"2.4703282292062328e-324", 0x0000000000000001ULL},
    {"2.47e-324", 0x0000000000000000ULL},
    {"1e-323", 0x0000000000000002ULL},
    {"1e-400", 0x0000000000000000ULL},
    {"1.7976931348623157e308", 0x7fefffffffffffffULL},
    {"1.7976931348623158e308", 0x7fefffffffffffffULL},
    {"1.7976931348623159e308", 0x7ff0000000000000ULL},
    {"1e309", 0x7ff0000000000000ULL},
    {"-1e309", 0xfff0000000000000ULL},
    {"1e99999999999999999999", 0x7ff0000000000000ULL},
    {"1e-99999999999999999999", 0x0000000000000000ULL},
    {"0e99999999999999999999", 0x0000000000000000ULL},
    {"6.9294956446009195e15", 0x43389e56ee5e7a58ULL},
    {"3.7455744005952583e15", 0x432a9d28ff412a75ULL},
    {"1.0000000000000002", 0x3ff0000000000001ULL},
    {"1.00000000000000011102230246251565404236316680908203125", 0x3ff0000000000000ULL},
    {"1.00000000000000011102230246251565404236316680908203124", 0x3ff0000000000000ULL},
    {"1.00000000000000011102230246251565404236316680908203126", 0x3ff0000000000001ULL},
    {"9214843084008499", 0x43405e6cec57761aULL},
    {"0.500000000000000166533453693773481063544750213623046875", 0x3fe0000000000002ULL},
    {"3.518437208883201171875e13", 0x42c0000000000002ULL},
    {"62.5364939768271845828", 0x404f44abd5aa7ca4ULL},
    {"8.10109172351e-10", 0x3e0bd5cbaef0fd0cULL},
    {"1.50000000000000011102230246251565404236316680908203125", 0x3ff8000000000000ULL},
    {"9007199254740991.4999999999999999999999999999999995", 0x433fffffffffffffULL},
    {"30078505129381147446200", 0x44997a3c7271b021ULL},
    {"1777820000000000000001", 0x4458180d5bad2e3eULL},
    {"0.500000000000000166547006220929549868969843373633921146392822265625", 0x3fe0000000000002ULL},
    {"0.50000000000000016656055874808561867439493653364479541778564453125", 0x3fe0000000000002ULL},
    {"0.3932922657273", 0x3fd92bb352c4623aULL},
    {"4.8929891601781557495462e3", 0x40b31cfd3999f7afULL},
    {"7.3177701707893310e15", 0x4339ff792393edd3ULL},
    {"1.014120480182583464902367222169599999e30", 0x4629999999999999ULL},
    {"1.014120480182583464902367222169600001e30", 0x4629999999999999ULL},
    {
        "7.41098468761869816264853189302332058547589703921487146638378523751013260905313127797949754542453988"
        "5696948470431685765963899850655339096945981621940161728171894510697854671067917687257517734731555330"
        "7795408549809608457500958111373034747658096871009590975442271004757307809711118935784838675653998783"
        "5030152280559340465937397917907387238682993958184816601691220194564999312897984113620624844986787135"
        "7218035220901702390328579173252022052897402080290685402160661237554998340267130003581248647904138574"
        "3401875520901590172592547146296175134159774938718574737870961645638908718119841271673056017045493004"
        "7052695901657637768849082679869725733665217655679410725087643375608460039849049721491174630855395563"
        "54188641513168478436313080237596295773983001708984375e-324", 0x0000000000000002ULL},
    {
        "7.41098468761869816264853189302332058547589703921487146638378523751013260905313127797949754542453988"
        "5696948470431685765963899850655339096945981621940161728171894510697854671067917687257517734731555330"
        "7795408549809608457500958111373034747658096871009590975442271004757307809711118935784838675653998783"
        "5030152280559340465937397917907387238682993958184816601691220194564999312897984113620624844986787135"
        "7218035220901702390328579173252022052897402080290685402160661237554998340267130003581248647904138574"
        "3401875520901590172592547146296175134159774938718574737870961645638908718119841271673056017045493004"
        "7052695901657637768849082679869725733665217655679410725087643375608460039849049721491174630855395563"
        "541886415131684784363130802375962957739830017089843750001e-324", 0x0000000000000002ULL},
    {
        "7.41098468761869816264853189302332058547589703921487146638378523751013260905313127797949754542453988"
        "5696948470431685765963899850655339096945981621940161728171894510697854671067917687257517734731555330"
        "7795408549809608457500958111373034747658096871009590975442271004757307809711118935784838675653998783"
        "5030152280559340465937397917907387238682993958184816601691220194564999312897984113620624844986787135"
        "7218035220901702390328579173252022052897402080290685402160661237554998340267130003581248647904138574"
        "3401875520901590172592547146296175134159774938718574737870961645638908718119841271673056017045493004"
        "7052695901657637768849082679869725733665217655679410725087643375608460039849049721491174630855395563"
        "5418864151316847843631308023759629577398300170898437e-324", 0x0000000000000001ULL},
    {
        "2.22507385850720163012305563795567615250361241457301801308322872404958664760675944619203679411688695"
        "3213985520549032000903434781884412325572184367563347617020518175998922941393629966742598285899994830"
        "1489714335555785676932793060159781831621424250679624607852958851992724935776883207324924799248168692"
        "3224716596493432925878395010225097395757951057160073834364573849432419299709217920738991976169431413"
        "1497173265255020084997973676783743155205818804439163810572367791175177756227497413804253387084478193"
        "6555330738674208345261625130294620227301090548200676540202015471120020281397001415752591234401773622"
        "4427371246815175018974555997865323425588621961151633592416795802960447706494647018477736093430045142"
        "168360701364747951396213837722826145437693412532098591327667236328125e-308", 0x0010000000000000ULL},
    {
        "2.22507385850720163012305563795567615250361241457301801308322872404958664760675944619203679411688695"
        "3213985520549032000903434781884412325572184367563347617020518175998922941393629966742598285899994830"
        "1489714335555785676932793060159781831621424250679624607852958851992724935776883207324924799248168692"
        "3224716596493432925878395010225097395757951057160073834364573849432419299709217920738991976169431413"
        "1497173265255020084997973676783743155205818804439163810572367791175177756227497413804253387084478193"
        "6555330738674208345261625130294620227301090548200676540202015471120020281397001415752591234401773622"
        "4427371246815175018974555997865323425588621961151633592416795802960447706494647018477736093430045142"
        "1683607013647479513962138377228261454376934125320985913276672363281250001e-308", 0x0010000000000001ULL},
    {
        "2.22507385850720163012305563795567615250361241457301801308322872404958664760675944619203679411688695"
        "3213985520549032000903434781884412325572184367563347617020518175998922941393629966742598285899994830"
        "1489714335555785676932793060159781831621424250679624607852958851992724935776883207324924799248168692"
        "3224716596493432925878395010225097395757951057160073834364573849432419299709217920738991976169431413"
        "1497173265255020084997973676783743155205818804439163810572367791175177756227497413804253387084478193"
        "6555330738674208345261625130294620227301090548200676540202015471120020281397001415752591234401773622"
        "4427371246815175018974555997865323425588621961151633592416795802960447706494647018477736093430045142"
        "16836070136474795139621383772282614543769341253209859132766723632812e-308", 0x0010000000000000ULL},
    {
        "1.79769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207"
        "0963302864166928879109465555478519404026306574886715058206819089020007083836762738548458177115317644"
        "7573027006985557136695962284291481986083493647529271907416844436551070434271155969950809304288017790"
        "4174497792e+308", 0x7ff0000000000000ULL},
    {
        "1.79769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207"
        "0963302864166928879109465555478519404026306574886715058206819089020007083836762738548458177115317644"
        "7573027006985557136695962284291481986083493647529271907416844436551070434271155969950809304288017790"
        "41744977920001e+308", 0x7ff0000000000000ULL},
    {
        "1.79769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207"
        "0963302864166928879109465555478519404026306574886715058206819089020007083836762738548458177115317644"
        "7573027006985557136695962284291481986083493647529271907416844436551070434271155969950809304288017790"
        "417449779e+308", 0x7fefffffffffffffULL},
    {"1.00000000000000011102230246251565404236316680908203125e+0", 0x3ff0000000000000ULL},
    {"1.000000000000000111022302462515654042363166809082031250001e+0", 0x3ff0000000000001ULL},
    {"1.0000000000000001110223024625156540423631668090820312e+0", 0x3ff0000000000000ULL},
    {"1.00000000000000012490009027033011079765856266021728515625e-1", 0x3fb999999999999aULL},
    {"1.000000000000000124900090270330110797658562660217285156250001e-1", 0x3fb999999999999bULL},
    {"1.0000000000000001249000902703301107976585626602172851562e-1", 0x3fb999999999999aULL},
    {"1.23456789125000007450580596923828125e+8", 0x419d6f3454800000ULL},
    {"1.234567891250000074505805969238281250001e+8", 0x419d6f3454800001ULL},
    {"1.2345678912500000745058059692382812e+8", 0x419d6f3454800000ULL},
    {
        "0.00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "001", 0x0000000000000000ULL},
    {
        "1000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0.5", 0x7e37e43c8800759cULL},
    {
        "0.99999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        "99", 0x3ff0000000000000ULL},
    {
        "1.00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "001", 0x3ff0000000000000ULL},
};

struct Shortest {
    uint64_t pattern;
    const char *text;
};

// Doubles and Python's repr() of them. The first rows are doubles for which a decimal of one or two
// digits lies exactly on the edge of the interval that rounds to them, and is the text to write.
static const Shortest SHORTEST[] = {
    {0x449017f7df96be18ULL, "1.9e+22"},
    {0x4491c9a62d04ed0cULL, "2.1e+22"},
    {0x44937b547a731c02ULL, "2.3e+22"},
    {0x44952d02c7e14af6ULL, "2.5e+22"},
    {0x4496deb1154f79ecULL, "2.7e+22"},
    {0x4498905f62bda8e0ULL, "2.9e+22"},
    {0x449a420db02bd7d6ULL, "3.1e+22"},
    {0x449bf3bbfd9a06caULL, "3.3e+22"},
    {0x449da56a4b0835c0ULL, "3.5e+22"},
    {0x449f5718987664b4ULL, "3.7e+22"},
    {0x44a52d02c7e14af6ULL, "5e+22"},
    {0x44ada56a4b0835c0ULL, "7e+22"},
    // The doubles either side of those: the same decimal is on their edge too, but reads back as the one above.
    {0x449017f7df96be19ULL, "1.9000000000000006e+22"},
    {0x449017f7df96be17ULL, "1.8999999999999998e+22"},
    {0x4491c9a62d04ed0dULL, "2.1000000000000002e+22"},
    {0x4491c9a62d04ed0bULL, "2.0999999999999994e+22"},
    {0x44937b547a731c03ULL, "2.3000000000000006e+22"},
    {0x44937b547a731c01ULL, "2.2999999999999998e+22"},
    {0x44952d02c7e14af7ULL, "2.5000000000000002e+22"},
    {0x44952d02c7e14af5ULL, "2.4999999999999994e+22"},
    {0x4496deb1154f79edULL, "2.7000000000000006e+22"},
    {0x4496deb1154f79ebULL, "2.6999999999999998e+22"},
    {0x4498905f62bda8e1ULL, "2.9000000000000002e+22"},
    {0x4498905f62bda8dfULL, "2.8999999999999994e+22"},
    {0x449a420db02bd7d7ULL, "3.1000000000000006e+22"},
    {0x449a420db02bd7d5ULL, "3.0999999999999998e+22"},
    {0x449bf3bbfd9a06cbULL, "3.3000000000000002e+22"},
    {0x449bf3bbfd9a06c9ULL, "3.2999999999999994e+22"},
    {0x449da56a4b0835c1ULL, "3.5000000000000006e+22"},
    {0x449da56a4b0835bfULL, "3.4999999999999998e+22"},
    {0x449f5718987664b5ULL, "3.7000000000000002e+22"},
    {0x449f5718987664b3ULL, "3.6999999999999994e+22"},
    {0x44a52d02c7e14af7ULL, "5.0000000000000004e+22"},
    {0x44a52d02c7e14af5ULL, "4.999999999999999e+22"},
    {0x44ada56a4b0835c1ULL, "7.000000000000001e+22"},
    {0x44ada56a4b0835bfULL, "6.9999999999999996e+22"},
    {0x0000000000000000ULL, "0.0"},
    {0x8000000000000000ULL, "-0.0"},
    {0x3ff0000000000000ULL, "1.0"},
    {0xbff0000000000000ULL, "-1.0"},
    {0x3fb999999999999aULL, "0.1"},
    {0x3fe0000000000000ULL, "0.5"},
    {0x4004000000000000ULL, "2.5"},
    {0x4059000000000000ULL, "100.0"},
    {0x430c6bf526340000ULL, "1000000000000000.0"},
    {0x4341c37937e08000ULL, "1e+16"},
    {0x4376345785d8a000ULL, "1e+17"},
    {0x437b69b4ba630f35ULL, "1.2345678901234568e+17"},
    {0x444b1ae4d6e2ef50ULL, "1e+21"},
    {0x4480f0cf064dd592ULL, "1e+22"},
    {0x44b52d02c7e14af6ULL, "1e+23"},
    {0x54b249ad2594c37dULL, "1e+100"},
    {0x7e41eb2d66005835ULL, "1.5e+300"},
    {0x7fefffffffffffffULL, "1.7976931348623157e+308"},
    {0x3f1a36e2eb1c432dULL, "0.0001"},
    {0x3ee4f8b588e368f1ULL, "1e-05"},
    {0x3e7ad7f29abcaf48ULL, "1e-07"},
    {0x3de49da7e361ce4cULL, "1.5e-10"},
    {0x0000000000000001ULL, "5e-324"},
    {0x0010000000000000ULL, "2.2250738585072014e-308"},
    {0x000fffffffffffffULL, "2.225073858507201e-308"},
    {0x3fd3333333333334ULL, "0.30000000000000004"},
    {0x3fd5555555555555ULL, "0.3333333333333333"},
    {0x3fe5555555555555ULL, "0.6666666666666666"},
    {0x419d6f3454800000ULL, "123456789.125"},
    {0x4340000000000000ULL, "9007199254740992.0"},
    {0x433fffffffffffffULL, "9007199254740991.0"},
    {0x4330000000000000ULL, "4503599627370496.0"},
    {0x3f30000000000000ULL, "0.000244140625"},
    {0x4090000000000000ULL, "1024.0"},
    {0x4085400000000000ULL, "680.0"},
    {0x4009666666666666ULL, "3.175"},
    {0x4028b0a3d70a3d71ULL, "12.345"},
    {0x42bd36f015b10ed0ULL, "32121793392910.812"},
    {0x4301bf12de0084baULL, "624395294871703.2"},
    {0x3f0a36e2eb1c432dULL, "5e-05"},
    {0x44b52d02c7e14af6ULL, "1e+23"},
    {0x3ee9e409302678baULL, "1.2345678901234568e-05"},
    {0x4011666666666666ULL, "4.35"},
    {0x3f50000000000000ULL, "0.0009765625"},
    {0x3f40000000000000ULL, "0.00048828125"},
    {0x43b0000000000000ULL, "1.152921504606847e+18"},
    {0x0010000000000000ULL, "2.2250738585072014e-308"},
    {0x3eb0000000000000ULL, "9.5367431640625e-07"},
    {0x3f1a36e2eb1c432cULL, "9.999999999999999e-05"},
    {0x3fd3333333333333ULL, "0.3"},
    {0x4005666666666666ULL, "2.675"},
    {0x40b3880000000000ULL, "5000.0"},
    {0x3ff0000000000001ULL, "1.0000000000000002"},
    {0x3fefffffffffffffULL, "0.9999999999999999"},
    {0x41b1de784a000000ULL, "299792458.0"},
    {0x44dfe185ca57c517ULL, "6.02214076e+23"},
    {0x3c07a4da290c1653ULL, "1.602176634e-19"},
    {0x0000000000000001ULL, "5e-324"},
    {0x7feffffffffffffeULL, "1.7976931348623155e+308"},
};

static void decimals() {
    for (const Decimal &row : DECIMALS) {
        const std::string text = row.text;
        double quick = 0, exact = 0;
        const bool read = text::parse_double(text.data(), text.data() + text.size(), quick) &&
                          text::parse_double(text.data(), text.data() + text.size(), exact, true);
        expect(read && bits_of(quick) == row.pattern && bits_of(exact) == row.pattern, "the nearest double to " + text.substr(0, 60));
    }
    for (const Shortest &row : SHORTEST) {
        const double value = from_bits(row.pattern);
        expect(text::repr(value) == row.text, std::string("repr gives ") + row.text + ", not " + text::repr(value));
        const std::string text = row.text;
        double back = 0;
        expect(text::parse_double(text.data(), text.data() + text.size(), back) && bits_of(back) == row.pattern,
               std::string("reading back ") + row.text);
    }
    double out = 0;
    for (const char *bad : {"", "-", "+", ".", "e5", "1e", "1e+", "1.5x", " 1", "1 ", "0x10", "1..2", "1e5.5", "--1", "inf", "nan", "1,5"}) {
        const std::string text = bad;
        expect(!text::parse_double(text.data(), text.data() + text.size(), out), "not a decimal: '" + text + "'");
    }
    for (const char *good : {"1.", ".5", "+1.5", "-.5e-3", "007", "1e+0005"}) {
        const std::string text = good;
        expect(text::parse_double(text.data(), text.data() + text.size(), out), "a decimal: '" + text + "'");
    }
    // A long run of digits costs no more than a short one once it is read: a megabyte either side of the point.
    const std::string long_whole = "1" + std::string(1 << 20, '0'), long_small = "0." + std::string(1 << 20, '0') + "1",
                      long_one = "1." + std::string(1 << 20, '0') + "1", long_nines = "0." + std::string(1 << 20, '9');
    expect(text::parse_double(long_whole.data(), long_whole.data() + long_whole.size(), out) && std::isinf(out), "a megabyte of digits: infinity");
    expect(text::parse_double(long_small.data(), long_small.data() + long_small.size(), out) && out == 0, "a megabyte of zeros: zero");
    expect(text::parse_double(long_one.data(), long_one.data() + long_one.size(), out) && out == 1.0, "a megabyte of zeros after 1");
    expect(text::parse_double(long_nines.data(), long_nines.data() + long_nines.size(), out) && out == 1.0, "a megabyte of nines");

    // Digits past those that are kept still count: a tie, and the same with a 1 nine hundred places along.
    const std::string tie = "9007199254740993." + std::string(900, '0'), above = tie + "1";
    double tied = 0, over = 0;
    expect(text::parse_double(tie.data(), tie.data() + tie.size(), tied) && bits_of(tied) == 0x4340000000000000ULL &&
               text::parse_double(above.data(), above.data() + above.size(), over) && bits_of(over) == 0x4340000000000001ULL,
           "a digit far past the last one kept breaks a tie");

    // Every short cut against the exact arithmetic, over doubles of every size and the decimals near them.
    Random random{0x9E3779B97F4A7C15ULL};
    int unequal_digits = 0, unequal_reads = 0, lost = 0;
    const auto check = [&](double value) {
        if (!std::isfinite(value) || value == 0) return;
        value = std::fabs(value);
        char quick[17], exact[17];
        int quick_point = 0, exact_point = 0;
        const int quick_count = text::shortest_digits(value, quick, quick_point);
        const int exact_count = text::shortest_digits_exact(value, exact, exact_point);
        if (quick_count != exact_count || quick_point != exact_point || std::memcmp(quick, exact, static_cast<size_t>(quick_count)) != 0)
            ++unequal_digits;
        // The shortest text, and the same digits with more after them, read by both paths.
        const std::string shortest = text::repr(value);
        std::string longer = "0." + std::string(exact, static_cast<size_t>(exact_count)) + std::to_string(random.next() % 1000) + "e" +
                             std::to_string(exact_point);
        for (const std::string &text : {shortest, longer}) {
            double a = 0, b = 0;
            text::parse_double(text.data(), text.data() + text.size(), a);
            text::parse_double(text.data(), text.data() + text.size(), b, true);
            if (bits_of(a) != bits_of(b)) ++unequal_reads;
            if (&text == &shortest && bits_of(a) != bits_of(value)) ++lost;
        }
    };
    for (int i = 0; i < 60000; ++i) {
        const uint64_t pattern = random.next();
        check(from_bits(pattern));                                                      // any double at all
        check(from_bits((pattern & 0x000fffffffffffffULL) | ((1013 + pattern % 64) << 52)));  // 2^-10 to 2^53: the quick digits
        check(static_cast<double>(static_cast<int64_t>(pattern % 8000000) - 4000000) / 1000.0);  // coordinates to three places
        check(static_cast<double>(pattern % 100000000) / 1000000.0 + 1e-9);
    }
    for (int power = -1074; power < 1024; ++power) {
        const double value = std::ldexp(1.0, power);
        check(value);
        check(std::nextafter(value, 0.0));
        check(std::nextafter(value, std::numeric_limits<double>::infinity()));
    }
    for (int whole = 1; whole < 20000; ++whole) {
        check(whole);
        check(whole / 8.0);
        check(whole / 1000.0);
    }
    expect(unequal_digits == 0, "the quick digits of a double are the exact ones (" + std::to_string(unequal_digits) + " differ)");
    expect(unequal_reads == 0, "the quick reading of a decimal is the exact one (" + std::to_string(unequal_reads) + " differ)");
    expect(lost == 0, "the shortest text reads back as the same double (" + std::to_string(lost) + " do not)");
}

static void reading() {
    expect(json::parse("null").is_null() && json::parse(" true ").get<bool>() && !json::parse("\tfalse\r\n").get<bool>(), "the literals");
    expect(json::parse("\"text\"").get<std::string>() == "text" && json::parse("[]").is_array() && json::parse("{}").is_object() &&
               json::parse("[]").empty() && json::parse("{}").empty(),
           "text, an empty array and an empty object");
    expect(json::parse("\xEF\xBB\xBF[1]").size() == 1 && !refusal(" \xEF\xBB\xBF[1]").empty() && !refusal("\xEF\xBB\xBF").empty(),
           "a byte order mark is allowed at the very start only");

    // The kind of number: as the Python reference tells an int from a float.
    expect(kind("0") == "u0" && kind("1") == "u1" && kind("-1") == "i-1" && kind("-0") == "i0" && kind("1.0") == "f1.0" &&
               kind("1e3") == "f1000.0" && kind("1E3") == "f1000.0" && kind("-0.0") == "f-0.0" && kind("0e0") == "f0.0" && kind("1e-2") == "f0.01",
           "whole numbers and floats");
    expect(kind("18446744073709551615") == "u18446744073709551615" && kind("18446744073709551616") == "f1.8446744073709552e+19" &&
               kind("9223372036854775808") == "u9223372036854775808" && kind("-9223372036854775808") == "i-9223372036854775808" &&
               kind("-9223372036854775809") == "f-9.223372036854776e+18" && kind("123456789012345678901234567890") == "f1.2345678901234568e+29",
           "a whole number too large for 64 bits is read as a float");
    expect(kind("0.1") == "f0.1" && kind("0.30000000000000004") == "f0.30000000000000004" && kind("1e22") == "f1e+22" &&
               kind("1e16") == "f1e+16" && kind("1e15") == "f1000000000000000.0" && kind("0.0001") == "f0.0001" && kind("0.00001") == "f1e-05" &&
               kind("5e-324") == "f5e-324" && kind("1.7976931348623157e308") == "f1.7976931348623157e+308" && kind("1e-400") == "f0.0" &&
               kind("-1e-400") == "f-0.0" && kind("123456789.125") == "f123456789.125",
           "floats are written as Python's repr writes them");
    expect(!refusal("1e309").empty() && !refusal("-1e309").empty() && !refusal("1e99999999999999999999").empty() &&
               !refusal("1" + std::string(400, '0')).empty() && !refusal("1.7976931348623159e308").empty(),
           "a number past the range of a double is refused");
    for (const Decimal &row : DECIMALS) {
        const std::string text = row.text;
        const bool json_number = text.find_first_of("+") != 0 && text[0] != '.' && text.find(".e") == std::string::npos;
        if (!json_number || std::isinf(from_bits(row.pattern))) continue;
        const json value = json::parse(text);
        const bool whole = text.find_first_of(".eE") == std::string::npos;
        // A whole number is compared by value: "-0" is the whole number 0, not the float -0.0.
        expect(whole ? value.is_number() && value.get<double>() == from_bits(row.pattern)
                     : value.is_number_float() && bits_of(value.get<double>()) == row.pattern,
               "the JSON number " + text.substr(0, 60));
    }

    // Text: every escape, surrogate pairs, and UTF-8 as it stands.
    expect(json::parse(R"("\"\\\/\b\f\n\r\t")").get<std::string>() == "\"\\/\b\f\n\r\t", "the short escapes");
    expect(json::parse(R"("Aé中😀𝄞")").get<std::string>() == "A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\xF0\x9D\x84\x9E",
           "\\u escapes and surrogate pairs become UTF-8");
    expect(json::parse(R"("\u0000x")").get<std::string>() == std::string("\0x", 2) && json::parse("\"\x7F\"").get<std::string>() == "\x7F",
           "an escaped NUL and a bare DEL");
    expect(json::parse("\"\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\xED\x9F\xBF\xEE\x80\x80\xF4\x8F\xBF\xBF\"").get<std::string>().size() == 19,
           "well-formed UTF-8 passes through");
    for (const char *bad : {"\"\xC3\"", "\"\xC3\x28\"", "\"\x80\"", "\"\xC0\xAF\"", "\"\xC1\xBF\"", "\"\xE0\x9F\xBF\"", "\"\xED\xA0\x80\"",
                            "\"\xED\xBF\xBF\"", "\"\xF0\x8F\xBF\xBF\"", "\"\xF4\x90\x80\x80\"", "\"\xF5\x80\x80\x80\"", "\"\xFF\"", "\"\xE4\xB8\"",
                            "\"\xF0\x9F\x98\"", "\"\xF8\x88\x80\x80\x80\""})
        expect(!refusal(bad).empty(), "bytes that are not UTF-8 are refused");
    for (const char *bad : {R"("\ud800")", R"("\ud800A")", R"("\udc00")", R"("\ud800\ud800")", R"("\uD800\n")", R"("\u12")", R"("\u123g")",
                            R"("\x41")", R"("\a")", R"("\')", "\"\t\"", "\"\n\"", "\"\x1F\"", "\"a", "\"\\", "\"\\u"})
        expect(!refusal(bad).empty(), std::string("text that is refused: ") + bad);
    expect(!refusal(std::string("\"\0\"", 3)).empty() && !refusal(std::string("null\0", 5)).empty() && !refusal(std::string("1\0 2", 4)).empty(),
           "a NUL byte is refused, in text and after a value");

    // Arrays and objects: names in order, the last of a repeated name.
    const json object = json::parse(R"( { "b" : [ 1 , 2.5 , "x" , null , true , { } , [ ] ] , "a" : { "c" : -3 } , "b" : 7, "B": 0, "": 1 , "é": 2 } )");
    expect(object.dump() == "{\"\":1,\"B\":0,\"a\":{\"c\":-3},\"b\":7,\"\xC3\xA9\":2}", "an object keeps its names in byte order, the last of two");
    expect(json::parse(R"({"a":1,"a":2,"a":3})").dump() == R"({"a":3})" && json::parse(R"({"z":1,"y":2,"z":3,"y":4,"x":5})").dump() == R"({"x":5,"y":4,"z":3})",
           "repeated names");
    expect(json::parse("[1,[2,[3,[4,[]]]],{\"k\":[{}]}]").dump() == "[1,[2,[3,[4,[]]]],{\"k\":[{}]}]", "nested arrays and objects");

    // What is not JSON.
    for (const char *bad : {"", " ", "nul", "nulll", "tru", "True", "NaN", "Infinity", "-Infinity", "+1", "01", "-01", "1.", ".5", "1e", "1e+", "0x10",
                            "-", "--1", "1 2", "[", "]", "{", "}", "[1,]", "[,1]", "[1 2]", "[1]]", "{\"a\":1,}", "{\"a\"}", "{\"a\":}", "{a:1}",
                            "{'a':1}", "{\"a\":1 \"b\":2}", "{1:2}", "{}{}", "[] x", "// c\nnull", "/* c */ null", "null // c", "\x0Cnull",
                            "\x0Bnull", "\xA0null", "[1,2", "{\"a\":[1,2}", "\"a\" \"b\"", "truefalse", "1,2", ":", ","})
        expect(!refusal(bad).empty(), std::string("not JSON: ") + bad);
    expect(refusal("[1,\n 2,\n x]") == "line 3, column 2: a value was expected, found 'x'", "a refusal says where: " + refusal("[1,\n 2,\n x]"));
    expect(refusal("{\"a\":1,}") == "line 1, column 8: a name in quotes was expected in an object, found '}'", "a refusal says what was expected");

    // Depth: enough for any real document, and bounded so that hostile text cannot exhaust the stack.
    const auto nested = [](size_t depth, bool objects) {
        std::string text;
        for (size_t i = 0; i < depth; ++i) text += objects ? "{\"a\":" : "[";
        text += "1";
        for (size_t i = 0; i < depth; ++i) text += objects ? "}" : "]";
        return text;
    };
    expect(refusal(nested(json::MAX_DEPTH, false)).empty() && refusal(nested(json::MAX_DEPTH, true)).empty(), "nesting to the limit is read");
    expect(refusal(nested(json::MAX_DEPTH + 1, false)).find("nested more than 200 deep") != std::string::npos &&
               !refusal(nested(json::MAX_DEPTH + 1, true)).empty(),
           "nesting past the limit is refused");
    expect(!refusal(std::string(1000000, '[')).empty() && !refusal(nested(300000, true)).empty() && !refusal(nested(300000, false)).empty(),
           "a million open brackets are refused, not followed");

    // Many names, given backwards and each twice: sorted once at the end, not inserted one by one.
    std::string many = "{";
    for (int i = 99999; i >= 0; --i) many += "\"k" + std::to_string(i) + "\":" + std::to_string(i) + ",\"k" + std::to_string(i) + "\":0,";
    many += "\"end\":null}";
    const json large = json::parse(many);
    expect(large.size() == 100001 && large["k99999"] == 0 && large.begin().key() == "end" && large.contains("k0") && !large.contains("k100000"),
           "a hundred thousand names in reverse");

    expect(json::parse("[1", nullptr, false).is_discarded() && json::parse("0.01", nullptr, false).get<double>() == 0.01 &&
               json::parse("mm", nullptr, false).is_discarded() && !json::parse("[1,2]", nullptr, false).is_discarded() &&
               throws<json::parse_error>([] { json::parse("[1", nullptr, true); }),
           "reading without exceptions");
}

static void writing() {
    expect(json().dump() == "null" && json(true).dump() == "true" && json(false).dump() == "false" && json("").dump() == "\"\"", "the simple values");
    expect(json(0).dump() == "0" && json(-1).dump() == "-1" && json(std::numeric_limits<int64_t>::min()).dump() == "-9223372036854775808" &&
               json(std::numeric_limits<int64_t>::max()).dump() == "9223372036854775807" &&
               json(std::numeric_limits<uint64_t>::max()).dump() == "18446744073709551615" && json(static_cast<size_t>(3)).dump() == "3" &&
               json(static_cast<short>(-7)).dump() == "-7" && json(static_cast<unsigned char>(200)).dump() == "200",
           "whole numbers");
    expect(json(0.0).dump() == "0.0" && json(-0.0).dump() == "-0.0" && json(1.0).dump() == "1.0" && json(0.1).dump() == "0.1" &&
               json(1e16).dump() == "1e+16" && json(1.5f).dump() == "1.5" && json(-2.5e-7).dump() == "-2.5e-07" && json(680.0).dump() == "680.0",
           "floats keep a point or an exponent, so that they are read back as floats");
    expect(json(std::numeric_limits<double>::quiet_NaN()).dump() == "null" && json(std::numeric_limits<double>::infinity()).dump() == "null" &&
               json(-std::numeric_limits<double>::infinity()).dump() == "null",
           "a float that is not finite is written as null");
    for (const Shortest &row : SHORTEST) {
        const double value = from_bits(row.pattern);
        const json back = json::parse(json(value).dump());
        expect(json(value).dump() == row.text && back.is_number_float() && bits_of(back.get<double>()) == row.pattern,
               std::string("a float written and read back: ") + row.text);
    }
    expect(json(std::string("q\"b\\s/\b\f\n\r\t\x01\x1F\x7F", 14)).dump() == R"("q\"b\\s/\b\f\n\r\t\u0001\u001f)" "\x7F\"" &&
               json(std::string("\0", 1)).dump() == R"("\u0000")",
           "text is written with the quote, the backslash and control characters escaped");
    expect(json("\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80").dump() == "\"\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\"", "UTF-8 is written as it stands");
    expect(throws<json::type_error>([] { json("\xFF").dump(); }) && throws<json::type_error>([] { json{{"k", "\xC3"}}.dump(); }) &&
               throws<json::type_error>([] { json{{"\xED\xA0\x80", 1}}.dump(); }),
           "text that is not UTF-8 is not written");
    const std::string all = R"({"a":[1,-2,3.5,"x",null,true,false,[],{}],"b":{"c":{"d":[[1.0,2.0],[3.0,4.0]]}},"e":""})";
    expect(json::parse(all).dump() == all, "one line, no spaces, as it was read");
}

static void values() {
    // Braced lists, as the core writes its answers.
    const std::vector<std::string> lines{"G21", "G90"};
    const std::vector<double> offsets{0.0, -0.125};
    const std::array<double, 4> bounds{{0.0, 1.0, 2.5, 3.0}};
    const size_t columns = 3;
    const json answer{{"lines", lines}, {"grid", {{"x0", 8.0}, {"columns", columns}}}, {"bounds", bounds}, {"offsets", offsets}, {"none", nullptr}};
    expect(answer.dump() == R"({"bounds":[0.0,1.0,2.5,3.0],"grid":{"columns":3,"x0":8.0},"lines":["G21","G90"],"none":null,"offsets":[0.0,-0.125]})",
           "a braced list of names and values is an object");
    expect(json{1, 2}.dump() == "[1,2]" && json{"a", 1}.dump() == R"(["a",1])" && json{{"a", 1}}.dump() == R"({"a":1})" &&
               json{{"a", 1}, {"b"}}.dump() == R"([["a",1],["b"]])" && json{{"b", 1}, {"a", 2}, {"b", 3}}.dump() == R"({"a":2,"b":1})" &&
               json{{"offset", {1, 2}}}.dump() == R"({"offset":[1,2]})",
           "what makes a braced list an object");
    expect(json::array({{"a", 1}}).dump() == R"([["a",1]])" && json::array({1.5, nullptr, 0, "s", true}).dump() == R"([1.5,null,0,"s",true])" &&
               json::array().dump() == "[]" && json::object().dump() == "{}",
           "array() and object()");
    json row = json::array({1.0, 2.0, 3.0, 4.0, 5.0, nullptr, 6.0});
    row[4] = row[6] = 0;
    row[5] = json(0.5);
    expect(row.dump() == "[1.0,2.0,3.0,4.0,0,0.5,0]", "a whole 0 beside floats stays a whole number");

    json built;
    built["z"] = 1;
    built["a"] = lines;
    built["m"] = offsets;
    built["a"] = "again";
    expect(built.dump() == R"({"a":"again","m":[0.0,-0.125],"z":1})" && built.size() == 3, "names are added in order and replaced in place");
    json list;
    list.push_back(1);
    list.push_back({1, 2});
    list.push_back({{"k", "v"}});
    list.push_back(lines);
    expect(list.dump() == R"([1,[1,2],{"k":"v"},["G21","G90"]])", "push_back");
    json rows;
    rows.emplace_back(2.5);
    rows.emplace_back("s");
    rows.emplace_back(offsets).push_back(1);
    expect(rows.dump() == R"([2.5,"s",[0.0,-0.125,1]])", "emplace_back");
    json fewer = built;
    expect(fewer.erase("m") == 1 && fewer.erase("m") == 0 && fewer.erase("none") == 0 && fewer.dump() == R"({"a":"again","z":1})" &&
               throws<json::type_error>([&] { list.erase("a"); }),
           "erase");
    expect(built.value("z", 5) == 1 && built.value("y", 5) == 5 && built.value("a", "x") == "again" && built.value("b", "x") == "x" &&
               built.value("z", 0.5) == 1.0 && built.value("q", std::string("s")) == "s" && throws<json::type_error>([&] { built.value("a", 1); }) &&
               throws<json::type_error>([&] { list.value("a", 1); }),
           "value with a fallback");
    json copy = built;
    copy["z"] = json::array({1});
    expect(built["z"] == 1 && copy["z"].is_array(), "a copy is separate");
    const json moved = std::move(copy);
    expect(moved["z"].is_array() && copy.is_null(), "a value moved from is null");  // NOLINT(bugprone-use-after-move)

    // Asking a value for what it holds.
    const json parsed = json::parse(R"({"n":3,"m":-4,"f":2.75,"s":"text","t":true,"l":["a","b"],"z":null,"big":18446744073709551615})");
    expect(parsed["n"].get<int>() == 3 && parsed["n"].get<size_t>() == 3 && parsed["n"].get<double>() == 3.0 && parsed["m"].get<int64_t>() == -4 &&
               parsed["f"].get<double>() == 2.75 && parsed["f"].get<int>() == 2 && parsed["big"].get<uint64_t>() == 18446744073709551615ULL &&
               parsed["s"].get<std::string>() == "text" && parsed["s"].get_ref<const std::string &>() == "text" && parsed["t"].get<bool>() &&
               parsed["l"].get<std::vector<std::string>>() == std::vector<std::string>{"a", "b"},
           "get");
    expect(throws<json::type_error>([&] { parsed["s"].get<double>(); }) && throws<json::type_error>([&] { parsed["n"].get<std::string>(); }) &&
               throws<json::type_error>([&] { parsed["n"].get<bool>(); }) && throws<json::type_error>([&] { parsed["t"].get<double>(); }) &&
               throws<json::type_error>([&] { parsed["t"].get<int>(); }) && throws<json::type_error>([&] { parsed["n"].get_ref<const std::string &>(); }) &&
               throws<json::type_error>([&] { parsed["l"].get<std::vector<double>>(); }) && throws<json::type_error>([&] { parsed["z"].get<int>(); }) &&
               throws<json::type_error>([] { json(1e30).get<int64_t>(); }) && throws<json::type_error>([] { json(-1.5).get<uint64_t>(); }) &&
               throws<json::type_error>([] { json(-1).get<size_t>(); }) && throws<json::type_error>([] { json(-1).get<uint64_t>(); }) &&
               throws<json::type_error>([] { json(300).get<unsigned char>(); }) &&
               throws<json::type_error>([] { json(uint64_t(1) << 63).get<int64_t>(); }) &&
               throws<json::type_error>([] { json(int64_t(1) << 40).get<int>(); }) && json(255).get<unsigned char>() == 255 &&
               json(-128).get<signed char>() == -128 && json(uint64_t(7)).get<int>() == 7 &&
               json::parse("[1", nullptr, false).dump() == "null" &&
               throws<json::type_error>([] { json(3e9).get<int>(); }) && throws<json::type_error>([] { json(std::nan("")).get<int>(); }),
           "get refuses what the value is not, and a float that does not fit a whole number");
    expect(parsed.is_object() && parsed["l"].is_array() && parsed["s"].is_string() && parsed["t"].is_boolean() && parsed["z"].is_null() &&
               parsed["n"].is_number() && parsed["n"].is_number_integer() && parsed["n"].is_number_unsigned() && !parsed["n"].is_number_float() &&
               parsed["m"].is_number_integer() && !parsed["m"].is_number_unsigned() && parsed["f"].is_number_float() && !parsed["f"].is_number_integer() &&
               !parsed["t"].is_number() && parsed["n"].type() == json::Type::number_unsigned && parsed["m"].type() == json::Type::number_integer,
           "the kinds of value");
    expect(parsed.size() == 8 && parsed["l"].size() == 2 && parsed["z"].size() == 0 && parsed["z"].empty() && parsed["s"].size() == 1 &&
               !parsed["n"].empty() && parsed.contains("f") && !parsed.contains("g") && !parsed["l"].contains("a") && parsed.find("g") == parsed.end() &&
               parsed.find("f")->get<double>() == 2.75 && parsed["s"].find("x") == parsed["s"].end() && parsed.at("l").at(1) == "b" &&
               parsed.find("l").key() == "l",
           "size, contains, find and at");
    expect(throws<json::out_of_range>([&] { parsed.at("g"); }) && throws<json::out_of_range>([&] { parsed["g"]; }) &&
               throws<json::out_of_range>([&] { parsed["l"][2]; }) && throws<json::type_error>([&] { parsed["n"]["x"]; }) &&
               throws<json::type_error>([&] { parsed["n"][0]; }) && throws<json::type_error>([&] { parsed[0]; }) &&
               throws<json::type_error>([] { json value = 1; value["x"] = 2; }) && throws<json::type_error>([] { json value = "s"; value.push_back(1); }) &&
               throws<json::exception>([&] { parsed.at("g"); }),
           "a name or a place that is not there");

    // Iteration.
    std::string seen;
    for (auto it = parsed.begin(); it != parsed.end(); ++it) seen += it.key() + "=" + it.value().dump() + ";";
    for (const json &entry : parsed["l"]) seen += entry.get<std::string>();
    for (const auto &item : parsed.items()) seen += item.key();
    for (const json &entry : parsed["n"]) seen += "<" + entry.dump() + ">";
    for (const json &entry : parsed["z"]) seen += "never" + entry.dump();
    expect(seen == R"(big=18446744073709551615;f=2.75;l=["a","b"];m=-4;n=3;s="text";t=true;z=null;abbigflmnstz<3>)", "iteration: " + seen);
    json changed = json::parse("[1,2,3]");
    for (json &entry : changed) entry = entry.get<int>() * 2;
    expect(changed.dump() == "[2,4,6]", "entries can be changed in place");

    // Equality: by value between the kinds of number, never between a number and anything else.
    expect(json(1) == json(1.0) && json(1u) == json(1) && json(0) == json(-0.0) && json(1) != json(2) && json(1.5) != json(1) &&
               json(true) != json(1) && json("1") != json(1) && json() == json(nullptr) && json() != json(0) && json::array() != json::object() &&
               json(std::nan("")) != json(std::nan("")) && json("v") == "v" && json("v") != "w" &&
               json::parse(R"({"b":[1,2.0,"x"],"a":{"c":null}})") == json{{"a", {{"c", nullptr}}}, {"b", {1.0, 2, "x"}}} &&
               json::parse("[1,2]") != json::parse("[1,2,3]") && json::parse(R"({"a":1})") != json::parse(R"({"a":1,"b":2})"),
           "equality");

    // Random documents, written and read back: the same value and the same text.
    Random random{12345};
    int unequal = 0;
    for (int round = 0; round < 3000; ++round) {
        const auto make = [&](const auto &self, int depth) -> json {
            const uint64_t pick = random.next() % (depth > 4 ? 7 : 10);
            switch (pick) {
            case 0:
                return nullptr;
            case 1:
                return random.next() % 2 == 0;
            case 2:
                return static_cast<int64_t>(random.next());
            case 3:
                return random.next();
            case 4: {
                const double value = from_bits(random.next());
                return std::isfinite(value) ? value : 0.25;
            }
            case 5:
                return static_cast<double>(static_cast<int64_t>(random.next() % 4000000) - 2000000) / 1000.0;
            case 6: {
                std::string text;
                for (uint64_t n = random.next() % 12; n; --n) {
                    static const char *const PIECES[] = {"a", "\"", "\\", "\n", "\x01", "\x7F", "\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80", "/", " ", "k"};
                    text += PIECES[random.next() % 12];
                }
                return text;
            }
            case 7:
            case 8: {
                json list = json::array();
                for (uint64_t n = random.next() % 5; n; --n) list.push_back(self(self, depth + 1));
                return list;
            }
            default: {
                json object = json::object();
                for (uint64_t n = random.next() % 5; n; --n) object[std::string(1, static_cast<char>('a' + random.next() % 6))] = self(self, depth + 1);
                return object;
            }
            }
        };
        const json value = make(make, 0);
        const std::string text = value.dump();
        const json back = json::parse(text);
        if (!(back == value) || back.dump() != text) ++unequal;
    }
    expect(unequal == 0, "random documents are read back as they were written (" + std::to_string(unequal) + " are not)");
}

int main() {
    decimals();
    reading();
    writing();
    values();
    std::printf(failures ? "%d checks failed\n" : "PASS JSON and number conversion self-test%.0d\n", failures);
    return failures ? 1 : 0;
}
