// Runs tools/backend.py with Python and relays requests, responses and events over its stdin and
// stdout, a JSON message a line.
use std::collections::HashMap;
use std::fs::File;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};
use tauri::{AppHandle, Emitter};
use tokio::sync::oneshot;

type Reply = Result<Value, String>;
type Pending = Arc<Mutex<HashMap<u64, oneshot::Sender<Reply>>>>;

pub struct Backend {
    stdin: Mutex<Option<ChildStdin>>,
    child: Mutex<Option<Child>>,
    pending: Pending,
    next: AtomicU64,
    error: Option<String>,
}

/// Prism's checkout: PRISM_ROOT, else the first folder above the executable with tools/backend.py
/// (the launcher runs from launcher/src-tauri/target/...), else the source checkout it was built from.
pub fn prism_root() -> PathBuf {
    if let Some(dir) = std::env::var_os("PRISM_ROOT") {
        return PathBuf::from(dir);
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.ancestors().skip(1).find(|dir| dir.join("tools/backend.py").is_file()) {
            return dir.to_path_buf();
        }
    }
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

/// The Pythons to try, in order: PRISM_PYTHON, then python and the py launcher from PATH.
fn pythons() -> Vec<(String, Vec<&'static str>)> {
    let mut found = Vec::new();
    if let Some(python) = std::env::var_os("PRISM_PYTHON") {
        found.push((python.to_string_lossy().into_owned(), vec![]));
    }
    found.push(("python".into(), vec![]));
    found.push(("py".into(), vec!["-3"]));
    found
}

impl Backend {
    pub fn start(app: AppHandle) -> Self {
        let root = prism_root();
        let failed = |error: String| Self {
            stdin: Mutex::new(None), child: Mutex::new(None), pending: Arc::default(), next: AtomicU64::new(1), error: Some(error),
        };
        let logs = root.join("work").join("logs");
        let _ = std::fs::create_dir_all(&logs);
        let Ok(log) = File::create(logs.join("launcher-backend.log")) else {
            return failed(format!("Could not write {}", logs.join("launcher-backend.log").display()));
        };
        let mut started = None;
        for (python, args) in pythons() {
            let mut command = Command::new(&python);
            command
                .args(&args)
                .arg("-u")
                .arg(root.join("tools").join("backend.py"))
                .current_dir(&root)
                .env("PYTHONIOENCODING", "utf-8")
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .stderr(log.try_clone().map(Stdio::from).unwrap_or(Stdio::null()));
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                const CREATE_NO_WINDOW: u32 = 0x0800_0000;
                command.creation_flags(CREATE_NO_WINDOW);
            }
            if let Ok(child) = command.spawn() {
                started = Some(child);
                break;
            }
        }
        let Some(mut child) = started else {
            return failed("Prism needs Python 3.10 or later, and this PC has no python or py on PATH. \
                           Install it (winget install Python.Python.3.13) and open Prism again.".into());
        };
        let stdin = child.stdin.take();
        let Some(stdout) = child.stdout.take() else { return failed("The backend has no output".into()) };
        let pending: Pending = Arc::default();

        let reader_pending = pending.clone();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else { break };
                let Ok(message) = serde_json::from_str::<Value>(&line) else { continue };
                if let Some(id) = message.get("id").and_then(Value::as_u64) {
                    let reply = if message.get("ok").and_then(Value::as_bool) == Some(true) {
                        Ok(message.get("value").cloned().unwrap_or(Value::Null))
                    } else {
                        Err(message.get("error").and_then(Value::as_str).unwrap_or("The request failed.").to_string())
                    };
                    if let Some(sender) = reader_pending.lock().unwrap().remove(&id) {
                        let _ = sender.send(reply);
                    }
                    continue;
                }
                if let Some(name @ ("changed" | "output" | "logcat")) = message.get("event").and_then(Value::as_str) {
                    let _ = app.emit(&format!("prism://{name}"), message.get("payload"));
                }
            }
            for (_, sender) in reader_pending.lock().unwrap().drain() {
                let _ = sender.send(Err(STOPPED.into()));
            }
            let _ = app.emit("prism://backend-exit", ());
        });

        Self { stdin: Mutex::new(stdin), child: Mutex::new(Some(child)), pending, next: AtomicU64::new(1), error: None }
    }

    fn write(&self, message: &Value) -> Result<(), String> {
        let mut guard = self.stdin.lock().unwrap();
        let stdin = guard.as_mut().ok_or(STOPPED)?;
        writeln!(stdin, "{message}").and_then(|_| stdin.flush()).map_err(|_| STOPPED.to_string())
    }

    pub async fn request(&self, method: &str, args: Vec<Value>) -> Reply {
        if let Some(error) = &self.error {
            return Err(error.clone());
        }
        let id = self.next.fetch_add(1, Ordering::Relaxed);
        let (sender, receiver) = oneshot::channel();
        self.pending.lock().unwrap().insert(id, sender);
        if let Err(error) = self.write(&json!({ "id": id, "method": method, "args": args })) {
            self.pending.lock().unwrap().remove(&id);
            return Err(error);
        }
        receiver.await.unwrap_or_else(|_| Err(STOPPED.into()))
    }

    /// Closing its input ends the backend: a VR bridge it started quits with it, while a setup or a
    /// build runs on into its log, and the emulator keeps running.
    pub fn shutdown(&self) {
        self.stdin.lock().unwrap().take();
        if let Some(mut child) = self.child.lock().unwrap().take() {
            std::thread::spawn(move || {
                for _ in 0..50 {
                    if matches!(child.try_wait(), Ok(Some(_))) {
                        return;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(100));
                }
                let _ = child.kill();
            });
        }
    }
}

const STOPPED: &str = "Prism's backend stopped: work\\logs\\launcher-backend.log says why. Restart Prism.";
