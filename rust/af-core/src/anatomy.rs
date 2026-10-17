// ==============================================================================================
//  af-core :: anatomy.rs  --  the anthropometric solver, ported from native/src/af_anatomy.cpp
// ==============================================================================================
//
//  This is the first numeric subsystem moved into Rust, and it is the right one to start with:
//  it is pure (spec in, metrics out, no I/O, no allocation beyond a fixed struct), it decides the
//  proportions every later stage consumes, and the C++ version is verified by the harness
//  (waist/hip/crotch exactness against tape measurements). A port of it can therefore be checked
//  *exactly*: `tests/parity.rs` compares every field against a dump from the C++ library, and the
//  two must agree to within float rounding.
//
//  Two stages, unchanged from the C++:
//    1. regression from sex/build/height to landmark heights and default girths;
//    2. constraint solve - any tape measurement the caller supplied overrides the regression and
//       is honoured exactly by inverting the ellipse-perimeter relation, so "waist 78 cm" produces
//       a body whose waist perimeter measures 78 cm.
//
//  Everything here is metres. The centimetre -> metre boundary happens in exactly one place, as
//  in the C++ (a centimetre leaking past it produces 100x geometry).
// ==============================================================================================

use crate::abi::*;

pub const K_PI: f32 = 3.141_592_653_589_793_238_46;

/// Skeletal landmark heights, metres above the sole plane.
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Landmarks {
    pub sole_y: f32,
    pub ankle_y: f32,
    pub calf_max_y: f32,
    pub knee_y: f32,
    pub mid_thigh_y: f32,
    pub crotch_y: f32,
    pub hip_y: f32,
    pub waist_y: f32,
    pub underbust_y: f32,
    pub bust_y: f32,
    pub armpit_y: f32,
    pub shoulder_y: f32,
    pub neck_base_y: f32,
    pub jaw_y: f32,
    pub chin_y: f32,
    pub eye_y: f32,
    pub crown_y: f32,
    pub arm_fold_y: f32,
    pub wrist_rest_y: f32,
}

/// An elliptical cross-section. `rz` is the front-to-back semi-axis, `centre_z` its offset.
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Segment {
    pub rx: f32,
    pub rz: f32,
    pub centre_z: f32,
}

/// Everything the body field needs to know about proportions. Field-for-field the C++ struct,
/// because the parity test compares all of it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BodyMetrics {
    pub sex: i32,
    pub build: i32,
    pub height: f32,
    pub muscle: f32,
    pub body_fat: f32,
    pub bust: f32,
    pub age: f32,
    pub seed: u32,
    pub y: Landmarks,
    pub neck: Segment,
    pub chest: Segment,
    pub bust_seg: Segment,
    pub underbust: Segment,
    pub waist: Segment,
    pub hip: Segment,
    pub head: Segment,
    pub jaw: Segment,
    pub biacromial_half: f32,
    pub deltoid_r: f32,
    pub arm_pit_half: f32,
    pub hand_length: f32,
    pub palm_width: f32,
    pub palm_thickness: f32,
    pub upper_arm_len: f32,
    pub forearm_len: f32,
    pub upper_arm_r: f32,
    pub elbow_r: f32,
    pub forearm_r: f32,
    pub wrist_r: f32,
    pub bicep_bulge: f32,
    pub tricep_bulge: f32,
    pub brachioradialis_bulge: f32,
    pub thigh_len: f32,
    pub shank_len: f32,
    pub thigh_rx: f32,
    pub thigh_rz: f32,
    pub knee_r: f32,
    pub calf_rx: f32,
    pub calf_rz: f32,
    pub ankle_rx: f32,
    pub ankle_rz: f32,
    pub quad_bulge: f32,
    pub hamstring_bulge: f32,
    pub adductor_bulge: f32,
    pub gastroc_bulge: f32,
    pub tibialis_bulge: f32,
    pub foot_length: f32,
    pub foot_width: f32,
    pub foot_heel_z: f32,
    pub foot_height: f32,
    pub toe_length: [f32; 5],
    pub toe_radius: [f32; 5],
    pub finger_length: [f32; 5],
    pub finger_radius: [f32; 5],
    pub pec_bulge: f32,
    pub ab_depth: f32,
    pub oblique_bulge: f32,
    pub glute_bulge: f32,
    pub belly_bulge: f32,
    pub trapezius_bulge: f32,
    pub lat_bulge: f32,
    pub hip_half_width: f32,
    pub leg_separation: f32,
    pub face_width: f32,
    pub face_height: f32,
    pub nose_length: f32,
    pub lip_width: f32,
    pub ear_length: f32,
    pub eye_separation: f32,
    pub eye_radius: f32,
    pub iris_radius: f32,
    pub mouth_y: f32,
    pub eye_z: f32,
    pub mouth_width: f32,
    pub bust_sep: f32,
    pub roughness_skin: f32,
    pub sss_radius: f32,
}

impl Default for BodyMetrics {
    fn default() -> Self {
        BodyMetrics {
            sex: AF_SEX_UNSPECIFIED,
            build: AF_BUILD_AVERAGE,
            height: 1.75,
            muscle: 0.45,
            body_fat: 0.22,
            bust: 0.4,
            age: 30.0,
            seed: 1337,
            y: Landmarks::default(),
            neck: Segment::default(),
            chest: Segment::default(),
            bust_seg: Segment::default(),
            underbust: Segment::default(),
            waist: Segment::default(),
            hip: Segment::default(),
            head: Segment::default(),
            jaw: Segment::default(),
            biacromial_half: 0.0,
            deltoid_r: 0.0,
            arm_pit_half: 0.0,
            hand_length: 0.0,
            palm_width: 0.0,
            palm_thickness: 0.0,
            upper_arm_len: 0.0,
            forearm_len: 0.0,
            upper_arm_r: 0.0,
            elbow_r: 0.0,
            forearm_r: 0.0,
            wrist_r: 0.0,
            bicep_bulge: 0.0,
            tricep_bulge: 0.0,
            brachioradialis_bulge: 0.0,
            thigh_len: 0.0,
            shank_len: 0.0,
            thigh_rx: 0.0,
            thigh_rz: 0.0,
            knee_r: 0.0,
            calf_rx: 0.0,
            calf_rz: 0.0,
            ankle_rx: 0.0,
            ankle_rz: 0.0,
            quad_bulge: 0.0,
            hamstring_bulge: 0.0,
            adductor_bulge: 0.0,
            gastroc_bulge: 0.0,
            tibialis_bulge: 0.0,
            foot_length: 0.0,
            foot_width: 0.0,
            foot_heel_z: 0.0,
            foot_height: 0.0,
            toe_length: [0.0; 5],
            toe_radius: [0.0; 5],
            finger_length: [0.0; 5],
            finger_radius: [0.0; 5],
            pec_bulge: 0.0,
            ab_depth: 0.0,
            oblique_bulge: 0.0,
            glute_bulge: 0.0,
            belly_bulge: 0.0,
            trapezius_bulge: 0.0,
            lat_bulge: 0.0,
            hip_half_width: 0.0,
            leg_separation: 0.0,
            face_width: 0.0,
            face_height: 0.0,
            nose_length: 0.0,
            lip_width: 0.0,
            ear_length: 0.0,
            eye_separation: 0.0,
            eye_radius: 0.0,
            iris_radius: 0.0,
            mouth_y: 0.0,
            eye_z: 0.0,
            mouth_width: 0.0,
            bust_sep: 0.0,
            roughness_skin: 0.0,
            sss_radius: 0.0,
        }
    }
}

/* ------------------------------------------------------------------ helpers ----------------- */
#[inline]
pub fn clampf(v: f32, lo: f32, hi: f32) -> f32 {
    if v < lo {
        lo
    } else if v > hi {
        hi
    } else {
        v
    }
}

#[inline]
pub fn saturate(v: f32) -> f32 {
    clampf(v, 0.0, 1.0)
}

#[inline]
pub fn lerpf(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t
}

/// Ramanujan's ellipse-perimeter approximation, identical to the C++ `ellipsePerimeter`.
pub fn ellipse_perimeter(rx: f32, rz: f32) -> f32 {
    let a = if rx > 1e-5 { rx } else { 1e-5 };
    let b = if rz > 1e-5 { rz } else { 1e-5 };
    let h = (a - b) * (a - b) / ((a + b) * (a + b));
    let p = 1.0 + 3.0 * h / (10.0 + (4.0 - 3.0 * h).sqrt());
    K_PI * (a + b) * p
}

/// Inverts the perimeter relation by bisection, exactly as the C++ does (40 rounds). Bisection is
/// used because the relation is monotonic and cheap; the harness asserts the round trip is better
/// than 0.05%.
pub fn ellipse_from_circumference(circumference: f32, depth_over_width: f32) -> (f32, f32) {
    let mut lo = 1e-4_f32;
    let mut hi = if circumference > 1e-3 { circumference } else { 1e-3 };
    for _ in 0..40 {
        let mid = 0.5 * (lo + hi);
        if ellipse_perimeter(mid, mid * depth_over_width) < circumference {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    let rx = 0.5 * (lo + hi);
    (rx, rx * depth_over_width)
}

/* ------------------------------------------------------------------ regression tables ------- */
#[derive(Clone, Copy)]
struct CircTable {
    neck: f32,
    chest: f32,
    underbust: f32,
    waist: f32,
    hip: f32,
    thigh: f32,
    calf: f32,
    bicep: f32,
    forearm: f32,
    wrist: f32,
}

const MALE_AVERAGE: CircTable = CircTable { neck: 37.8, chest: 102.3, underbust: 92.0, waist: 88.6, hip: 100.4, thigh: 57.4, calf: 38.1, bicep: 33.5, forearm: 28.3, wrist: 17.2 };
const MALE_SLIM: CircTable = CircTable { neck: 34.6, chest: 89.0, underbust: 80.0, waist: 74.5, hip: 91.0, thigh: 50.5, calf: 34.5, bicep: 27.5, forearm: 24.5, wrist: 16.2 };
const MALE_ATHLETIC: CircTable = CircTable { neck: 37.0, chest: 97.0, underbust: 87.0, waist: 78.0, hip: 95.0, thigh: 55.0, calf: 37.0, bicep: 35.0, forearm: 28.0, wrist: 17.0 };
const MALE_MUSCULAR: CircTable = CircTable { neck: 40.5, chest: 112.0, underbust: 100.0, waist: 86.0, hip: 105.0, thigh: 63.0, calf: 40.0, bicep: 41.0, forearm: 31.0, wrist: 18.0 };
const MALE_HEAVY: CircTable = CircTable { neck: 42.0, chest: 115.0, underbust: 105.0, waist: 103.5, hip: 110.0, thigh: 65.5, calf: 40.5, bicep: 38.5, forearm: 30.5, wrist: 18.5 };
const FEMALE_AVERAGE: CircTable = CircTable { neck: 32.0, chest: 93.5, underbust: 76.0, waist: 79.8, hip: 99.5, thigh: 57.5, calf: 35.5, bicep: 28.3, forearm: 24.0, wrist: 15.4 };
const FEMALE_SLIM: CircTable = CircTable { neck: 30.0, chest: 82.0, underbust: 68.0, waist: 64.5, hip: 89.0, thigh: 49.0, calf: 31.0, bicep: 23.5, forearm: 20.5, wrist: 14.4 };
const FEMALE_ATHLETIC: CircTable = CircTable { neck: 31.5, chest: 88.0, underbust: 72.0, waist: 68.0, hip: 93.5, thigh: 54.0, calf: 34.0, bicep: 27.0, forearm: 23.0, wrist: 15.0 };
const FEMALE_MUSCULAR: CircTable = CircTable { neck: 34.5, chest: 100.0, underbust: 82.0, waist: 76.0, hip: 103.0, thigh: 61.0, calf: 37.0, bicep: 32.0, forearm: 26.0, wrist: 16.2 };
const FEMALE_HEAVY: CircTable = CircTable { neck: 35.5, chest: 106.0, underbust: 92.0, waist: 94.0, hip: 112.0, thigh: 65.0, calf: 39.5, bicep: 32.0, forearm: 26.5, wrist: 16.6 };

fn defaults_for(sex: i32, build: i32) -> CircTable {
    let female = sex == AF_SEX_FEMALE;
    match build {
        AF_BUILD_SLIM => if female { FEMALE_SLIM } else { MALE_SLIM },
        AF_BUILD_ATHLETIC => if female { FEMALE_ATHLETIC } else { MALE_ATHLETIC },
        AF_BUILD_MUSCULAR => if female { FEMALE_MUSCULAR } else { MALE_MUSCULAR },
        AF_BUILD_HEAVY => if female { FEMALE_HEAVY } else { MALE_HEAVY },
        _ => if female { FEMALE_AVERAGE } else { MALE_AVERAGE },
    }
}

const K_DEPTH_NECK: f32 = 0.95;
const K_DEPTH_CHEST: f32 = 0.72;
const K_DEPTH_BUST: f32 = 0.74;
const K_DEPTH_WAIST: f32 = 0.68;
const K_DEPTH_HIP: f32 = 0.80;
/* Thigh and calf depth ratios are consumed by the body field, not by this solver: the solver
   stores the *radii* (thigh_rx/thigh_rz) and the field uses these to shape the limb profile.
   Kept here, next to the other boundary constants, so the port of af_field.cpp has one place to
   take them from. */
#[allow(dead_code)]
const K_DEPTH_THIGH: f32 = 0.95;
#[allow(dead_code)]
const K_DEPTH_CALF: f32 = 0.90;
const K_DEPTH_ANKLE: f32 = 0.62;

#[inline]
fn circum_to_radius(circumference_m: f32, depth_ratio: f32) -> f32 {
    if circumference_m <= 0.01 {
        let c = if circumference_m > 0.01 { circumference_m } else { 0.01 };
        return c / (2.0 * K_PI);
    }
    let (rx, rz) = ellipse_from_circumference(circumference_m, depth_ratio);
    0.5 * (rx + rz)
}

fn solve_seg(circumference_m: f32, depth_ratio: f32, centre_z: f32) -> Segment {
    let (rx, rz) = ellipse_from_circumference(circumference_m, depth_ratio);
    Segment { rx, rz, centre_z }
}

/* ------------------------------------------------------------------ the solver --------------- */
pub fn solve(spec: &AfSpec) -> BodyMetrics {
    let mut m = BodyMetrics::default();
    m.sex = spec.sex;
    m.build = spec.build;
    m.muscle = saturate(if spec.muscle > 0.0 { spec.muscle } else { 0.45 });
    m.body_fat = saturate(if spec.body_fat > 0.0 { spec.body_fat } else { 0.22 });
    m.bust = saturate(if spec.bust >= 0.0 { spec.bust } else { 0.4 });
    m.age = clampf(if spec.age_years > 0.0 { spec.age_years } else { 30.0 }, 16.0, 70.0);
    m.seed = if spec.seed > 0 { spec.seed as u32 } else { 1337 };

    let default_height = if m.sex == AF_SEX_FEMALE { 1.63 } else { 1.756 };
    m.height = clampf(if spec.height_m > 0.0 { spec.height_m } else { default_height }, 1.30, 2.25);
    let h = m.height;

    /* Landmark heights: Drillis & Contini stature fractions, with the two sexual-dimorphism
       corrections the C++ applies (longer female torso, narrower shoulders, wider pelvis). */
    let female = m.sex == AF_SEX_FEMALE;
    let torso_bias = if female { 0.008 } else { 0.0 };
    let shoulder_bias = if female { -0.014 } else { 0.0 };
    let pelvis_bias = if female { 0.010 } else { 0.0 };
    m.y.sole_y = 0.0;
    m.y.ankle_y = 0.039 * h;
    m.y.knee_y = (0.285 + if female { 0.002 } else { 0.0 }) * h;
    m.y.crotch_y = (0.480 - torso_bias) * h;
    m.y.hip_y = (0.530 - torso_bias + pelvis_bias) * h;
    m.y.waist_y = (0.615 - torso_bias) * h;
    m.y.underbust_y = (0.660 - torso_bias) * h;
    m.y.bust_y = (0.720 - torso_bias) * h;
    m.y.armpit_y = (0.752 - torso_bias) * h;
    m.y.shoulder_y = (0.818 - torso_bias + shoulder_bias) * h;
    m.y.neck_base_y = (0.855 - torso_bias + shoulder_bias * 0.5) * h;
    m.y.jaw_y = (0.893 - torso_bias * 0.5) * h;
    m.y.chin_y = (0.870 - torso_bias * 0.5) * h;
    m.y.eye_y = (0.936 - torso_bias * 0.25) * h;
    m.y.crown_y = h;
    m.y.calf_max_y = lerpf(m.y.ankle_y, m.y.knee_y, 0.34);
    m.y.mid_thigh_y = lerpf(m.y.crotch_y, m.y.knee_y, 0.55);

    m.thigh_len = m.y.hip_y - m.y.knee_y;
    m.shank_len = m.y.knee_y - m.y.ankle_y;
    m.upper_arm_len = 0.186 * h;
    m.forearm_len = 0.146 * h;
    m.hand_length = 0.108 * h;
    m.foot_length = 0.152 * h;
    m.foot_width = 0.055 * h;

    /* Inseam, when given, pins the crotch height and re-partitions the leg. */
    if spec.inseam_cm > 1.0 {
        let inseam = spec.inseam_cm / 100.0;
        let ratio = clampf(inseam / m.y.crotch_y.max(1e-3), 0.80, 1.20);
        m.y.crotch_y = inseam;
        m.y.hip_y = inseam + 0.050 * h * ratio;
        m.y.waist_y = inseam + 0.135 * h * ratio;
        m.y.underbust_y = inseam + 0.180 * h * ratio;
        m.y.bust_y = inseam + 0.240 * h * ratio;
        m.y.armpit_y = inseam + 0.272 * h * ratio;
        m.y.shoulder_y = inseam + 0.338 * h * ratio;
        m.y.neck_base_y = inseam + 0.375 * h * ratio;
        m.y.jaw_y = inseam + 0.413 * h * ratio;
        m.y.chin_y = inseam + 0.390 * h * ratio;
        m.y.eye_y = inseam + 0.456 * h * ratio;
        m.thigh_len = m.y.hip_y - m.y.knee_y;
        m.shank_len = m.y.knee_y - m.y.ankle_y;
    }
    m.y.arm_fold_y = m.y.waist_y - 0.02 * h;
    m.y.wrist_rest_y = m.y.hip_y - 0.19 * h;

    let mut c = defaults_for(m.sex, m.build);

    /* Soft-tissue offsets, in centimetres, from the difference between the requested muscle/fat
       index and the regression baseline. */
    let fat_delta = m.body_fat - 0.22;
    let mus_delta = m.muscle - 0.45;
    c.waist += fat_delta * 62.0;
    c.hip += fat_delta * 42.0;
    c.chest += fat_delta * 22.0 + mus_delta * 22.0;
    c.thigh += fat_delta * 20.0 + mus_delta * 14.0;
    c.calf += fat_delta * 8.0 + mus_delta * 8.0;
    c.bicep += mus_delta * 11.0 + fat_delta * 6.0;
    c.forearm += mus_delta * 5.0;
    c.neck += mus_delta * 5.0 + fat_delta * 7.0;

    let stature_scale = (h / default_height).powf(0.72);
    c.neck *= stature_scale;
    c.chest *= stature_scale;
    c.underbust *= stature_scale;
    c.waist *= stature_scale;
    c.hip *= stature_scale;
    c.thigh *= stature_scale;
    c.calf *= stature_scale;
    c.bicep *= stature_scale;
    c.forearm *= stature_scale;
    c.wrist *= stature_scale;

    /* Exact overrides from the caller. */
    if spec.neck_cm > 1.0 { c.neck = spec.neck_cm; }
    if spec.chest_cm > 1.0 { c.chest = spec.chest_cm; }
    if spec.underbust_cm > 1.0 { c.underbust = spec.underbust_cm; }
    if spec.waist_cm > 1.0 { c.waist = spec.waist_cm; }
    if spec.hip_cm > 1.0 { c.hip = spec.hip_cm; }
    if spec.thigh_cm > 1.0 { c.thigh = spec.thigh_cm; }
    if spec.calf_cm > 1.0 { c.calf = spec.calf_cm; }
    if spec.bicep_cm > 1.0 { c.bicep = spec.bicep_cm; }
    if spec.forearm_cm > 1.0 { c.forearm = spec.forearm_cm; }
    if spec.wrist_cm > 1.0 { c.wrist = spec.wrist_cm; }

    /* --- the centimetre -> metre boundary. Nothing above this line is metres. --- */
    c.neck *= 0.01;
    c.chest *= 0.01;
    c.underbust *= 0.01;
    c.waist *= 0.01;
    c.hip *= 0.01;
    c.thigh *= 0.01;
    c.calf *= 0.01;
    c.bicep *= 0.01;
    c.forearm *= 0.01;
    c.wrist *= 0.01;

    m.neck = solve_seg(c.neck, K_DEPTH_NECK, -0.004 * h);
    m.chest = solve_seg(c.chest, K_DEPTH_CHEST, 0.004 * h);
    m.bust_seg = if female { solve_seg(c.chest, K_DEPTH_BUST, 0.004 * h) } else { m.chest };
    m.underbust = solve_seg(c.underbust, K_DEPTH_BUST, 0.006 * h);
    m.waist = solve_seg(c.waist, K_DEPTH_WAIST, 0.004 * h);
    m.hip = solve_seg(c.hip, K_DEPTH_HIP, 0.004 * h);

    /* Shoulder width from chest girth rather than from build alone, so a slim person does not
       acquire a heavy person's shoulders. */
    let biacromial_cm = (if female { 36.6 } else { 41.5 })
        + (c.chest - (if female { 0.935 } else { 1.023 })) * 100.0 * 0.16
        + (h - default_height) * 100.0 * 0.10;
    m.biacromial_half = clampf(biacromial_cm / 100.0, 0.28, 0.60) * 0.5;
    m.arm_pit_half = m.chest.rx * 0.86;
    if spec.shoulder_width_cm > 1.0 {
        m.biacromial_half = spec.shoulder_width_cm / 200.0;
    }
    m.hip_half_width = (m.hip.rx * 0.52).max(m.biacromial_half * 0.44);
    m.leg_separation = m.hip_half_width * 0.86;

    let head_circumference_m = (if spec.head_circumference_cm > 1.0 {
        spec.head_circumference_cm
    } else {
        (if female { 55.0 } else { 57.0 }) * (h / default_height).powf(0.5)
    }) * 0.01;
    let head_radius_eq = head_circumference_m / (2.0 * K_PI);
    m.head.rx = head_radius_eq * 0.78;
    m.head.rz = head_radius_eq * 0.97;
    m.face_height = m.y.crown_y - m.y.chin_y;
    m.head.centre_z = 0.0;
    m.jaw.rx = m.head.rx * 0.86;
    m.jaw.rz = m.head.rz * 0.90;
    m.face_width = m.head.rx * 1.42;
    m.nose_length = (m.y.chin_y - m.y.eye_y) * 0.42;
    m.lip_width = m.face_width * 0.42;
    m.ear_length = (m.y.crown_y - m.y.chin_y) * 0.24;
    m.eye_separation = m.head.rx * 0.72;
    m.eye_radius = m.head.rx * 0.172;
    m.iris_radius = m.eye_radius * 0.52;
    m.mouth_y = lerpf(m.y.chin_y, m.y.eye_y, 0.30);
    m.eye_z = -m.head.rz * 0.78;
    m.mouth_width = m.lip_width * 1.15;
    m.bust_sep = m.bust_seg.rx * 0.62;

    let upper_arm_r = c.bicep / (2.0 * K_PI) * 0.92;
    m.upper_arm_r = upper_arm_r;
    m.elbow_r = upper_arm_r * 0.86;
    let forearm_r = c.forearm / (2.0 * K_PI) * 0.95;
    m.forearm_r = forearm_r;
    m.wrist_r = (c.wrist / (2.0 * K_PI) * 1.05).max(0.022);
    m.deltoid_r = upper_arm_r * 1.30;
    m.palm_width = m.hand_length * 0.44;
    m.palm_thickness = m.palm_width * 0.33;

    /* Finger spans: middle longest, then ring and index equal, then little, then thumb. */
    let finger_span = m.hand_length * 0.48;
    m.finger_length = [
        finger_span * 0.80,
        finger_span * 0.94,
        finger_span * 1.00,
        finger_span * 0.94,
        finger_span * 0.78,
    ];
    let base_finger_r = m.palm_width * 0.118;
    m.finger_radius = [
        base_finger_r * 1.18,
        base_finger_r * 1.00,
        base_finger_r * 1.02,
        base_finger_r * 0.95,
        base_finger_r * 0.84,
    ];

    let thigh_r = c.thigh / (2.0 * K_PI);
    m.thigh_rx = thigh_r * 1.03;
    m.thigh_rz = thigh_r * 0.90;
    m.knee_r = m.thigh_rx * 0.76;
    let calf_r = c.calf / (2.0 * K_PI);
    m.calf_rx = calf_r * 1.06;
    m.calf_rz = calf_r * 0.86;
    m.ankle_rx = m.wrist_r * 1.20;
    m.ankle_rz = m.ankle_rx * K_DEPTH_ANKLE;
    m.foot_height = 0.040 * h;
    m.foot_heel_z = -0.048 * h;
    m.toe_length = [
        m.foot_length * 0.20,
        m.foot_length * 0.17,
        m.foot_length * 0.155,
        m.foot_length * 0.135,
        m.foot_length * 0.110,
    ];
    for i in 0..5 {
        m.toe_radius[i] = m.foot_width * (0.062 - 0.005 * i as f32);
    }

    /* Soft-tissue bulges, derived from the excess over the regression baselines. These shape the
       field, not the skeleton, so they never change a tape measurement. */
    let muscle_excess = clampf((m.muscle - 0.45) / 0.55, -1.0, 1.0);
    let fat_excess = clampf((m.body_fat - 0.22) / 0.23, -1.0, 1.0);
    m.bicep_bulge = (m.upper_arm_r * (0.14 * muscle_excess + 0.02)).max(0.0);
    m.tricep_bulge = (m.upper_arm_r * (0.10 * muscle_excess)).max(0.0);
    m.brachioradialis_bulge = (m.forearm_r * (0.12 * muscle_excess)).max(0.0);
    m.quad_bulge = (m.thigh_rx * (0.16 * muscle_excess)).max(0.0);
    m.hamstring_bulge = (m.thigh_rz * (0.12 * muscle_excess)).max(0.0);
    m.adductor_bulge = (m.thigh_rx * (0.08 * muscle_excess)).max(0.0);
    m.gastroc_bulge = (m.calf_rx * (0.20 * muscle_excess)).max(0.0);
    m.tibialis_bulge = (m.calf_rx * (0.09 * muscle_excess)).max(0.0);
    m.pec_bulge = (m.chest.rz * (0.10 * muscle_excess + if female { 0.0 } else { 0.06 })).max(0.0);
    m.trapezius_bulge = (m.neck.rx * (0.55 * muscle_excess + 0.35)).max(0.0);
    m.lat_bulge = (m.chest.rx * (0.12 * muscle_excess)).max(0.0);
    m.ab_depth = (m.waist.rz * (0.06 * muscle_excess)).max(0.0);
    m.oblique_bulge = (m.waist.rx * (0.05 * muscle_excess)).max(0.0);
    m.glute_bulge = m.hip.rz * (0.16 + 0.10 * clampf(fat_excess, -0.5, 1.0) + if female { 0.06 } else { 0.0 });
    m.belly_bulge = m.waist.rz * fat_excess.max(0.0) * 0.55;

    /* Bust volume is derived from the difference between the bust and underbust girths, i.e. from
       breast tissue on top of the ribcage rather than from growing the ribcage itself. */
    if female {
        let rib_r = circum_to_radius(c.underbust, K_DEPTH_BUST);
        let bust_r = circum_to_radius(c.chest, K_DEPTH_BUST);
        m.bust = saturate(0.25 + (bust_r - rib_r) * 4.2);
    }

    /* Age drives skin micro-roughness and subsurface radius for the texture bake and the renderer. */
    let age_t = saturate((m.age - 16.0) / 54.0);
    m.roughness_skin = lerpf(0.34, 0.62, age_t) + if female { 0.02 } else { 0.0 };
    m.sss_radius = lerpf(0.0021, 0.0013, age_t);

    m
}

/// Reconstructed perimeter of a torso segment - what the harness measures against a tape value.
pub fn perimeter(s: &Segment) -> f32 {
    ellipse_perimeter(s.rx, s.rz)
}

/// Estimate of body-mass index from the solved proportions, for the plausibility report.
///
/// Cylindrical shells through the major segments; each limb is counted for both sides (once per
/// side means the radii pass through unscaled - scaling them here was a real bug that produced a
/// BMI of 13 for an average adult).
pub fn bmi(m: &BodyMetrics) -> f32 {
    let mut volume = 0.0_f32;
    let mut seg = |y0: f32, y1: f32, rx: f32, rz: f32| {
        let hh = (y1 - y0).abs();
        volume += K_PI * rx * rz * hh;
    };
    seg(m.y.crotch_y, m.y.hip_y, m.hip.rx, m.hip.rz);
    seg(m.y.hip_y, m.y.waist_y, 0.5 * (m.hip.rx + m.waist.rx), 0.5 * (m.hip.rz + m.waist.rz));
    seg(m.y.waist_y, m.y.bust_y, 0.5 * (m.waist.rx + m.chest.rx), 0.5 * (m.waist.rz + m.chest.rz));
    seg(m.y.bust_y, m.y.shoulder_y, m.chest.rx, m.chest.rz);
    seg(m.y.shoulder_y, m.y.chin_y, m.neck.rx, m.neck.rz);
    seg(m.y.chin_y, m.y.crown_y, m.head.rx, m.head.rz);
    for _ in 0..2 {
        seg(m.y.knee_y, m.y.hip_y, m.thigh_rx, m.thigh_rz);
        seg(m.y.ankle_y, m.y.knee_y, m.calf_rx, m.calf_rz);
        seg(m.y.shoulder_y - m.upper_arm_len, m.y.shoulder_y, m.upper_arm_r, m.upper_arm_r * 0.92);
        seg(m.y.shoulder_y - m.upper_arm_len - m.forearm_len, m.y.shoulder_y - m.upper_arm_len, m.forearm_r, m.forearm_r * 0.88);
        seg(m.y.sole_y, m.y.ankle_y * 1.6, m.foot_width * 0.5, m.foot_length * 0.42);
    }
    let mass_kg = volume * 985.0; /* mean body density, kg/m^3 */
    mass_kg / (m.height * m.height)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec(sex: i32, build: i32, height: f32) -> AfSpec {
        let mut s = AfSpec::defaulted();
        s.sex = sex;
        s.build = build;
        s.height_m = height;
        s
    }

    #[test]
    fn tape_measurements_are_honoured_exactly() {
        /* The harness asserts the same property against the C++: a requested waist appears in the
           solved cross-section to better than 0.2%. */
        let mut s = spec(AF_SEX_FEMALE, AF_BUILD_ATHLETIC, 1.70);
        s.waist_cm = 68.0;
        s.hip_cm = 96.0;
        let m = solve(&s);
        assert!((perimeter(&m.waist) - 0.68).abs() < 0.68 * 0.002,
                "waist perimeter {} m", perimeter(&m.waist));
        assert!((perimeter(&m.hip) - 0.96).abs() < 0.96 * 0.002,
                "hip perimeter {} m", perimeter(&m.hip));
    }

    #[test]
    fn stature_is_exact_and_landmarks_are_ordered() {
        for height in [1.52_f32, 1.70, 1.93] {
            let m = solve(&spec(AF_SEX_MALE, AF_BUILD_AVERAGE, height));
            assert!((m.height - height).abs() < 1e-6);
            assert!((m.y.crown_y - height).abs() < 1e-6);
            assert!(m.y.ankle_y < m.y.knee_y);
            assert!(m.y.knee_y < m.y.crotch_y);
            assert!(m.y.crotch_y < m.y.waist_y);
            assert!(m.y.waist_y < m.y.shoulder_y);
            assert!(m.y.shoulder_y < m.y.chin_y);
            assert!(m.y.chin_y < m.y.eye_y);
            assert!(m.y.eye_y < m.y.crown_y);
        }
    }

    #[test]
    fn bmi_is_plausible_for_every_build() {
        for sex in [AF_SEX_MALE, AF_SEX_FEMALE] {
            for build in [AF_BUILD_SLIM, AF_BUILD_AVERAGE, AF_BUILD_ATHLETIC, AF_BUILD_MUSCULAR, AF_BUILD_HEAVY] {
                let m = solve(&spec(sex, build, if sex == AF_SEX_FEMALE { 1.65 } else { 1.78 }));
                let v = bmi(&m);
                assert!((15.0..40.0).contains(&v), "sex {} build {} -> BMI {}", sex, build, v);
            }
        }
    }

    #[test]
    fn determinism_one_spec_one_body() {
        let s = spec(AF_SEX_FEMALE, AF_BUILD_SLIM, 1.62);
        assert_eq!(solve(&s), solve(&s));
    }
}
