// ==============================================================================================
//  af-core :: spec.rs  --  defaults and validation for af_spec
// ==============================================================================================
//
//  `af_spec_init` is small but load-bearing: every caller in the project (Kotlin's AvatarSpec,
//  the Rust wrapper, the probes, the harness) starts from it, and the "0 means derive it" rule is
//  what lets a caller set only the two or three fields a UI exposes. The defaults here are checked
//  against the C++ kernel by tools/abi_conformance.py, field by field.
// ==============================================================================================

use crate::abi::*;
use core::ffi::c_char;

impl AfSpec {
    /// The documented defaults, identical to the C++ `af_spec_init`.
    pub fn defaulted() -> Self {
        AfSpec {
            struct_size: core::mem::size_of::<AfSpec>() as i32,
            sex: AF_SEX_UNSPECIFIED,
            build: AF_BUILD_AVERAGE,
            hair_style: AF_HAIR_MEDIUM,
            outfit: AF_OUTFIT_NONE,
            footwear: AF_FOOTWEAR_NONE,
            seed: 1337,
            height_m: 0.0, /* 0 => sex-specific default stature */
            age_years: 30.0,
            muscle: 0.45,
            body_fat: 0.22,
            bust: 0.45,
            neck_cm: 0.0,
            chest_cm: 0.0,
            underbust_cm: 0.0,
            waist_cm: 0.0,
            hip_cm: 0.0,
            thigh_cm: 0.0,
            calf_cm: 0.0,
            bicep_cm: 0.0,
            forearm_cm: 0.0,
            wrist_cm: 0.0,
            shoulder_width_cm: 0.0,
            inseam_cm: 0.0,
            head_circumference_cm: 0.0,
            skin_tone: [0.0; 3],
            skin_melanin: 0.35,
            hair_tone: [0.0; 3],
            cloth_tone: [0.0; 3],
            gear_tone: [0.0; 3],
            cloth_roughness: -1.0,
            gear_metalness: -1.0,
            prompt: [0; AF_PROMPT_BYTES],
        }
    }

    /// Rejects values a caller could only have set by mistake, instead of quietly clamping them
    /// into a body nobody asked for. Returns the offending field name.
    ///
    /// The ranges are the ones documented in afcore.h; `0` is always accepted for "derive it".
    pub fn validate(&self) -> Result<(), &'static str> {
        if self.struct_size != 0 && self.struct_size as usize != core::mem::size_of::<AfSpec>() {
            return Err("struct_size");
        }
        if !(0..=4).contains(&self.build) {
            return Err("build");
        }
        if !(AF_SEX_UNSPECIFIED..=AF_SEX_FEMALE).contains(&self.sex) {
            return Err("sex");
        }
        if !(0..=AF_HAIR_MOHAWK).contains(&self.hair_style) {
            return Err("hair_style");
        }
        if !(0..=AF_OUTFIT_COAT).contains(&self.outfit) {
            return Err("outfit");
        }
        if !(0..=AF_FOOTWEAR_HEEL).contains(&self.footwear) {
            return Err("footwear");
        }
        if self.height_m < 0.0 || (self.height_m > 0.0 && !(1.30..=2.25).contains(&self.height_m)) {
            return Err("height_m");
        }
        if self.age_years < 0.0 || (self.age_years > 0.0 && !(10.0..=100.0).contains(&self.age_years)) {
            return Err("age_years");
        }
        for (v, name) in [
            (self.muscle, "muscle"),
            (self.body_fat, "body_fat"),
            (self.bust, "bust"),
            (self.skin_melanin, "skin_melanin"),
        ] {
            if !(0.0..=1.0).contains(&v) {
                return Err(name);
            }
        }
        for (v, name) in [
            (self.neck_cm, "neck_cm"),
            (self.chest_cm, "chest_cm"),
            (self.underbust_cm, "underbust_cm"),
            (self.waist_cm, "waist_cm"),
            (self.hip_cm, "hip_cm"),
            (self.thigh_cm, "thigh_cm"),
            (self.calf_cm, "calf_cm"),
            (self.bicep_cm, "bicep_cm"),
            (self.forearm_cm, "forearm_cm"),
            (self.wrist_cm, "wrist_cm"),
            (self.shoulder_width_cm, "shoulder_width_cm"),
            (self.inseam_cm, "inseam_cm"),
            (self.head_circumference_cm, "head_circumference_cm"),
        ] {
            if v < 0.0 || v > 250.0 {
                return Err(name);
            }
        }
        Ok(())
    }

    /// The prompt as a `&str`, for the parsers and for the GLB's metadata block. A spec that the
    /// caller filled by hand may not be NUL-terminated, so the whole 512-byte field is scanned.
    pub fn prompt_str(&self) -> &str {
        let bytes: &[u8] = unsafe { core::slice::from_raw_parts(self.prompt.as_ptr() as *const u8, AF_PROMPT_BYTES) };
        let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
        core::str::from_utf8(&bytes[..end]).unwrap_or("")
    }

    /// Sets the prompt from a `&str`, truncating at a character boundary that fits the field.
    pub fn set_prompt(&mut self, text: &str) {
        let mut n = text.len().min(AF_PROMPT_BYTES - 1);
        while n > 0 && !text.is_char_boundary(n) {
            n -= 1;
        }
        for (dst, src) in self.prompt.iter_mut().zip(text.as_bytes()[..n].iter()) {
            *dst = *src as c_char;
        }
        for dst in self.prompt.iter_mut().skip(n) {
            *dst = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_match_the_documented_values() {
        let s = AfSpec::defaulted();
        assert_eq!(s.struct_size as usize, core::mem::size_of::<AfSpec>());
        assert_eq!(s.seed, 1337);
        assert_eq!(s.muscle, 0.45);
        assert_eq!(s.body_fat, 0.22);
        assert_eq!(s.cloth_roughness, -1.0);
        assert_eq!(s.prompt_str(), "");
    }

    #[test]
    fn validation_rejects_nonsense_but_accepts_derived_zeros() {
        let mut s = AfSpec::defaulted();
        assert!(s.validate().is_ok());
        s.height_m = 0.0;                       /* derive from sex */
        assert!(s.validate().is_ok());
        s.height_m = 3.1;
        assert_eq!(s.validate(), Err("height_m"));
        s = AfSpec::defaulted();
        s.muscle = 1.4;
        assert_eq!(s.validate(), Err("muscle"));
    }

    #[test]
    fn prompt_round_trips_and_truncates_on_a_char_boundary() {
        let mut s = AfSpec::defaulted();
        s.set_prompt("a tall woman in a green tunic");
        assert_eq!(s.prompt_str(), "a tall woman in a green tunic");

        /* 511 bytes of ASCII plus a 3-byte character that cannot fit: it is dropped whole rather
           than split, so the C side can never read half a codepoint. */
        let mut long = String::new();
        long.push_str(&"x".repeat(511));
        long.push('€');
        s.set_prompt(&long);
        assert_eq!(s.prompt_str().len(), 511);
        assert!(std::str::from_utf8(s.prompt_str().as_bytes()).is_ok());
    }
}
