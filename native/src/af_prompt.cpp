#include "af_prompt.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace af {
namespace {

struct Word {
    std::string text;   /* lower-cased, punctuation stripped */
    size_t start = 0;
    size_t end = 0;
};

bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '\'' || c == '-';
}

std::vector<Word> tokenise(const char* prompt) {
    std::vector<Word> out;
    if (!prompt) return out;
    const char* p = prompt;
    while (*p) {
        while (*p && !isWordChar(*p)) ++p;
        if (!*p) break;
        Word w;
        w.start = static_cast<size_t>(p - prompt);
        const char* s = p;
        while (*p && isWordChar(*p)) ++p;
        w.end = static_cast<size_t>(p - prompt);
        std::string raw(s, p - s);
        for (char& c : raw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        w.text = raw;
        out.push_back(w);
    }
    return out;
}

bool isNumber(const std::string& s) {
    if (s.empty()) return false;
    bool digit = false, dot = false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) { digit = true; continue; }
        if (c == '.' && !dot) { dot = true; continue; }
        return false;
    }
    return digit;
}

float toFloat(const std::string& s) {
    return static_cast<float>(std::atof(s.c_str()));
}

bool anyOf(const std::string& w, const char* const* list, int n) {
    for (int i = 0; i < n; ++i) {
        if (w == list[i]) return true;
    }
    return false;
}

/* A token is recorded for the UI's "understood" list. */
void record(PromptParse* out, const std::string& text, const char* field, int* matched) {
    if (!out) { ++*matched; return; }
    if (out->tokenCount < 32) {
        PromptToken& t = out->tokens[out->tokenCount];
        std::snprintf(t.text, sizeof(t.text), "%s", text.c_str());
        std::snprintf(t.field, sizeof(t.field), "%s", field);
        ++out->tokenCount;
    }
    ++*matched;
}

/* ---- colour vocabulary ------------------------------------------------------------------- */
struct NamedColour { const char* word; float rgb[3]; };

/* Approximate linear-sRGB reflectances for the common colour words. These are BASE COLOURS, not
   display values: a "navy" garment is a very dark blue with a slight desaturation, and the shader
   will add the lighting. Values are deliberately mid-range so an over-bright light does not clip. */
const NamedColour kColours[] = {
    {"black",       {0.020f, 0.020f, 0.022f}},
    {"charcoal",    {0.045f, 0.046f, 0.050f}},
    {"white",       {0.860f, 0.860f, 0.845f}},
    {"ivory",       {0.820f, 0.795f, 0.720f}},
    {"grey",        {0.240f, 0.245f, 0.250f}},
    {"gray",        {0.240f, 0.245f, 0.250f}},
    {"silver",      {0.640f, 0.650f, 0.660f}},
    {"red",         {0.420f, 0.045f, 0.040f}},
    {"crimson",     {0.330f, 0.020f, 0.035f}},
    {"maroon",      {0.140f, 0.020f, 0.030f}},
    {"orange",      {0.520f, 0.180f, 0.030f}},
    {"yellow",      {0.640f, 0.520f, 0.070f}},
    {"gold",        {0.700f, 0.520f, 0.140f}},
    {"green",       {0.075f, 0.230f, 0.080f}},
    {"olive",       {0.190f, 0.200f, 0.070f}},
    {"teal",        {0.040f, 0.200f, 0.190f}},
    {"cyan",        {0.060f, 0.400f, 0.450f}},
    {"blue",        {0.045f, 0.110f, 0.360f}},
    {"navy",        {0.020f, 0.040f, 0.140f}},
    {"purple",      {0.180f, 0.060f, 0.290f}},
    {"violet",      {0.250f, 0.090f, 0.400f}},
    {"magenta",     {0.420f, 0.050f, 0.280f}},
    {"pink",        {0.640f, 0.290f, 0.330f}},
    {"brown",       {0.160f, 0.085f, 0.045f}},
    {"tan",         {0.400f, 0.270f, 0.170f}},
    {"beige",       {0.560f, 0.480f, 0.370f}},
    {"cream",       {0.700f, 0.640f, 0.520f}},
    {"bronze",      {0.330f, 0.200f, 0.090f}},
    {"copper",      {0.450f, 0.220f, 0.110f}},
};
const int kColourCount = static_cast<int>(sizeof(kColours) / sizeof(kColours[0]));

bool colourFor(const std::string& w, float rgb[3]) {
    for (int i = 0; i < kColourCount; ++i) {
        if (w == kColours[i].word) {
            rgb[0] = kColours[i].rgb[0];
            rgb[1] = kColours[i].rgb[1];
            rgb[2] = kColours[i].rgb[2];
            return true;
        }
    }
    return false;
}

bool isGarmentWord(const std::string& w) {
    static const char* const k[] = {"shirt", "tshirt", "tee", "t-shirt", "trouser", "trousers",
                                    "clothes", "clothing", "jacket", "jeans", "suit", "coat",
                                    "armour", "armor", "dress", "gown", "sweater", "hoodie",
                                    "clothes", "uniform", "outfit", "suit"};
    return anyOf(w, k, static_cast<int>(sizeof(k) / sizeof(k[0])));
}

bool isHairWord(const std::string& w) {
    static const char* const k[] = {"hair", "braid", "braids", "ponytail", "bun", "afro", "mohawk",
                                    "locks", "dreadlocks", "bald", "buzz"};
    return anyOf(w, k, static_cast<int>(sizeof(k) / sizeof(k[0])));
}

/* Maps a body noun onto the af_spec field it names, writing `value` (centimetres). One table, so
   the noun-before-number and number-before-noun paths cannot drift apart. */
const char* setBodyCm(af_spec* spec, const std::string& noun, float value) {
    if (noun == "waist") { spec->waist_cm = value; return "waist_cm"; }
    if (noun == "hip" || noun == "hips") { spec->hip_cm = value; return "hip_cm"; }
    if (noun == "chest" || noun == "bust") { spec->chest_cm = value; return "chest_cm"; }
    if (noun == "neck") { spec->neck_cm = value; return "neck_cm"; }
    if (noun == "thigh") { spec->thigh_cm = value; return "thigh_cm"; }
    if (noun == "calf") { spec->calf_cm = value; return "calf_cm"; }
    if (noun == "bicep" || noun == "arm") { spec->bicep_cm = value; return "bicep_cm"; }
    if (noun == "shoulder" || noun == "shoulders") { spec->shoulder_width_cm = value; return "shoulder_width_cm"; }
    if (noun == "inseam") { spec->inseam_cm = value; return "inseam_cm"; }
    if (noun == "head") { spec->head_circumference_cm = value; return "head_circumference_cm"; }
    return nullptr;
}

bool isSkinToneWord(const std::string& w) {
    static const char* const k[] = {"pale", "fair", "light", "olive", "tan", "tanned", "bronze",
                                    "brown", "dark", "deep", "ebony", "porcelain", "alabaster"};
    return anyOf(w, k, static_cast<int>(sizeof(k) / sizeof(k[0])));
}

} // namespace

int parsePrompt(const char* prompt, af_spec* spec, PromptParse* out) {
    if (out) {
        *out = PromptParse();
    }
    if (!prompt || !spec) return 0;
    const std::vector<Word> words = tokenise(prompt);
    if (out) out->words = static_cast<int>(words.size());
    int matched = 0;

    /* Pending colour words: "red dress", "brown boots", "black hair". A colour adjective is
       remembered until a noun tells us which slot it belongs to; if no noun follows, it is applied
       to the garment, which is the most common intent ("a woman in red"). */
    float pendingColour[3] = {0, 0, 0};
    bool hasPending = false;
    /* Skin-tone and hair-length adjectives behave like colours: "dark hair", "olive skin",
       "long hair" all put the qualifier BEFORE the noun it qualifies, sometimes with a colour in
       between ("long dark hair"). They are held here and applied when the noun arrives. */
    int pendingStyle = -1;        /* af_hair_style, or -1 */
    float pendingMelanin = -1.0f; /* -1 = none */
    /* A word can be consumed by a branch that is not the word's own noun branch: "68 cm waist"
       eats the noun "waist". A later number must not look back at a noun that is already spent,
       or "68 cm waist, 96 cm hips" would set the waist twice. */
    std::vector<bool> consumed(words.size(), false);
    bool leatherModifier = false, plateModifier = false, spaceModifier = false;
    for (const Word& w0 : words) {
        if (w0.text == "leather") leatherModifier = true;
        if (w0.text == "plate" || w0.text == "cuirass" || w0.text == "breastplate") plateModifier = true;
        if (w0.text == "space" || w0.text == "spacesuit" || w0.text == "astronaut") spaceModifier = true;
    }

    for (size_t i = 0; i < words.size(); ++i) {
        const std::string& w = words[i].text;
        float rgb[3];

        /* ---- measurements: "<number> cm | m | mm | inch | ft" and "5 foot 9" ---- */
        if (isNumber(w)) {
            const float value = toFloat(w);
            const std::string next = (i + 1 < words.size()) ? words[i + 1].text : "";
            if (next == "cm" || next == "centimetre" || next == "centimetres" || next == "centimeter" ||
                next == "centimeters") {
                /* Two accepted word orders: "waist 68 cm" (noun first) and "68 cm waist" (number
                   first). People write both, so both resolve to the same field. A noun that has
                   already been spent by an earlier number is not looked at again. */
                const std::string prev = (i > 0 && !consumed[i - 1]) ? words[i - 1].text : "";
                const std::string after = (i + 2 < words.size()) ? words[i + 2].text : "";
                if (const char* field = setBodyCm(spec, prev, value)) {
                    record(out, w + " cm", field, &matched);
                    i += 1;
                    continue;
                }
                if (const char* field = setBodyCm(spec, after, value)) {
                    consumed[i + 2] = true;
                    record(out, w + " cm " + after, field, &matched);
                    i += 2;
                    continue;
                }
                /* A bare "<n> cm" is a height. */
                spec->height_m = value * 0.01f;
                record(out, w + " cm", "height_m", &matched);
                i += 1;
                continue;
            }
            if (next == "m" || next == "metre" || next == "metres" || next == "meter" || next == "meters") {
                if (value > 1.0f && value < 2.4f) {
                    spec->height_m = value;
                    record(out, w + " m", "height_m", &matched);
                }
                i += 1;
                continue;
            }
            if (next == "ft" || next == "foot" || next == "feet") {
                const float feet = value;
                float inches = 0.0f;
                if (i + 3 < words.size() && isNumber(words[i + 2].text) &&
                    (words[i + 3].text == "in" || words[i + 3].text == "inch" || words[i + 3].text == "inches")) {
                    inches = toFloat(words[i + 2].text);
                    i += 2;
                }
                spec->height_m = feet * 0.3048f + inches * 0.0254f;
                record(out, w + " ft", "height_m", &matched);
                i += 1;
                continue;
            }
            /* "<number>-year-old" / "aged 34" */
            const std::string prev = (i > 0) ? words[i - 1].text : "";
            if (prev == "aged" || prev == "age") {
                spec->age_years = value;
                record(out, w, "age_years", &matched);
                continue;
            }
            if (value >= 16.0f && value <= 70.0f && next.rfind("year", 0) == 0) {
                spec->age_years = value;
                record(out, w + "-year-old", "age_years", &matched);
                i += 1;
                continue;
            }
            continue;
        }

        /* ---- sex ---- */
        static const char* const kMale[] = {"man", "male", "boy", "gentleman", "guy", "he", "his"};
        static const char* const kFemale[] = {"woman", "female", "girl", "lady", "she", "her"};
        if (anyOf(w, kMale, 7)) { spec->sex = AF_SEX_MALE; record(out, w, "sex", &matched); continue; }
        if (anyOf(w, kFemale, 6)) { spec->sex = AF_SEX_FEMALE; record(out, w, "sex", &matched); continue; }

        /* ---- build ---- */
        static const char* const kSlim[] = {"slim", "skinny", "thin", "lean", "slender", "petite", "lithe"};
        static const char* const kAthletic[] = {"athletic", "fit", "toned", "sporty", "swimmer"};
        static const char* const kMuscular[] = {"muscular", "buff", "ripped", "jacked", "bodybuilder", "strongman"};
        static const char* const kHeavy[] = {"heavy", "stocky", "chubby", "plump", "curvy", "overweight", "burly"};
        static const char* const kAverage[] = {"average", "normal", "medium", "regular"};
        if (anyOf(w, kSlim, 7)) { spec->build = AF_BUILD_SLIM; spec->body_fat = 0.14f; spec->muscle = 0.35f; record(out, w, "build", &matched); continue; }
        if (anyOf(w, kAthletic, 5)) { spec->build = AF_BUILD_ATHLETIC; spec->muscle = 0.60f; spec->body_fat = 0.18f; record(out, w, "build", &matched); continue; }
        if (anyOf(w, kMuscular, 6)) { spec->build = AF_BUILD_MUSCULAR; spec->muscle = 0.85f; spec->body_fat = 0.16f; record(out, w, "build", &matched); continue; }
        if (anyOf(w, kHeavy, 7)) { spec->build = AF_BUILD_HEAVY; spec->body_fat = 0.42f; record(out, w, "build", &matched); continue; }
        if (anyOf(w, kAverage, 4)) { spec->build = AF_BUILD_AVERAGE; record(out, w, "build", &matched); continue; }

        /* ---- stature adjectives ---- */
        if (w == "tall") { spec->height_m = (spec->sex == AF_SEX_FEMALE) ? 1.78f : 1.90f; record(out, w, "height_m", &matched); continue; }
        if (w == "short") { spec->height_m = (spec->sex == AF_SEX_FEMALE) ? 1.56f : 1.68f; record(out, w, "height_m", &matched); continue; }

        /* ---- age adjectives ---- */
        if (w == "young") { spec->age_years = 22.0f; record(out, w, "age_years", &matched); continue; }
        if (w == "teen" || w == "teenage") { spec->age_years = 17.0f; record(out, w, "age_years", &matched); continue; }
        if (w == "middle" && i + 1 < words.size() && words[i + 1].text.rfind("aged", 0) == 0) { spec->age_years = 45.0f; record(out, "middle-aged", "age_years", &matched); i += 1; continue; }
        if (w == "old" || w == "elderly" || w == "senior") { spec->age_years = 66.0f; record(out, w, "age_years", &matched); continue; }

        /* ---- hair ---- */
        /* Length adjectives held for the next hair noun. */
        if (w == "long" || w == "waist" || w == "flowing") { pendingStyle = AF_HAIR_LONG; continue; }
        if (w == "short") { pendingStyle = AF_HAIR_SHORT; continue; }
        if (w == "medium" || w == "shoulder-length" || w == "shoulder") { pendingStyle = AF_HAIR_MEDIUM; continue; }

        if (isHairWord(w)) {
            static const char* const kBald[] = {"bald", "shaved"};
            static const char* const kBuzz[] = {"buzz", "buzzcut"};
            static const char* const kBraids[] = {"braid", "braids", "locks", "dreadlocks"};
            if (anyOf(w, kBald, 2)) spec->hair_style = AF_HAIR_NONE;
            else if (anyOf(w, kBuzz, 2)) spec->hair_style = AF_HAIR_BUZZ;
            else if (w == "ponytail") spec->hair_style = AF_HAIR_PONYTAIL;
            else if (w == "bun") spec->hair_style = AF_HAIR_BUN;
            else if (w == "afro") spec->hair_style = AF_HAIR_AFRO;
            else if (w == "mohawk") spec->hair_style = AF_HAIR_MOHAWK;
            else if (anyOf(w, kBraids, 4)) spec->hair_style = AF_HAIR_BRAIDS;
            else if (pendingStyle >= 0) {
                spec->hair_style = pendingStyle;
            }
            /* "dark hair" is a hair colour; "dark skin" is a skin tone. The qualifier decides. */
            if (pendingMelanin >= 0.0f) {
                if (spec->hair_tone[0] == 0.0f) {
                    const float v = std::fmax(0.02f, 1.0f - pendingMelanin);
                    spec->hair_tone[0] = v; spec->hair_tone[1] = v * 0.94f; spec->hair_tone[2] = v * 0.90f;
                }
                pendingMelanin = -1.0f;
            }
            pendingStyle = -1;
            if (hasPending) {
                spec->hair_tone[0] = pendingColour[0];
                spec->hair_tone[1] = pendingColour[1];
                spec->hair_tone[2] = pendingColour[2];
                /* Very dark hair words map onto the melanin scale too, so a "black-haired"
                   avatar gets the right ear/shadow response as well as the right card colour. */
                if (pendingColour[0] < 0.05f && pendingColour[1] < 0.05f) spec->skin_melanin = std::fmax(spec->skin_melanin, 0.55f);
                hasPending = false;
            }
            record(out, w, "hair_style", &matched);
            continue;
        }
        /* "long"/"short" immediately before hair are consumed above; also accept "long hair" as
           two words in the other order. */
        if ((w == "long" || w == "shoulder-length") && i + 1 < words.size() && isHairWord(words[i + 1].text)) {
            spec->hair_style = (w == "long") ? AF_HAIR_LONG : AF_HAIR_MEDIUM;
            record(out, w + " hair", "hair_style", &matched);
            continue;
        }

        /* ---- skin / hair tone modifiers ---- */
        if (isSkinToneWord(w)) {
            static const char* const kPale[] = {"pale", "fair", "porcelain", "alabaster", "light"};
            static const char* const kOlive[] = {"olive"};
            static const char* const kTan[] = {"tan", "tanned", "bronze"};
            static const char* const kBrown[] = {"brown"};
            static const char* const kDeep[] = {"dark", "deep", "ebony"};
            float melanin = 0.35f;
            if (anyOf(w, kPale, 5)) melanin = 0.12f;
            else if (anyOf(w, kOlive, 1)) melanin = 0.38f;
            else if (anyOf(w, kTan, 3)) melanin = 0.50f;
            else if (anyOf(w, kBrown, 1)) melanin = 0.66f;
            else if (anyOf(w, kDeep, 3)) melanin = 0.88f;
            /* Applied now unless the very next word is a noun that claims it. */
            const std::string next = (i + 1 < words.size()) ? words[i + 1].text : "";
            if (isHairWord(next)) {
                pendingMelanin = melanin;      /* "dark hair" -> hair colour, not skin       */
            } else {
                spec->skin_melanin = melanin;
                record(out, w, "skin_melanin", &matched);
            }
            continue;
        }
        if (w == "skin" && pendingMelanin >= 0.0f) {
            spec->skin_melanin = pendingMelanin;
            pendingMelanin = -1.0f;
            record(out, "skin tone", "skin_melanin", &matched);
            continue;
        }

        /* ---- clothing, footwear and equipment ---- */
        static const char* const kBare[] = {"nude", "naked", "bare", "unclothed"};
        static const char* const kTshirt[] = {"tshirt", "t-shirt", "tee", "shirt"};
        static const char* const kAthleticWear[] = {"sportswear", "athletic", "gym", "running"};
        static const char* const kDress[] = {"dress", "gown"};
        static const char* const kSuit[] = {"suit", "tuxedo", "business"};
        static const char* const kJeans[] = {"jeans", "denim"};
        static const char* const kJacket[] = {"jacket", "hoodie", "sweater"};
        static const char* const kPlate[] = {"armour", "armor", "plate", "cuirass", "knight", "knightly", "breastplate"};
        static const char* const kLeather[] = {"leather", "ranger", "rogue"};
        static const char* const kSuit7[] = {"spacesuit", "space", "astronaut", "hazmat", "exosuit"};
        static const char* const kCoat[] = {"coat", "trench", "overcoat", "cloak"};
        static const char* const kBoots[] = {"boots", "boot"};
        static const char* const kSneakers[] = {"sneakers", "sneaker", "trainers", "shoes", "shoe", "runners"};
        static const char* const kHeels[] = {"heels", "stilettos", "high-heels"};
        static const char* const kSabatons[] = {"sabatons", "sabaton", "greaves"};

        if (anyOf(w, kBare, 4)) { spec->outfit = AF_OUTFIT_NONE; spec->footwear = AF_FOOTWEAR_NONE; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kAthleticWear, 4)) { spec->outfit = AF_OUTFIT_ATHLETIC; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kTshirt, 4)) { spec->outfit = AF_OUTFIT_TSHIRT; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kDress, 2)) { spec->outfit = AF_OUTFIT_DRESS; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kSuit, 3)) { spec->outfit = AF_OUTFIT_SUIT; spec->footwear = AF_FOOTWEAR_BOOT; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kJeans, 2)) { spec->outfit = AF_OUTFIT_JEANS_JACKET; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kJacket, 3)) { spec->outfit = AF_OUTFIT_JEANS_JACKET; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kLeather, 3)) { spec->outfit = AF_OUTFIT_ARMOR_LEATHER; spec->footwear = AF_FOOTWEAR_BOOT; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kPlate, 6)) {
            /* "leather armour" is leather armour: a material modifier anywhere in the prompt
               overrides the generic noun, because users write the material first. */
            spec->outfit = (leatherModifier && !plateModifier) ? AF_OUTFIT_ARMOR_LEATHER : AF_OUTFIT_ARMOR_PLATE;
            spec->footwear = (spec->outfit == AF_OUTFIT_ARMOR_LEATHER) ? AF_FOOTWEAR_BOOT : AF_FOOTWEAR_SABATON;
            record(out, w, "outfit", &matched);
            continue;
        }
        if (anyOf(w, kSuit7, 5) || (spaceModifier && (w == "suit" || w == "armour" || w == "armor"))) {
            spec->outfit = AF_OUTFIT_SPACESUIT; record(out, w, "outfit", &matched); continue;
        }
        if (anyOf(w, kCoat, 4)) { spec->outfit = AF_OUTFIT_COAT; record(out, w, "outfit", &matched); continue; }
        if (anyOf(w, kBoots, 2)) { spec->footwear = AF_FOOTWEAR_BOOT; record(out, w, "footwear", &matched); continue; }
        if (anyOf(w, kSneakers, 6)) { spec->footwear = AF_FOOTWEAR_SNEAKER; record(out, w, "footwear", &matched); continue; }
        if (anyOf(w, kHeels, 3)) { spec->footwear = AF_FOOTWEAR_HEEL; record(out, w, "footwear", &matched); continue; }
        if (anyOf(w, kSabatons, 3)) { spec->footwear = AF_FOOTWEAR_SABATON; record(out, w, "footwear", &matched); continue; }

        /* ---- colours ---- */
        if (colourFor(w, rgb)) {
            pendingColour[0] = rgb[0];
            pendingColour[1] = rgb[1];
            pendingColour[2] = rgb[2];
            hasPending = true;
            /* If the colour is followed by a noun it applies to, that is handled by the noun's
               branch on the next iteration. If it is the last word, it belongs to the garment. */
            if (i + 1 >= words.size() || (!isGarmentWord(words[i + 1].text) && !isHairWord(words[i + 1].text))) {
                if (spec->gear_tone[0] == 0.0f && (spec->outfit == AF_OUTFIT_ARMOR_PLATE ||
                                                   spec->outfit == AF_OUTFIT_ARMOR_LEATHER ||
                                                   spec->outfit == AF_OUTFIT_SPACESUIT)) {
                    spec->gear_tone[0] = pendingColour[0];
                    spec->gear_tone[1] = pendingColour[1];
                    spec->gear_tone[2] = pendingColour[2];
                    record(out, w, "gear_tone", &matched);
                } else {
                    spec->cloth_tone[0] = pendingColour[0];
                    spec->cloth_tone[1] = pendingColour[1];
                    spec->cloth_tone[2] = pendingColour[2];
                    record(out, w, "cloth_tone", &matched);
                }
                hasPending = false;
            }
            continue;
        }
        if (isGarmentWord(w) && hasPending) {
            spec->cloth_tone[0] = pendingColour[0];
            spec->cloth_tone[1] = pendingColour[1];
            spec->cloth_tone[2] = pendingColour[2];
            record(out, w, "cloth_tone", &matched);
            hasPending = false;
            continue;
        }
    }

    /*
     * Second pass: colours that belong to a noun which the first pass consumed early (the outfit
     * branches run before the colour nouns do, so "white coat" would otherwise lose its colour).
     * The rule is the same one a reader uses: a colour adjective belongs to the nearest following
     * noun, and hair and skin take their colour from the hair/skin rules above.
     */
    for (size_t i = 0; i < words.size(); ++i) {
        float rgb[3];
        if (!colourFor(words[i].text, rgb)) continue;
        for (size_t k = i + 1; k < words.size() && k <= i + 3; ++k) {
            const std::string& noun = words[k].text;
            if (isHairWord(noun) || noun == "skin" || isSkinToneWord(noun)) break;   /* not ours */
            static const char* const kFoot[] = {"boots", "boot", "sneakers", "sneaker", "shoes",
                                                "shoe", "heels", "trainers", "sabatons", "sabaton"};
            static const char* const kArmour[] = {"armour", "armor", "plate", "cuirass", "spacesuit",
                                                  "helmet", "gauntlets", "exosuit"};
            if (anyOf(noun, kFoot, 10)) {
                spec->gear_tone[0] = rgb[0]; spec->gear_tone[1] = rgb[1]; spec->gear_tone[2] = rgb[2];
                record(out, words[i].text + " " + noun, "gear_tone", &matched);
                break;
            }
            if (anyOf(noun, kArmour, 8)) {
                spec->gear_tone[0] = rgb[0]; spec->gear_tone[1] = rgb[1]; spec->gear_tone[2] = rgb[2];
                record(out, words[i].text + " " + noun, "gear_tone", &matched);
                break;
            }
            if (isGarmentWord(noun)) {
                spec->cloth_tone[0] = rgb[0]; spec->cloth_tone[1] = rgb[1]; spec->cloth_tone[2] = rgb[2];
                record(out, words[i].text + " " + noun, "cloth_tone", &matched);
                break;
            }
        }
    }

    if (matched > 0) {
        /* Keep the numeric ranges inside the ABI's documented domain: the parser is a user-facing
           input path, and a clamped spec is a usable avatar while an out-of-range one is a crash
           somewhere downstream. */
        if (spec->height_m != 0.0f) {
            if (spec->height_m < 1.40f) spec->height_m = 1.40f;
            if (spec->height_m > 2.20f) spec->height_m = 2.20f;
        }
        if (spec->age_years < 16.0f) spec->age_years = 16.0f;
        if (spec->age_years > 70.0f) spec->age_years = 70.0f;
        if (spec->body_fat < 0.05f) spec->body_fat = 0.05f;
        if (spec->body_fat > 0.60f) spec->body_fat = 0.60f;
        /* Zero the whole buffer first: snprintf stops at the NUL, so a shorter prompt parsed after
           a longer one would otherwise leave the previous prompt's tail in the struct and the same
           spec would not be byte-identical to one parsed from scratch. The ABI promises a spec is a
           value, not a view into history. */
        std::memset(spec->prompt, 0, sizeof(spec->prompt));
        std::snprintf(spec->prompt, sizeof(spec->prompt), "%s", prompt);
    }
    if (out) out->matched = matched;
    return matched;
}

bool promptLooksLikeAvatarRequest(const char* prompt, PromptParse* out) {
    if (!prompt) return false;
    PromptParse local;
    const std::vector<Word> words = tokenise(prompt);
    if (out) {
        out->words = static_cast<int>(words.size());
        out->matched = 0;
        out->tokenCount = 0;
    }
    static const char* const kAvatarWords[] = {"avatar", "character", "person", "figure", "woman",
                                               "man", "girl", "boy", "human", "body", "model"};
    for (const Word& w : words) {
        for (const char* k : kAvatarWords) {
            if (w.text == k) return true;
        }
        /* A prompt naming nothing but attributes ("athletic 1.8 m female") is still a request. */
        if (w.text == "female" || w.text == "male" || w.text == "athletic" || w.text == "slim") return true;
    }
    (void)local;
    return false;
}

} // namespace af
