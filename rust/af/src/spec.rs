// =============================================================================================
//  af::spec -- the avatar specification, in Rust
// =============================================================================================
//
//  The C `af_spec` is a flat, forwards-compatible struct (struct_size + plain fields + a
//  fixed-size prompt buffer). This module gives it a typed Rust face: enums instead of magic
//  integers, `Option<f32>` for the measurements that mean "derive this from height and build
//  when zero", and a builder that keeps the struct_size field correct.
//
//  Everything here is local: constructing a Spec performs no allocation beyond the prompt string
//  and touches no global state.
// =============================================================================================

use af_sys as sys;
use core::ffi::c_char;

use crate::error::{Error, Result};

/// Biological sex used for the anthropometric template. `Unspecified` is a real choice: the
/// kernel then fits an androgynous template rather than guessing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Sex {
    #[default]
    Unspecified = 0,
    Male = 1,
    Female = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Build {
    #[default]
    Average = 0,
    Slim = 1,
    Athletic = 2,
    Muscular = 3,
    Heavy = 4,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum HairStyle {
    #[default]
    None = 0,
    Buzz = 1,
    Short = 2,
    Medium = 3,
    Long = 4,
    Ponytail = 5,
    Bun = 6,
    Braids = 7,
    Afro = 8,
    Mohawk = 9,
}

impl HairStyle {
    /// Every style the kernel can build. The UI iterates this rather than hard-coding a list.
    pub const ALL: [HairStyle; 10] = [
        HairStyle::None,
        HairStyle::Buzz,
        HairStyle::Short,
        HairStyle::Medium,
        HairStyle::Long,
        HairStyle::Ponytail,
        HairStyle::Bun,
        HairStyle::Braids,
        HairStyle::Afro,
        HairStyle::Mohawk,
    ];

    pub fn as_str(self) -> &'static str {
        match self {
            HairStyle::None => "none",
            HairStyle::Buzz => "buzz",
            HairStyle::Short => "short",
            HairStyle::Medium => "medium",
            HairStyle::Long => "long",
            HairStyle::Ponytail => "ponytail",
            HairStyle::Bun => "bun",
            HairStyle::Braids => "braids",
            HairStyle::Afro => "afro",
            HairStyle::Mohawk => "mohawk",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Outfit {
    None = 0,
    #[default]
    TShirt = 1,
    Dress = 2,
    Suit = 3,
    JeansJacket = 4,
    ArmorPlate = 5,
    ArmorLeather = 6,
    SpaceSuit = 7,
    Athletic = 8,
    Coat = 9,
}

impl Outfit {
    pub const ALL: [Outfit; 10] = [
        Outfit::None,
        Outfit::TShirt,
        Outfit::Dress,
        Outfit::Suit,
        Outfit::JeansJacket,
        Outfit::ArmorPlate,
        Outfit::ArmorLeather,
        Outfit::SpaceSuit,
        Outfit::Athletic,
        Outfit::Coat,
    ];

    pub fn as_str(self) -> &'static str {
        match self {
            Outfit::None => "none",
            Outfit::TShirt => "t-shirt",
            Outfit::Dress => "dress",
            Outfit::Suit => "suit",
            Outfit::JeansJacket => "jeans and jacket",
            Outfit::ArmorPlate => "plate armour",
            Outfit::ArmorLeather => "leather armour",
            Outfit::SpaceSuit => "spacesuit",
            Outfit::Athletic => "athletic wear",
            Outfit::Coat => "coat",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Footwear {
    #[default]
    None = 0,
    Sneaker = 1,
    Boot = 2,
    Sabaton = 3,
    Heel = 4,
}

impl Footwear {
    pub const ALL: [Footwear; 5] =
        [Footwear::None, Footwear::Sneaker, Footwear::Boot, Footwear::Sabaton, Footwear::Heel];

    pub fn as_str(self) -> &'static str {
        match self {
            Footwear::None => "barefoot",
            Footwear::Sneaker => "sneakers",
            Footwear::Boot => "boots",
            Footwear::Sabaton => "sabatons",
            Footwear::Heel => "heels",
        }
    }
}

/// An avatar specification. Build one with `Spec::new()` and the chained setters, or from a text
/// prompt with `Engine::analyze_prompt`.
#[derive(Clone)]
pub struct Spec {
    pub(crate) raw: sys::af_spec,
    prompt: String,
}

impl core::fmt::Debug for Spec {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Spec")
            .field("sex", &self.sex_value())
            .field("height_m", &self.raw.height_m)
            .field("age_years", &self.raw.age_years)
            .field("muscle", &self.raw.muscle)
            .field("body_fat", &self.raw.body_fat)
            .field("hair_style", &self.raw.hair_style)
            .field("outfit", &self.raw.outfit)
            .field("footwear", &self.raw.footwear)
            .field("seed", &self.raw.seed)
            .field("prompt", &self.prompt)
            .finish()
    }
}

impl Default for Spec {
    fn default() -> Self {
        Self::new()
    }
}

impl Spec {
    /// A fully initialised spec: kernel defaults (1.75 m, average build, no hair, barefoot) with
    /// `struct_size` already correct.
    pub fn new() -> Self {
        let mut raw: sys::af_spec = unsafe { core::mem::zeroed() };
        unsafe { sys::af_spec_init(&mut raw) };
        Self { raw, prompt: String::new() }
    }

    pub fn sex(mut self, value: Sex) -> Self {
        self.raw.sex = value as i32;
        self
    }

    pub fn build(mut self, value: Build) -> Self {
        self.raw.build = value as i32;
        self
    }

    pub fn hair(mut self, value: HairStyle) -> Self {
        self.raw.hair_style = value as i32;
        self
    }

    pub fn outfit(mut self, value: Outfit) -> Self {
        self.raw.outfit = value as i32;
        self
    }

    pub fn footwear(mut self, value: Footwear) -> Self {
        self.raw.footwear = value as i32;
        self
    }

    /// Deterministic seed: identical spec + identical seed produce byte-identical geometry.
    pub fn seed(mut self, value: i32) -> Self {
        self.raw.seed = value;
        self
    }

    /// Height in metres. The kernel clamps to a physiologically plausible 1.40..2.20 m.
    pub fn height_m(mut self, value: f32) -> Self {
        self.raw.height_m = value;
        self
    }

    pub fn age_years(mut self, value: f32) -> Self {
        self.raw.age_years = value;
        self
    }

    /// 0..1 lean mass index.
    pub fn muscle(mut self, value: f32) -> Self {
        self.raw.muscle = value.clamp(0.0, 1.0);
        self
    }

    /// 0..1 body fat (0.10 shredded .. 0.45 heavy).
    pub fn body_fat(mut self, value: f32) -> Self {
        self.raw.body_fat = value.clamp(0.0, 0.9);
        self
    }

    pub fn bust(mut self, value: f32) -> Self {
        self.raw.bust = value.clamp(0.0, 1.0);
        self
    }

    /// Tape measurements in centimetres. Any value supplied is honoured exactly (the kernel
    /// preserves the perimeter to better than 0.5 %); leave them at 0 to derive from height and
    /// build. `set_measurements` takes them all at once so a UI form cannot half-apply.
    pub fn set_measurements(
        mut self,
        neck_cm: f32,
        chest_cm: f32,
        underbust_cm: f32,
        waist_cm: f32,
        hip_cm: f32,
        thigh_cm: f32,
        calf_cm: f32,
        bicep_cm: f32,
        forearm_cm: f32,
        wrist_cm: f32,
        shoulder_width_cm: f32,
        inseam_cm: f32,
        head_circumference_cm: f32,
    ) -> Self {
        self.raw.neck_cm = neck_cm;
        self.raw.chest_cm = chest_cm;
        self.raw.underbust_cm = underbust_cm;
        self.raw.waist_cm = waist_cm;
        self.raw.hip_cm = hip_cm;
        self.raw.thigh_cm = thigh_cm;
        self.raw.calf_cm = calf_cm;
        self.raw.bicep_cm = bicep_cm;
        self.raw.forearm_cm = forearm_cm;
        self.raw.wrist_cm = wrist_cm;
        self.raw.shoulder_width_cm = shoulder_width_cm;
        self.raw.inseam_cm = inseam_cm;
        self.raw.head_circumference_cm = head_circumference_cm;
        self
    }

    /// Linear-sRGB skin reflectance. `melanin` 0..1 modulates it the way UV exposure does.
    pub fn skin(mut self, tone: [f32; 3], melanin: f32) -> Self {
        self.raw.skin_tone = tone;
        self.raw.skin_melanin = melanin.clamp(0.0, 1.0);
        self
    }

    pub fn hair_tone(mut self, tone: [f32; 3]) -> Self {
        self.raw.hair_tone = tone;
        self
    }

    pub fn cloth_tone(mut self, tone: [f32; 3]) -> Self {
        self.raw.cloth_tone = tone;
        self
    }

    pub fn gear_tone(mut self, tone: [f32; 3]) -> Self {
        self.raw.gear_tone = tone;
        self
    }

    /// Override the garment roughness, or `None` for the material default.
    pub fn cloth_roughness(mut self, value: Option<f32>) -> Self {
        self.raw.cloth_roughness = value.unwrap_or(-1.0);
        self
    }

    /// Override armour/equipment metalness, or `None` for the material default.
    pub fn gear_metalness(mut self, value: Option<f32>) -> Self {
        self.raw.gear_metalness = value.unwrap_or(-1.0);
        self
    }

    /// Records the prompt this spec came from. Stored (truncated to 511 bytes, UTF-8 boundary
    /// safe) in the export's `extras` block, so an exported file can explain itself later.
    pub fn prompt(mut self, text: impl AsRef<str>) -> Self {
        let text = text.as_ref();
        self.prompt = text.to_string();
        let bytes = text.as_bytes();
        // Never cut a multi-byte character in half: if the limit falls inside one, walk back to
        // the previous character boundary. (When nothing is truncated there is no boundary to
        // repair, which is why the check is guarded by `end < bytes.len()`.)
        let mut end = bytes.len().min(sys::AF_PROMPT_BYTES - 1);
        if end < bytes.len() {
            while end > 0 && (bytes[end] & 0xC0) == 0x80 {
                end -= 1;
            }
        }
        for (i, byte) in bytes[..end].iter().enumerate() {
            self.raw.prompt[i] = *byte as c_char;
        }
        self.raw.prompt[end] = 0;
        self
    }

    pub fn prompt_text(&self) -> &str {
        &self.prompt
    }

    pub fn sex_value(&self) -> Sex {
        match self.raw.sex {
            1 => Sex::Male,
            2 => Sex::Female,
            _ => Sex::Unspecified,
        }
    }

    pub fn height_meters(&self) -> f32 {
        self.raw.height_m
    }

    /// Hands the C struct to a caller that must keep it alive for the duration of a call.
    pub(crate) fn as_raw(&self) -> &sys::af_spec {
        &self.raw
    }

    /// Adopts a spec produced by `af_engine_analyze_prompt`.
    pub(crate) fn from_raw(raw: sys::af_spec) -> Self {
        let mut spec = Self { raw, prompt: String::new() };
        let bytes: Vec<u8> = raw
            .prompt
            .iter()
            .take_while(|c| **c != 0)
            .map(|c| *c as u8)
            .collect();
        spec.prompt = String::from_utf8_lossy(&bytes).into_owned();
        spec
    }

    pub(crate) fn validate(&self) -> Result<()> {
        if self.raw.struct_size != core::mem::size_of::<sys::af_spec>() as i32 {
            return Err(Error::InvalidArgument(format!(
                "spec.struct_size is {} but this build's af_spec is {} bytes",
                self.raw.struct_size,
                core::mem::size_of::<sys::af_spec>()
            )));
        }
        if self.raw.outfit == sys::AF_OUTFIT_NONE {
            const MARKERS: &[&str] = &[
                "boy", "child", "girl", "kid", "minor", "schoolboy", "schoolgirl", "teen",
                "teenage", "underage",
            ];
            let contains_minor_marker = self
                .prompt
                .split(|c: char| !c.is_ascii_alphanumeric())
                .filter(|token| !token.is_empty())
                .any(|token| MARKERS.iter().any(|marker| token.eq_ignore_ascii_case(marker)));
            if contains_minor_marker {
                return Err(Error::InvalidArgument(
                    "nude/anatomical generation rejects minor or teen prompt terms".into(),
                ));
            }
            if !self.raw.age_years.is_finite() || self.raw.age_years < 18.0 {
                return Err(Error::InvalidArgument(
                    "nude/anatomical generation requires age_years >= 18".into(),
                ));
            }
        }
        Ok(())
    }
}
