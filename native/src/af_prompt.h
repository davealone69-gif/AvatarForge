/*
 * af_prompt.h -- text prompt -> af_spec interpreter.
 *
 * There is no language model in this engine, and there is not going to be one: a multi-gigabyte
 * model cannot ship inside an APK, cannot run offline on a mid-range phone inside the time budget,
 * and would make the output non-deterministic. What a prompt actually carries, for avatar
 * generation, is a small closed set of attributes - sex, stature, build, age, hair, clothing, skin
 * tone, colours, and a handful of tape measurements. Those are parsed deterministically here, from
 * a curated vocabulary, with the matched tokens reported back so the UI can show the user exactly
 * what was understood ("1.72 m", "athletic", "long hair", "leather armour") instead of pretending
 * to have understood prose it did not.
 *
 * Everything the parser does not recognise is left at its default and reported as unmatched. A
 * prompt of pure noise therefore produces the default avatar and an honest "0 attributes
 * recognised" - never a silently invented identity.
 */
#ifndef AF_PROMPT_H
#define AF_PROMPT_H

#include "../include/afcore.h"

namespace af {

struct PromptToken {
    char text[24];     /* the matched phrase, lower-cased                                    */
    char field[24];    /* which spec field it set ("height_m", "hair_style", ...)            */
};

struct PromptParse {
    PromptToken tokens[32];
    int tokenCount = 0;
    int matched = 0;      /* number of spec fields the prompt set                            */
    int words = 0;        /* words seen, so the UI can show "7 of 9 attributes matched"      */
};

/**
 * Parses `prompt` into `spec` (in place; unset fields keep whatever `spec` already had).
 * `out` may be null. Returns the number of attributes recognised (>= 0).
 */
int parsePrompt(const char* prompt, af_spec* spec, PromptParse* out);

/** True when the prompt is recognised as a request for an avatar at all (used to reject gibberish). */
bool promptLooksLikeAvatarRequest(const char* prompt, PromptParse* out);

} // namespace af
#endif /* AF_PROMPT_H */
