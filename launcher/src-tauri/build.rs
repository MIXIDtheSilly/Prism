fn main() {
    // Only the launcher's own window may use its one command.
    tauri_build::try_build(tauri_build::Attributes::new().app_manifest(tauri_build::AppManifest::new().commands(&["call"])))
        .expect("failed to run tauri-build");
}
