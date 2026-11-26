use af::{Engine, Spec};

#[test]
fn malformed_glb_bytes_are_rejected_and_do_not_create_geometry() {
    let mut engine = Engine::new(Spec::new()).expect("create engine with the native library");
    let before = engine.stats().expect("read initial stats");
    assert_eq!(before.vertex_count, 0);
    assert_eq!(before.triangle_count, 0);

    let result = engine.import_glb_bytes(b"not a GLB");
    assert!(result.is_err(), "malformed GLB bytes must be rejected");
    assert!(engine.last_error().starts_with("import:"));

    let after = engine.stats().expect("read stats after rejected import");
    assert_eq!(after.vertex_count, before.vertex_count);
    assert_eq!(after.triangle_count, before.triangle_count);
}
