#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod backend;

use serde_json::{json, Value};
use tauri::{Manager, State, WebviewWindow, WindowEvent};
use tauri_plugin_dialog::{DialogExt, FilePath};
use tauri_plugin_opener::OpenerExt;
use tokio::sync::oneshot;

use backend::Backend;

/// The window's one entry point. Most methods go straight to tools/backend.py; those that need a
/// file dialog or the system shell are handled here first.
#[tauri::command]
async fn call(window: WebviewWindow, backend: State<'_, Backend>, method: String, args: Vec<Value>) -> Result<Value, String> {
    if window.label() != "main" {
        return Err("Untrusted request.".into());
    }
    match method.as_str() {
        "pickOta" => match pick(&window, Pick::Ota).await {
            Some(mut paths) => backend.request("settings", vec![json!({ "ota": paths.remove(0) })]).await,
            None => Ok(Value::Null),
        },
        "pickApks" => match pick(&window, Pick::Apks).await {
            Some(paths) => backend.request("install", vec![Value::Array(paths)]).await,
            None => Ok(Value::Null),
        },
        // The backend names the folder or file (the logs, the reports, the last report).
        "open" => {
            let target = backend.request(&method, args).await?;
            let opener = window.app_handle().opener();
            if let Some(path) = target.get("openPath").and_then(Value::as_str) {
                opener.open_path(path, None::<&str>).map_err(|e| e.to_string())?;
            } else if let Some(path) = target.get("revealPath").and_then(Value::as_str) {
                opener.reveal_item_in_dir(path).map_err(|e| e.to_string())?;
            }
            Ok(Value::Null)
        }
        _ => backend.request(&method, args).await,
    }
}

enum Pick { Ota, Apks }

async fn pick(window: &WebviewWindow, kind: Pick) -> Option<Vec<Value>> {
    let (sender, receiver) = oneshot::channel::<Option<Vec<FilePath>>>();
    let dialog = window.dialog().file().set_parent(window);
    match kind {
        Pick::Ota => dialog.set_title("Your headset's full Horizon OS OTA").add_filter("OTA", &["zip", "bin"])
            .pick_file(|f| { let _ = sender.send(f.map(|f| vec![f])); }),
        Pick::Apks => dialog.set_title("Install apps on Horizon").add_filter("Android app", &["apk"])
            .pick_files(|f| { let _ = sender.send(f); }),
    }
    let files = receiver.await.ok()??;
    let paths: Vec<Value> = files.into_iter().filter_map(|f| f.into_path().ok())
        .map(|p| Value::String(p.to_string_lossy().into_owned())).collect();
    (!paths.is_empty()).then_some(paths)
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _, _| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.unminimize();
                let _ = window.set_focus();
            }
        }))
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            app.manage(Backend::start(app.handle().clone()));
            Ok(())
        })
        .on_window_event(|window, event| {
            if window.label() == "main" && matches!(event, WindowEvent::Destroyed) {
                window.state::<Backend>().shutdown();
            }
        })
        .invoke_handler(tauri::generate_handler![call])
        .run(tauri::generate_context!())
        .expect("error while running Prism");
}
