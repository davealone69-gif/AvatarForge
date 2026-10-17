// ==============================================================================================
//  af-core :: prompt.rs  --  text prompt -> af_spec, ported from native/src/af_prompt.cpp
// ==============================================================================================
//
//  There is no language model in this engine and there is not going to be one: a multi-gigabyte
//  model cannot ship inside an APK, cannot run offline on a mid-range phone inside the time budget,
//  and would make the output non-deterministic. What a prompt actually carries, for avatar
//  generation, is a small closed set of attributes - sex, stature, build, age, hair, clothing, skin
//  tone, colours, and a handful of tape measurements. They are parsed deterministically from a
//  curated vocabulary, and the matched tokens are reported back so the UI can show the user exactly
//  what was understood instead of pretending to have understood prose it did not.
//
//  A prompt of pure noise therefore produces the default avatar and an honest "0 attributes
//  recognised" - never a silently invented identity.
//
//  The port keeps the C++ behaviour *exactly*, including the parts that look odd (a colour verb
//  before a noun it does not own still leaves a pending colour; "tall" is sex-dependent; "leather"
//  anywhere in the prompt overrides the armour material). `tests/parity.rs` diffs this against the
//  C++ parser over a corpus of prompts, field by field - which is why the odd parts are preserved
//  rather than "improved" in passing.
// ==============================================================================================

use crate::abi::*;

/// One recognised phrase: the text and the spec field it set. Mirrors `PromptToken`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PromptToken {
    /// The matched phrase, lower-cased, truncated to 23 bytes.
    pub text: [u8; 24],
    /// The spec field it set ("height_m", "hair_style", ...).
    pub field: [u8; 24],
}

impl PromptToken {
    pub fn text_str(&self) -> &str {
        cstr_slice(&self.text)
    }
    pub fn field_str(&self) -> &str {
        cstr_slice(&self.field)
    }
}

fn cstr_slice(buf: &[u8]) -> &str {
    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    core::str::from_utf8(&buf[..end]).unwrap_or("")
}

fn write_buf(dst: &mut [u8; 24], text: &str) {
    let bytes = text.as_bytes();
    let n = bytes.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&bytes[..n]);
    for b in dst[n..].iter_mut() {
        *b = 0;
    }
}

/// Byte spans of every token in the original prompt, for the UI's highlight view. The matched
/// tokens carry the same text as `PromptToken`, so a caller can underline exactly the words the
/// parser acted on instead of the whole sentence.
pub fn token_spans(prompt: &str) -> Vec<(usize, usize)> {
    tokenise(prompt).iter().map(|w| (w.start, w.end)).collect()
}

/// Result of a parse: how many attributes were recognised, over how many words, and what matched.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PromptParse {
    pub tokens: [PromptToken; 32],
    pub token_count: i32,
    pub matched: i32,
    pub words: i32,
}

/* Owns its text: the lower-cased word is built once per parse and dropped with the vector. No
   interning, no leaking - parsing happens once per generation, so a handful of small allocations
   is the right trade for keeping the token spans and the letters the user typed. */
struct Word {
    text: String,
    start: usize,
    end: usize,
}

fn is_word_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '.' || c == '\'' || c == '-'
}

fn tokenise(prompt: &str) -> Vec<Word> {
    /* Byte offsets are kept so a token's span can be sliced back out of the original (the C++
       stores them for the UI's highlight view). Non-ASCII bytes are separators, exactly like the
       C++ isalnum() under a "C" locale, so both tokenisers agree on every input. */
    let bytes = prompt.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < bytes.len() {
        while i < bytes.len() && !is_word_char(bytes[i] as char) {
            i += 1;
        }
        if i >= bytes.len() {
            break;
        }
        let start = i;
        while i < bytes.len() && is_word_char(bytes[i] as char) {
            i += 1;
        }
        let raw = &prompt[start..i];
        out.push(Word { text: raw.to_ascii_lowercase(), start, end: i });
    }
    out
}

fn is_number(s: &str) -> bool {
    if s.is_empty() {
        return false;
    }
    let mut digit = false;
    let mut dot = false;
    for c in s.chars() {
        if c.is_ascii_digit() {
            digit = true;
            continue;
        }
        if c == '.' && !dot {
            dot = true;
            continue;
        }
        return false;
    }
    digit
}

fn to_float(s: &str) -> f32 {
    s.parse::<f32>().unwrap_or(0.0)
}

/// Membership test that accepts either `&str` or `&String`, so call sites stay readable.
fn any_of<W: AsRef<str>>(w: W, list: &[&str]) -> bool {
    let w = w.as_ref();
    list.iter().any(|k| *k == w)
}

fn colour_for(w: &str) -> Option<[f32; 3]> {
    COLOURS.iter().find(|(name, _)| *name == w).map(|(_, rgb)| *rgb)
}

const COLOURS: &[(&str, [f32; 3])] = &[
    ("black", [0.020, 0.020, 0.022]),
    ("charcoal", [0.045, 0.046, 0.050]),
    ("white", [0.860, 0.860, 0.845]),
    ("ivory", [0.820, 0.795, 0.720]),
    ("grey", [0.240, 0.245, 0.250]),
    ("gray", [0.240, 0.245, 0.250]),
    ("silver", [0.640, 0.650, 0.660]),
    ("red", [0.420, 0.045, 0.040]),
    ("crimson", [0.330, 0.020, 0.035]),
    ("maroon", [0.140, 0.020, 0.030]),
    ("orange", [0.520, 0.180, 0.030]),
    ("yellow", [0.640, 0.520, 0.070]),
    ("gold", [0.700, 0.520, 0.140]),
    ("green", [0.075, 0.230, 0.080]),
    ("olive", [0.190, 0.200, 0.070]),
    ("teal", [0.040, 0.200, 0.190]),
    ("cyan", [0.060, 0.400, 0.450]),
    ("blue", [0.045, 0.110, 0.360]),
    ("navy", [0.020, 0.040, 0.140]),
    ("purple", [0.180, 0.060, 0.290]),
    ("violet", [0.250, 0.090, 0.400]),
    ("magenta", [0.420, 0.050, 0.280]),
    ("pink", [0.640, 0.290, 0.330]),
    ("brown", [0.160, 0.085, 0.045]),
    ("tan", [0.400, 0.270, 0.170]),
    ("beige", [0.560, 0.480, 0.370]),
    ("cream", [0.700, 0.640, 0.520]),
    ("bronze", [0.330, 0.200, 0.090]),
    ("copper", [0.450, 0.220, 0.110]),
];

const GARMENT_WORDS: &[&str] = &[
    "shirt", "tshirt", "tee", "t-shirt", "trouser", "trousers", "clothes", "clothing", "jacket",
    "jeans", "suit", "coat", "armour", "armor", "dress", "gown", "sweater", "hoodie", "uniform",
    "outfit",
];

const HAIR_WORDS: &[&str] = &[
    "hair", "braid", "braids", "ponytail", "bun", "afro", "mohawk", "locks", "dreadlocks", "bald",
    "buzz",
];

const SKIN_TONE_WORDS: &[&str] = &[
    "pale", "fair", "light", "olive", "tan", "tanned", "bronze", "brown", "dark", "deep", "ebony",
    "porcelain", "alabaster",
];

fn is_garment_word(w: &str) -> bool {
    any_of(w, GARMENT_WORDS)
}
fn is_hair_word(w: &str) -> bool {
    any_of(w, HAIR_WORDS)
}

struct Recorder<'a> {
    out: Option<&'a mut PromptParse>,
    matched: i32,
}

impl<'a> Recorder<'a> {
    fn record(&mut self, text: &str, field: &str) {
        if let Some(out) = self.out.as_deref_mut() {
            if out.token_count < 32 {
                let idx = out.token_count as usize;
                write_buf(&mut out.tokens[idx].text, text);
                write_buf(&mut out.tokens[idx].field, field);
                out.token_count += 1;
            }
        }
        self.matched += 1;
    }
}

/// Maps a body noun onto the `af_spec` field it names, writing `value` (centimetres) into it.
/// Returns the field name for the UI's "understood" list. One table, so the noun-before-number
/// and number-before-noun paths can never drift apart.
fn set_body_cm(spec: &mut AfSpec, noun: &str, value: f32) -> Option<&'static str> {
    let field = match noun {
        "waist" => {
            spec.waist_cm = value;
            "waist_cm"
        }
        "hip" | "hips" => {
            spec.hip_cm = value;
            "hip_cm"
        }
        "chest" | "bust" => {
            spec.chest_cm = value;
            "chest_cm"
        }
        "neck" => {
            spec.neck_cm = value;
            "neck_cm"
        }
        "thigh" => {
            spec.thigh_cm = value;
            "thigh_cm"
        }
        "calf" => {
            spec.calf_cm = value;
            "calf_cm"
        }
        "bicep" | "arm" => {
            spec.bicep_cm = value;
            "bicep_cm"
        }
        "shoulder" | "shoulders" => {
            spec.shoulder_width_cm = value;
            "shoulder_width_cm"
        }
        "inseam" => {
            spec.inseam_cm = value;
            "inseam_cm"
        }
        "head" => {
            spec.head_circumference_cm = value;
            "head_circumference_cm"
        }
        _ => return None,
    };
    Some(field)
}

/// The word two slots after the number ("<num> cm <noun>"), or "" when there is none.
fn next_after_units(words: &[Word], i: usize) -> &str {
    if i + 2 < words.len() {
        words[i + 2].text.as_str()
    } else {
        ""
    }
}

/// Parses `prompt` into `spec` in place; unset fields keep whatever `spec` already had.
/// Returns the number of attributes recognised.
pub fn parse_prompt(prompt: &str, spec: &mut AfSpec, mut out: Option<&mut PromptParse>) -> i32 {
    if let Some(o) = out.as_deref_mut() {
        *o = PromptParse::default();
    }
    let words = tokenise(prompt);
    if let Some(o) = out.as_deref_mut() {
        o.words = words.len() as i32;
    }
    let mut rec = Recorder { out, matched: 0 };

    let mut pending_colour = [0.0_f32; 3];
    let mut has_pending = false;
    let mut pending_style: i32 = -1;
    let mut pending_melanin: f32 = -1.0;
    /* A word can be consumed by a branch that is not the word's own noun branch: "68 cm waist"
       eats the noun "waist". A later number must not look back at a noun that is already spent,
       or "68 cm waist, 96 cm hips" would set the waist twice. */
    let mut consumed = vec![false; words.len()];
    let mut leather_modifier = false;
    let mut plate_modifier = false;
    let mut space_modifier = false;
    for w in &words {
        if w.text == "leather" {
            leather_modifier = true;
        }
        if any_of(&w.text, &["plate", "cuirass", "breastplate"]) {
            plate_modifier = true;
        }
        if any_of(&w.text, &["space", "spacesuit", "astronaut"]) {
            space_modifier = true;
        }
    }

    let mut i = 0usize;
    while i < words.len() {
        let w = words[i].text.as_str();
        let next = if i + 1 < words.len() { words[i + 1].text.as_str() } else { "" };

        /* ---- measurements ---- */
        if is_number(w) {
            let value = to_float(w);
            if any_of(next, &["cm", "centimetre", "centimetres", "centimeter", "centimeters"]) {
                /* Two accepted word orders: "waist 68 cm" (noun first) and "68 cm waist" (number
                   first). People write both, so both resolve to the same field. */
                let prev = if i > 0 && !consumed[i - 1] { words[i - 1].text.as_str() } else { "" };
                let after = next_after_units(&words, i);
                if let Some(field) = set_body_cm(spec, prev, value) {
                    rec.record(&format!("{} cm", w), field);
                    i += 2;
                    continue;
                }
                if let Some(field) = set_body_cm(spec, after, value) {
                    consumed[i + 2] = true;
                    rec.record(&format!("{} cm {}", w, after), field);
                    i += 3;
                    continue;
                }
                /* A bare "<n> cm" is a height. */
                spec.height_m = value * 0.01;
                rec.record(&format!("{} cm", w), "height_m");
                i += 2;
                continue;
            }
            if any_of(next, &["m", "metre", "metres", "meter", "meters"]) {
                if value > 1.0 && value < 2.4 {
                    spec.height_m = value;
                    rec.record(&format!("{} m", w), "height_m");
                }
                i += 2;
                continue;
            }
            if any_of(next, &["ft", "foot", "feet"]) {
                let feet = value;
                let mut inches = 0.0_f32;
                if i + 3 < words.len()
                    && is_number(words[i + 2].text.as_str())
                    && any_of(words[i + 3].text.as_str(), &["in", "inch", "inches"])
                {
                    inches = to_float(words[i + 2].text.as_str());
                    i += 2;
                }
                spec.height_m = feet * 0.3048 + inches * 0.0254;
                rec.record(&format!("{} ft", w), "height_m");
                i += 2;
                continue;
            }
            let prev = if i > 0 { words[i - 1].text.as_str() } else { "" };
            if any_of(prev, &["aged", "age"]) {
                spec.age_years = value;
                rec.record(w, "age_years");
                i += 1;
                continue;
            }
            if (16.0..=70.0).contains(&value) && next.starts_with("year") {
                spec.age_years = value;
                rec.record(&format!("{}-year-old", w), "age_years");
                i += 2;
                continue;
            }
            i += 1;
            continue;
        }

        /* ---- sex ---- */
        if any_of(w, &["man", "male", "boy", "gentleman", "guy", "he", "his"]) {
            spec.sex = AF_SEX_MALE;
            rec.record(w, "sex");
            i += 1;
            continue;
        }
        if any_of(w, &["woman", "female", "girl", "lady", "she", "her"]) {
            spec.sex = AF_SEX_FEMALE;
            rec.record(w, "sex");
            i += 1;
            continue;
        }

        /* ---- build ---- */
        if any_of(w, &["slim", "skinny", "thin", "lean", "slender", "petite", "lithe"]) {
            spec.build = AF_BUILD_SLIM;
            spec.body_fat = 0.14;
            spec.muscle = 0.35;
            rec.record(w, "build");
            i += 1;
            continue;
        }
        if any_of(w, &["athletic", "fit", "toned", "sporty", "swimmer"]) {
            spec.build = AF_BUILD_ATHLETIC;
            spec.muscle = 0.60;
            spec.body_fat = 0.18;
            rec.record(w, "build");
            i += 1;
            continue;
        }
        if any_of(w, &["muscular", "buff", "ripped", "jacked", "bodybuilder", "strongman"]) {
            spec.build = AF_BUILD_MUSCULAR;
            spec.muscle = 0.85;
            spec.body_fat = 0.16;
            rec.record(w, "build");
            i += 1;
            continue;
        }
        if any_of(w, &["heavy", "stocky", "chubby", "plump", "curvy", "overweight", "burly"]) {
            spec.build = AF_BUILD_HEAVY;
            spec.body_fat = 0.42;
            rec.record(w, "build");
            i += 1;
            continue;
        }
        if any_of(w, &["average", "normal", "medium", "regular"]) {
            spec.build = AF_BUILD_AVERAGE;
            rec.record(w, "build");
            i += 1;
            continue;
        }

        /* ---- stature adjectives (sex-dependent, as in the C++) ---- */
        if w == "tall" {
            spec.height_m = if spec.sex == AF_SEX_FEMALE { 1.78 } else { 1.90 };
            rec.record(w, "height_m");
            i += 1;
            continue;
        }
        if w == "short" {
            spec.height_m = if spec.sex == AF_SEX_FEMALE { 1.56 } else { 1.68 };
            rec.record(w, "height_m");
            i += 1;
            continue;
        }

        /* ---- age adjectives ---- */
        if any_of(w, &["young", "teen", "teenage"]) {
            spec.age_years = 19.0;
            rec.record(w, "age_years");
            i += 1;
            continue;
        }
        if w == "middle" && i + 1 < words.len() && words[i + 1].text.starts_with("aged") {
            spec.age_years = 45.0;
            rec.record("middle-aged", "age_years");
            i += 2;
            continue;
        }
        if any_of(w, &["old", "elderly", "senior"]) {
            spec.age_years = 66.0;
            rec.record(w, "age_years");
            i += 1;
            continue;
        }

        /* ---- hair: length adjectives are held for the next hair noun ---- */
        if any_of(w, &["long", "waist", "flowing"]) {
            pending_style = AF_HAIR_LONG;
            /* The C++ consumes this word without recording it, unless the very next word is a
               hair noun (handled below), so fall through with i advanced. */
            i += 1;
            continue;
        }
        if w == "short" {
            pending_style = AF_HAIR_SHORT;
            i += 1;
            continue;
        }
        if any_of(w, &["medium", "shoulder-length", "shoulder"]) {
            pending_style = AF_HAIR_MEDIUM;
            i += 1;
            continue;
        }

        if is_hair_word(w) {
            if any_of(w, &["bald", "shaved"]) {
                spec.hair_style = AF_HAIR_NONE;
            } else if any_of(w, &["buzz", "buzzcut"]) {
                spec.hair_style = AF_HAIR_BUZZ;
            } else if w == "ponytail" {
                spec.hair_style = AF_HAIR_PONYTAIL;
            } else if w == "bun" {
                spec.hair_style = AF_HAIR_BUN;
            } else if w == "afro" {
                spec.hair_style = AF_HAIR_AFRO;
            } else if w == "mohawk" {
                spec.hair_style = AF_HAIR_MOHAWK;
            } else if any_of(w, &["braid", "braids", "locks", "dreadlocks"]) {
                spec.hair_style = AF_HAIR_BRAIDS;
            } else if pending_style >= 0 {
                spec.hair_style = pending_style;
            }
            if pending_melanin >= 0.0 {
                if spec.hair_tone[0] == 0.0 {
                    let v = (1.0 - pending_melanin).max(0.02);
                    spec.hair_tone = [v, v * 0.94, v * 0.90];
                }
                pending_melanin = -1.0;
            }
            pending_style = -1;
            if has_pending {
                spec.hair_tone = pending_colour;
                /* Very dark hair words also raise the melanin floor, so a "black-haired" avatar
                   gets the right ear/shadow response as well as the right card colour. */
                if pending_colour[0] < 0.05 && pending_colour[1] < 0.05 {
                    spec.skin_melanin = spec.skin_melanin.max(0.55);
                }
                has_pending = false;
            }
            rec.record(w, "hair_style");
            i += 1;
            continue;
        }

        /* ---- skin / hair tone modifiers ---- */
        if any_of(w, &SKIN_TONE_WORDS) {
            let melanin = if any_of(w, &["pale", "fair", "porcelain", "alabaster", "light"]) {
                0.12
            } else if w == "olive" {
                0.38
            } else if any_of(w, &["tan", "tanned", "bronze"]) {
                0.50
            } else if w == "brown" {
                0.66
            } else {
                0.88 /* dark, deep, ebony */
            };
            if is_hair_word(next) {
                pending_melanin = melanin; /* "dark hair" -> hair colour, not skin */
            } else {
                spec.skin_melanin = melanin;
                rec.record(w, "skin_melanin");
            }
            i += 1;
            continue;
        }
        if w == "skin" && pending_melanin >= 0.0 {
            spec.skin_melanin = pending_melanin;
            pending_melanin = -1.0;
            rec.record("skin tone", "skin_melanin");
            i += 1;
            continue;
        }

        /* ---- clothing, footwear, equipment ---- */
        if any_of(w, &["nude", "naked", "bare", "unclothed"]) {
            spec.outfit = AF_OUTFIT_NONE;
            spec.footwear = AF_FOOTWEAR_NONE;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["sportswear", "athletic", "gym", "running"]) {
            spec.outfit = AF_OUTFIT_ATHLETIC;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["tshirt", "t-shirt", "tee", "shirt"]) {
            spec.outfit = AF_OUTFIT_TSHIRT;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["dress", "gown"]) {
            spec.outfit = AF_OUTFIT_DRESS;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["suit", "tuxedo", "business"]) {
            spec.outfit = AF_OUTFIT_SUIT;
            spec.footwear = AF_FOOTWEAR_BOOT;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["jeans", "denim"]) {
            spec.outfit = AF_OUTFIT_JEANS_JACKET;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["jacket", "hoodie", "sweater"]) {
            spec.outfit = AF_OUTFIT_JEANS_JACKET;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["leather", "ranger", "rogue"]) {
            spec.outfit = AF_OUTFIT_ARMOR_LEATHER;
            spec.footwear = AF_FOOTWEAR_BOOT;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["armour", "armor", "plate", "cuirass", "knight", "knightly", "breastplate"]) {
            /* "leather armour" is leather armour: a material modifier anywhere in the prompt
               overrides the generic noun, because users write the material first. */
            spec.outfit = if leather_modifier && !plate_modifier {
                AF_OUTFIT_ARMOR_LEATHER
            } else {
                AF_OUTFIT_ARMOR_PLATE
            };
            spec.footwear = if spec.outfit == AF_OUTFIT_ARMOR_LEATHER {
                AF_FOOTWEAR_BOOT
            } else {
                AF_FOOTWEAR_SABATON
            };
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["spacesuit", "space", "astronaut", "hazmat", "exosuit"])
            || (space_modifier && any_of(w, &["suit", "armour", "armor"]))
        {
            spec.outfit = AF_OUTFIT_SPACESUIT;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["coat", "trench", "overcoat", "cloak"]) {
            spec.outfit = AF_OUTFIT_COAT;
            rec.record(w, "outfit");
            i += 1;
            continue;
        }
        if any_of(w, &["boots", "boot"]) {
            spec.footwear = AF_FOOTWEAR_BOOT;
            rec.record(w, "footwear");
            i += 1;
            continue;
        }
        if any_of(w, &["sneakers", "sneaker", "trainers", "shoes", "shoe", "runners"]) {
            spec.footwear = AF_FOOTWEAR_SNEAKER;
            rec.record(w, "footwear");
            i += 1;
            continue;
        }
        if any_of(w, &["heels", "stilettos", "high-heels"]) {
            spec.footwear = AF_FOOTWEAR_HEEL;
            rec.record(w, "footwear");
            i += 1;
            continue;
        }
        if any_of(w, &["sabatons", "sabaton", "greaves"]) {
            spec.footwear = AF_FOOTWEAR_SABATON;
            rec.record(w, "footwear");
            i += 1;
            continue;
        }

        /* ---- colours ---- */
        if let Some(rgb) = colour_for(w) {
            pending_colour = rgb;
            has_pending = true;
            let next_is_noun = !next.is_empty() && (is_garment_word(next) || is_hair_word(next));
            if i + 1 >= words.len() || !next_is_noun {
                let armour_like = matches!(
                    spec.outfit,
                    AF_OUTFIT_ARMOR_PLATE | AF_OUTFIT_ARMOR_LEATHER | AF_OUTFIT_SPACESUIT
                );
                if spec.gear_tone[0] == 0.0 && armour_like {
                    spec.gear_tone = pending_colour;
                    rec.record(w, "gear_tone");
                } else {
                    spec.cloth_tone = pending_colour;
                    rec.record(w, "cloth_tone");
                }
                has_pending = false;
            }
            i += 1;
            continue;
        }
        if is_garment_word(w) && has_pending {
            spec.cloth_tone = pending_colour;
            rec.record(w, "cloth_tone");
            has_pending = false;
            i += 1;
            continue;
        }

        i += 1;
    }

    /*
     * Second pass: colours that belong to a noun the first pass consumed early (the outfit branches
     * run before the colour nouns do, so "white coat" would otherwise lose its colour). The rule is
     * the one a reader uses: a colour adjective belongs to the nearest following noun, and hair and
     * skin take their colour from the rules above.
     */
    for i in 0..words.len() {
        let rgb = match colour_for(words[i].text.as_str()) {
            Some(c) => c,
            None => continue,
        };
        let limit = (i + 4).min(words.len());
        for k in (i + 1)..limit {
            let noun = words[k].text.as_str();
            if is_hair_word(noun) || noun == "skin" || any_of(noun, SKIN_TONE_WORDS) {
                break; /* not ours */
            }
            let foot = ["boots", "boot", "sneakers", "sneaker", "shoes", "shoe", "heels",
                        "trainers", "sabatons", "sabaton"];
            let armour = ["armour", "armor", "plate", "cuirass", "spacesuit", "helmet",
                          "gauntlets", "exosuit"];
            if any_of(noun, &foot) || any_of(noun, &armour) {
                spec.gear_tone = rgb;
                rec.record(&format!("{} {}", words[i].text, noun), "gear_tone");
                break;
            }
            if is_garment_word(noun) {
                spec.cloth_tone = rgb;
                rec.record(&format!("{} {}", words[i].text, noun), "cloth_tone");
                break;
            }
        }
    }

    if rec.matched > 0 {
        /* Keep the numeric ranges inside the ABI's documented domain: the parser is a user-facing
           input path, and a clamped spec is a usable avatar while an out-of-range one is a crash
           somewhere downstream. */
        if spec.height_m != 0.0 {
            spec.height_m = spec.height_m.clamp(1.40, 2.20);
        }
        spec.age_years = spec.age_years.clamp(16.0, 70.0);
        spec.body_fat = spec.body_fat.clamp(0.05, 0.60);
        spec.set_prompt(prompt);
    }
    let matched = rec.matched;
    if let Some(o) = rec.out.as_deref_mut() {
        o.matched = matched;
    }
    matched
}

/// True when the prompt is recognised as a request for an avatar at all (used to reject gibberish).
pub fn looks_like_avatar_request(prompt: &str) -> bool {
    let words = tokenise(prompt);
    const AVATAR_WORDS: &[&str] = &[
        "avatar", "character", "person", "figure", "woman", "man", "girl", "boy", "human", "body",
        "model",
    ];
    for w in &words {
        if any_of(&w.text, AVATAR_WORDS) {
            return true;
        }
        /* A prompt naming nothing but attributes ("athletic 1.8 m female") is still a request. */
        if any_of(&w.text, &["female", "male", "athletic", "slim"]) {
            return true;
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(text: &str) -> (AfSpec, PromptParse, i32) {
        let mut spec = AfSpec::defaulted();
        let mut out = PromptParse::default();
        let n = parse_prompt(text, &mut spec, Some(&mut out));
        (spec, out, n)
    }

    #[test]
    fn a_rich_prompt_sets_every_documented_field() {
        let (spec, out, n) = parse(
            "a tall athletic woman in a green linen tunic, ponytail, 1.72 m, brown leather boots",
        );
        assert!(n >= 3, "recognised {} attributes", n);
        assert_eq!(spec.sex, AF_SEX_FEMALE);
        assert!(spec.height_m > 1.70 && spec.height_m < 1.80);
        assert_eq!(spec.hair_style, AF_HAIR_PONYTAIL);
        assert_eq!(spec.footwear, AF_FOOTWEAR_BOOT);
        assert!(out.words > 5);
        assert_eq!(out.matched, n);
    }

    #[test]
    fn heights_parse_in_every_supported_unit() {
        let (m, _, _) = parse("a woman 172 cm");
        assert!((m.height_m - 1.72).abs() < 1e-4);
        let (m, _, _) = parse("a man 1.83 m");
        assert!((m.height_m - 1.83).abs() < 1e-4);
        let (m, _, _) = parse("a man 6 ft 1 in");
        assert!((m.height_m - (6.0 * 0.3048 + 1.0 * 0.0254)).abs() < 1e-4);
    }

    #[test]
    fn tape_measurements_land_in_the_right_fields() {
        /* both word orders: "waist 68 cm" and "68 cm waist" */
        let (m, _, _) = parse("a female waist 68 cm, hips 96 cm");
        assert!((m.waist_cm - 68.0).abs() < 1e-4);
        assert!((m.hip_cm - 96.0).abs() < 1e-4);
        let (m, _, _) = parse("a female 68 cm waist, 96 cm hips");
        assert!((m.waist_cm - 68.0).abs() < 1e-4);
        assert!((m.hip_cm - 96.0).abs() < 1e-4);
        /* a bare "172 cm" is a height, not a waist */
        let (m, _, _) = parse("a woman 172 cm");
        assert!((m.height_m - 1.72).abs() < 1e-4);
    }

    #[test]
    fn material_words_override_the_generic_armour_noun() {
        let (m, _, _) = parse("a knight in leather armour");
        assert_eq!(m.outfit, AF_OUTFIT_ARMOR_LEATHER);
        assert_eq!(m.footwear, AF_FOOTWEAR_BOOT);
        let (m, _, _) = parse("a knight in plate armour");
        assert_eq!(m.outfit, AF_OUTFIT_ARMOR_PLATE);
        assert_eq!(m.footwear, AF_FOOTWEAR_SABATON);
    }

    #[test]
    fn colour_attaches_to_the_nearest_following_noun() {
        let (m, _, _) = parse("a woman in a red dress");
        assert_eq!(spec_rgb(&m.cloth_tone), [0.420, 0.045, 0.040]);
        let (m, _, _) = parse("a man with black hair");
        assert!(m.hair_tone[0] < 0.05);
        assert!(m.skin_melanin >= 0.55, "black hair raises the melanin floor");
        let (m, _, _) = parse("a knight in black sabatons");
        assert_eq!(spec_rgb(&m.gear_tone), [0.020, 0.020, 0.022]);
    }

    fn spec_rgb(v: &[f32; 3]) -> [f32; 3] {
        [(v[0] * 1000.0).round() / 1000.0, (v[1] * 1000.0).round() / 1000.0, (v[2] * 1000.0).round() / 1000.0]
    }

    #[test]
    fn noise_produces_the_default_avatar_and_zero_matches() {
        let mut spec = AfSpec::defaulted();
        let n = parse_prompt("qwertyuiop zxcvbnm", &mut spec, None);
        assert_eq!(n, 0);
        assert_eq!(spec.sex, AF_SEX_UNSPECIFIED);
        assert_eq!(spec.height_m, 0.0);
        assert!(spec.prompt_str().is_empty(), "no prompt is stored when nothing matched");
        assert!(!looks_like_avatar_request("qwertyuiop"));
        assert!(looks_like_avatar_request("a tall woman"));
    }

    #[test]
    fn token_spans_point_at_the_original_words() {
        let text = "a  tall   woman";
        let spans = token_spans(text);
        assert_eq!(spans.len(), 3);
        assert_eq!(&text[spans[1].0..spans[1].1], "tall");
        assert_eq!(&text[spans[2].0..spans[2].1], "woman");
    }

    #[test]
    fn tokens_name_the_field_they_set() {
        let (_, out, _) = parse("a slim woman with short hair");
        let tokens: Vec<(String, String)> = (0..out.token_count as usize)
            .map(|i| (out.tokens[i].text_str().to_string(), out.tokens[i].field_str().to_string()))
            .collect();
        assert!(tokens.iter().any(|(_, f)| f == "hair_style"), "tokens: {:?}", tokens);
        assert!(tokens.iter().any(|(_, f)| f == "build"), "tokens: {:?}", tokens);
        assert!(tokens.iter().any(|(_, f)| f == "sex"), "tokens: {:?}", tokens);
    }
}
