/*
 * tools/prompt_probe.cpp -- dumps the prompt parser's full output for a list of prompts.
 *
 * The Rust core has its own prompt grammar (rust/af-core/src/prompt.rs); this probe prints the
 * C++ reference behaviour in a form the Rust `promptdump` binary can be diffed against, so the two
 * implementations are kept honest over a corpus instead of by inspection. The two binaries must
 * print byte-identical output for every prompt in tools/prompt_corpus.txt.
 *
 * Build (host):
 *   $CXX -std=c++17 -O2 -I native/include -I native/src tools/prompt_probe.cpp \
 *        native/src/af_prompt.cpp -o build-host/prompt_probe
 * Run:
 *   build-host/prompt_probe tools/prompt_corpus.txt > /tmp/cpp_prompts.txt
 *
 * One line per prompt, fields in a fixed order so a diff is readable:
 *   ok=<n> words=<n> tokens=<n> | sex= build= hair= outfit= foot= age= height= muscle= fat=
 *   waist= hip= chest= neck= thigh= calf= bicep= shoulder= inseam= head= melanin=
 *   hair_tone=r,g,b cloth=r,g,b gear=r,g,b | tok:"text"->field,...
 */
#include "../native/include/afcore.h"
#include "../native/src/af_prompt.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

/* Both sides print integers scaled by 1e5: C's %g and Rust's float formatting do not agree on
   insignificant digits, and a parity diff must fail only on real behaviour differences. */
static long long Q(float v) { return static_cast<long long>(std::llround(static_cast<double>(v) * 100000.0)); }

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "tools/prompt_corpus.txt";
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        af_spec spec;
        af_spec_init(&spec);
        af::PromptParse parse;
        const int n = af::parsePrompt(line.c_str(), &spec, &parse);
        std::printf(
            "ok=%d words=%d tokens=%d | sex=%d build=%d hair=%d outfit=%d foot=%d age=%lld "
            "height=%lld muscle=%lld fat=%lld waist=%lld hip=%lld chest=%lld neck=%lld "
            "thigh=%lld calf=%lld bicep=%lld shoulder=%lld inseam=%lld head=%lld melanin=%lld "
            "hair_tone=%lld,%lld,%lld cloth=%lld,%lld,%lld gear=%lld,%lld,%lld |",
            n, parse.words, parse.tokenCount, spec.sex, spec.build, spec.hair_style, spec.outfit,
            spec.footwear, Q(spec.age_years), Q(spec.height_m), Q(spec.muscle), Q(spec.body_fat),
            Q(spec.waist_cm), Q(spec.hip_cm), Q(spec.chest_cm), Q(spec.neck_cm), Q(spec.thigh_cm),
            Q(spec.calf_cm), Q(spec.bicep_cm), Q(spec.shoulder_width_cm), Q(spec.inseam_cm),
            Q(spec.head_circumference_cm), Q(spec.skin_melanin), Q(spec.hair_tone[0]),
            Q(spec.hair_tone[1]), Q(spec.hair_tone[2]), Q(spec.cloth_tone[0]),
            Q(spec.cloth_tone[1]), Q(spec.cloth_tone[2]), Q(spec.gear_tone[0]),
            Q(spec.gear_tone[1]), Q(spec.gear_tone[2]));
        for (int i = 0; i < parse.tokenCount; ++i) {
            std::printf(" \"%s\"->%s,", parse.tokens[i].text, parse.tokens[i].field);
        }
        std::printf("\n");
    }
    return 0;
}
