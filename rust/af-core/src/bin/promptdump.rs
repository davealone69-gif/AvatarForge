// ==============================================================================================
//  af-core :: promptdump  --  the Rust half of the prompt-parity diff
// ==============================================================================================
//
//  Prints the Rust parser's output for tools/prompt_corpus.txt in exactly the format
//  tools/prompt_probe.cpp prints the C++ reference's output, so `diff` is the whole test:
//
//      cargo run --release --offline -p af-core --bin promptdump -- ../tools/prompt_corpus.txt
//
//  Field order and printf conversions are copied from the probe on purpose - a format difference
//  would show up as a false mismatch.
// ==============================================================================================

use afcore_rust::{parse_prompt, AfSpec, PromptParse};
use std::fs;

/// Both sides print integers scaled by 1e5: C's %g and Rust's float formatting do not agree on
/// insignificant digits, and a parity diff must fail only on real behaviour differences.
fn q(v: f32) -> i64 {
    (v as f64 * 100000.0).round() as i64
}

fn main() {
    let path = std::env::args().nth(1).unwrap_or_else(|| "../tools/prompt_corpus.txt".into());
    let text = match fs::read_to_string(&path) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("cannot open {}: {}", path, e);
            std::process::exit(1);
        }
    };
    for line in text.lines() {
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut spec = AfSpec::defaulted();
        let mut out = PromptParse::default();
        let n = parse_prompt(line, &mut spec, Some(&mut out));
        print!(
            "ok={} words={} tokens={} | sex={} build={} hair={} outfit={} foot={} age={} \
height={} muscle={} fat={} waist={} hip={} chest={} neck={} \
thigh={} calf={} bicep={} shoulder={} inseam={} head={} melanin={} \
hair_tone={},{},{} cloth={},{},{} gear={},{},{} |",
            n, out.words, out.token_count, spec.sex, spec.build, spec.hair_style, spec.outfit,
            spec.footwear, q(spec.age_years), q(spec.height_m), q(spec.muscle), q(spec.body_fat),
            q(spec.waist_cm), q(spec.hip_cm), q(spec.chest_cm), q(spec.neck_cm), q(spec.thigh_cm),
            q(spec.calf_cm), q(spec.bicep_cm), q(spec.shoulder_width_cm), q(spec.inseam_cm),
            q(spec.head_circumference_cm), q(spec.skin_melanin), q(spec.hair_tone[0]),
            q(spec.hair_tone[1]), q(spec.hair_tone[2]), q(spec.cloth_tone[0]),
            q(spec.cloth_tone[1]), q(spec.cloth_tone[2]), q(spec.gear_tone[0]),
            q(spec.gear_tone[1]), q(spec.gear_tone[2])
        );
        for i in 0..out.token_count as usize {
            print!(" \"{}\"->{},", out.tokens[i].text_str(), out.tokens[i].field_str());
        }
        println!();
    }
}
