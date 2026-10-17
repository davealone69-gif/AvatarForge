// ==============================================================================================
//  af-core :: tables.rs  --  the named data every implementation of the ABI must agree on
// ==============================================================================================
//
//  These three tables are the parts of the engine a *caller* addresses by name or index, and the
//  reason they live behind the ABI rather than in the Kotlin layer:
//
//    * clip ids          - the SDK, the renderer's playback clock and the .glb animation list all
//                          speak them; a reordering would silently play the wrong animation;
//    * blendshape ids    - ARKit's order, and glTF morph targets are positional, so the order is
//                          the wire format;
//    * region ids        - the material/submesh classification the inspector and the shaders use.
//
//  `tools/abi_conformance.py --reference` diffs all three against the reference implementation,
//  so a port cannot quietly disagree about what id 7 means.
// ==============================================================================================

use crate::abi::*;

/// Clip metadata. `duration <= 0` means "held pose, no authored track", which is how REST and the
/// three static poses are represented - the same convention as the C++ clip library.
pub struct ClipInfo {
    pub name: &'static str,
    pub duration: f32,
}

/* Indexed by af_clip id. REST is first because it is the id every "clear the pose" path uses. */
pub const CLIPS: [ClipInfo; AF_CLIP_COUNT as usize] = [
    ClipInfo { name: "rest", duration: 0.0 },
    ClipInfo { name: "idle", duration: 4.0 },
    ClipInfo { name: "breathe", duration: 5.0 },
    ClipInfo { name: "walk", duration: 1.1 },
    ClipInfo { name: "run", duration: 0.62 },
    ClipInfo { name: "jog", duration: 0.72 },
    ClipInfo { name: "squat", duration: 2.4 },
    ClipInfo { name: "jump", duration: 1.6 },
    ClipInfo { name: "wave", duration: 2.2 },
    ClipInfo { name: "punch", duration: 0.9 },
    ClipInfo { name: "kick", duration: 1.1 },
    ClipInfo { name: "t_pose", duration: 0.1 },
    ClipInfo { name: "a_pose", duration: 0.1 },
    ClipInfo { name: "sit", duration: 0.1 },
    ClipInfo { name: "turn_360", duration: 2.0 },
    ClipInfo { name: "pushup", duration: 2.0 },
];

pub fn clip_name(clip: i32) -> Option<&'static str> {
    if clip < 0 || clip >= AF_CLIP_COUNT {
        return None;
    }
    Some(CLIPS[clip as usize].name)
}

pub fn clip_duration(clip: i32) -> Option<f32> {
    if clip < 0 || clip >= AF_CLIP_COUNT {
        return None;
    }
    Some(CLIPS[clip as usize].duration)
}

/*
 * The 52 ARKit face channels, in ARKit's own order. This order is the .glb morph-target order and
 * the weight-vector order, so it is a wire format, not a preference.
 */
pub const BLENDSHAPES: [&str; 52] = [
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
    "cheekPuff", "cheekSquintLeft", "cheekSquintRight",
    "eyeBlinkLeft", "eyeBlinkRight", "eyeLookDownLeft", "eyeLookDownRight",
    "eyeLookInLeft", "eyeLookInRight", "eyeLookOutLeft", "eyeLookOutRight",
    "eyeLookUpLeft", "eyeLookUpRight", "eyeSquintLeft", "eyeSquintRight",
    "eyeWideLeft", "eyeWideRight",
    "jawForward", "jawLeft", "jawOpen", "jawRight",
    "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight",
    "mouthFunnel", "mouthLeft", "mouthLowerDownLeft", "mouthLowerDownRight",
    "mouthPressLeft", "mouthPressRight", "mouthPucker", "mouthRight",
    "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper",
    "mouthSmileLeft", "mouthSmileRight", "mouthStretchLeft", "mouthStretchRight",
    "mouthUpperUpLeft", "mouthUpperUpRight", "noseSneerLeft", "noseSneerRight",
    "tongueOut",
];

pub const BLENDSHAPE_COUNT: i32 = 52;

pub fn blendshape_index(name: &str) -> Option<i32> {
    BLENDSHAPES.iter().position(|n| *n == name).map(|i| i as i32)
}

/*
 * Body regions (the `af_region` ids in afcore.h, ordered exactly as `enum Region` in
 * native/src/af_mesh.h). The inspector shows them by name and the shaders switch their material
 * model on them: skin gets subsurface scattering, the two eyes get corneal refraction, hair and
 * lash get a fibre specular, cloth gets sheen, gear gets metal.
 *
 * The two eyes are separate regions rather than one "eye", and the tongue/lips/lash have their own
 * ids, because each carries a different material - an earlier table here said
 * "skin, eye, teeth, hair, nail, cloth_upper, cloth_lower, gear, shoe, tongue, gums, lashes,
 * accessory", which was simply wrong, and `tools/abi_conformance.py --reference` caught it by
 * diffing against the C++ implementation. This is what that tool is for.
 */
pub const REGIONS: [&str; AF_REGION_COUNT] = [
    "skin", "eye_left", "eye_right", "teeth", "tongue", "lips", "hair", "lash",
    "nail", "cloth_upper", "cloth_lower", "shoe", "gear",
];

/// Name for a region id. Out-of-range ids answer "skin", matching the C++ `regionLabel` (the
/// fallback exists because the renderer's region highlight accepts arbitrary ints from the SDK).
pub fn region_name(id: i32) -> &'static str {
    if id < 0 || id as usize >= AF_REGION_COUNT {
        return "skin";
    }
    REGIONS[id as usize]
}

/// Stage names, matching the af_stage enum; the SDK's progress screen labels stages with these.
pub fn stage_name(stage: i32) -> &'static str {
    match stage {
        AF_STAGE_IDLE => "idle",
        AF_STAGE_ANALYZE_INPUTS => "analyze inputs",
        AF_STAGE_FIT_ANATOMY => "fit anatomy",
        AF_STAGE_GEOMETRY => "geometry",
        AF_STAGE_UV_UNWRAP => "uv unwrap",
        AF_STAGE_BAKE_TEXTURES => "bake textures",
        AF_STAGE_RIG => "rig",
        AF_STAGE_BLENDSHAPES => "blendshapes",
        AF_STAGE_ANIMATION => "animation",
        AF_STAGE_EXPORT => "export",
        AF_STAGE_DONE => "done",
        _ => "unknown",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clip_table_covers_every_id_including_rest() {
        assert_eq!(CLIPS.len(), AF_CLIP_COUNT as usize);
        assert_eq!(clip_name(AF_CLIP_REST), Some("rest"));
        assert_eq!(clip_name(AF_CLIP_WALK), Some("walk"));
        assert_eq!(clip_name(AF_CLIP_PUSHUP), Some("pushup"));
        assert_eq!(clip_name(AF_CLIP_COUNT), None);
        assert_eq!(clip_name(-1), None);
        /* Every clip except REST animates; the three held poses are single-key clips. */
        for c in 1..AF_CLIP_COUNT {
            assert!(clip_duration(c).unwrap_or(0.0) > 0.0, "clip {} has a duration", c);
        }
        assert_eq!(clip_duration(AF_CLIP_REST), Some(0.0));
    }

    #[test]
    fn blendshape_table_is_arkit_order() {
        assert_eq!(BLENDSHAPES.len(), 52);
        assert_eq!(blendshape_index("browDownLeft"), Some(0));
        assert_eq!(blendshape_index("jawOpen"), Some(24));
        assert_eq!(blendshape_index("tongueOut"), Some(51));
        assert_eq!(blendshape_index("notAChannel"), None);
    }

    #[test]
    fn region_table_names_every_id_and_falls_back_like_the_reference() {
        assert_eq!(REGIONS.len(), AF_REGION_COUNT);
        assert_eq!(region_name(0), "skin");
        assert_eq!(region_name(3), "teeth");
        assert_eq!(region_name(9), "cloth_upper");
        assert_eq!(region_name(12), "gear");
        /* Out of range: the reference answers "skin" rather than an empty string, so a bad id
           drawn in the inspector still has a label. */
        assert_eq!(region_name(13), "skin");
        assert_eq!(region_name(-4), "skin");
    }
}
